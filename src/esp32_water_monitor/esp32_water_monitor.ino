#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <Adafruit_BME280.h>
#include <time.h>
#include <mbedtls/sha256.h>
#include "WaterMonitorCore.h"

// ============================================================================
// SISTEMA IOT DE BORDE: MONITOREO HIDROMETEOROLÓGICO Y ALERTA TEMPRANA
// Sabana Centro, Cundinamarca - Latitud: 4.81° N (2,600 m.s.n.m.)
// Challenge #2: Arquitectura Concurrente Dual-Core FreeRTOS (Core 0 / Core 1)
//   + WebServer (Core 0) + Autenticación SHA-256 + NTP + Histéresis
// Hardware: ESP32 DevKit v1 + HC-SR04 + BME280 + ILI9341 SPI + SD + Buzzer + LEDs
// ============================================================================

// Mapeo de Pines de Hardware
#define PIN_TRIG        4
#define PIN_ECHO        34
#if ENABLE_FLOW_SENSOR
#define PIN_FLOW_HALL   14
#endif
#define PIN_BUZZER      27
#define PIN_LED_RED     25
#define PIN_LED_GREEN   26
#define PIN_LED_BLUE    33
#define PIN_TFT_SCK     18
#define PIN_TFT_MISO    19
#define PIN_TFT_MOSI    23
#define PIN_TFT_CS      5
#define PIN_TFT_DC      16
#define PIN_TFT_RST     17
#define PIN_TFT_BL      32
#define PIN_SD_CS       13

// Constantes de Muestreo y Bus
constexpr int I2C_SDA_PIN = 21;
constexpr int I2C_SCL_PIN = 22;
constexpr unsigned long SAMPLE_PERIOD_MS = 2000;
constexpr unsigned long HISTORY_PERSIST_MS = 900000; // Persistir cada 15 min
constexpr uint32_t DEFAULT_SILENCE_SEC = 300;
constexpr float PRESSURE_UNAVAILABLE_HPA = -1.0f;

// Credenciales de Autenticación (SHA-256 hash de "sabana2026" con salt "SabanaCentro")
const char* AUTH_USERNAME = "admin";
const char* AUTH_SALT = "SabanaCentro";
const char* AUTH_PASSWORD_HASH = ""; 

// NTP para Día Juliano Real
constexpr long GMT_OFFSET_SEC = -5 * 3600; // UTC-5 Colombia
constexpr int DAYLIGHT_OFFSET_SEC = 0;
const char* NTP_SERVER = "pool.ntp.org";

// Configuración de Red: Default STA con Fallback Dinámico a SoftAP
const char* WIFI_STA_SSID = "Kapum A52";
const char* WIFI_STA_PASS = "Ivanna<3";
const char* WIFI_AP_SSID  = "AguaSabana-Nodo";
const char* WIFI_AP_PASS  = "sabana2026";
constexpr unsigned long WIFI_STA_TIMEOUT_MS = 10000;        // 10s ventana estricta de arranque

// Configuración MQTT: Árbol de tópicos agua/<nodo_id>/... hacia el Gateway Raspberry Pi
// Credenciales de broker Mosquitto cargadas por build flag (-D), nunca en texto plano en VCS.
#ifndef MQTT_NODE_ID
#define MQTT_NODE_ID "nodo01"
#endif
#ifndef MQTT_BROKER_HOST
#define MQTT_BROKER_HOST "192.168.25.1"
#endif
#ifndef MQTT_BROKER_PORT
#define MQTT_BROKER_PORT 1883
#endif
#ifndef MQTT_USERNAME
#define MQTT_USERNAME ""
#endif
#ifndef MQTT_PASSWORD
#define MQTT_PASSWORD ""
#endif
constexpr unsigned long MQTT_TELEMETRY_PERIOD_MS = 5000;   // Publicacion a Mosquitto cada 5 s
constexpr unsigned long WIFI_RECONNECT_INTERVAL_MS = 15000; // 15s intervalo de reintento en Core 0

// Estado de Conectividad Wi-Fi
bool apFallbackActive = false;
unsigned long lastStaRetryMillis = 0;

// Instancias de Periféricos y Servidor Web Local en puerto 80 (Core 0)
Adafruit_BME280 bme;
Adafruit_ILI9341 tft(PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
WebServer       server(80);

// Sincronización y Primitivas FreeRTOS
#if ENABLE_FLOW_SENSOR
static portMUX_TYPE flowMux = portMUX_INITIALIZER_UNLOCKED;
#endif
SemaphoreHandle_t telemetryMutex = NULL;
SemaphoreHandle_t spiMutex = NULL;
TaskHandle_t xWebServerTaskHandle = NULL;
TaskHandle_t xSensorTaskHandle = NULL;
TaskHandle_t xAnalyticsTaskHandle = NULL;
TaskHandle_t xHmiTaskHandle = NULL;
TaskHandle_t xMqttTaskHandle = NULL;

// Cliente MQTT (PubSubClient en tarea dedicada, LWT retenido)
WiFiClient mqttWifiClient;
PubSubClient mqttClient(mqttWifiClient);
char mqttClientId[32] = {};
char topicTelemetria[48] = {};
char topicAlerta[48] = {};
char topicEstado[48] = {};
char topicComando[48] = {};
char topicConfirmacion[48] = {};
struct AckMsg {
    char reqId[32];
    char source[12];
    bool muted;
};
QueueHandle_t mqttAckQueue = NULL;  // PubSubClient no es thread-safe: solo la tarea MQTT publica

// Buffer Circular en SRAM de 360 Muestras + Máquina de Histéresis
iot::watermonitor::SramRingBuffer sramRingBuffer;
iot::watermonitor::TelemetryRecord latestRecord{};
iot::watermonitor::RiskStateMachine riskStateMachine;

// Variables Volátiles de Interrupción
#if ENABLE_FLOW_SENSOR
volatile unsigned long pulseCounter = 0;
float currentFlowFactor = iot::watermonitor::FLOW_FACTOR;
#endif

// Variables de Estado Compartido
int manualRiskOverride = -1;
bool bmeAvailable = false;
uint8_t bmeAddress = 0x76;
bool ntpSynced = false;
bool echoSensorOk = true;
bool sdMounted = false;

// Perro Guardián de Silenciamiento Remoto
bool isAlarmSilenced = false;
unsigned long silenceStartMillis = 0;
unsigned long silenceDurationMillis = 0;

// Ventana Térmica 24h para Hargreaves-Samani (Tmax/Tmin reales)
float tempMin24h = 100.0f;
float tempMax24h = -100.0f;
unsigned long lastTempResetMillis = 0;
constexpr unsigned long TEMP_WINDOW_MS = 86400000UL;

// Autenticación: Sesión activa (máx 4 simultáneas)
struct SessionEntry {
    char token[65];
    unsigned long expiresAt;
    bool active;
};
constexpr int MAX_SESSIONS = 4;
SessionEntry sessions[MAX_SESSIONS] = {};
constexpr unsigned long SESSION_LIFETIME_MS = 900000;

// Consola Serial CLI
char cliBuffer[64];
uint8_t cliIndex = 0;

// Estructura y cola FreeRTOS para intercambio atómico sensor → analytics
struct RawAcquisitionData {
    float ambientTempC;
    float pressureHpa;
    float humidityPct;
    float waterLevelCm;
    unsigned long pulses;
    float elapsedSec;
};
QueueHandle_t rawSensorQueue = NULL;

char storedPasswordHash[65] = {};

// ============================================================================
// Utilidades de Criptografía
// ============================================================================
void sha256Hex(const char* input, size_t len, char* output) {
    uint8_t hash[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, (const unsigned char*)input, len);
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);
    for (int i = 0; i < 32; i++) {
        sprintf(output + i * 2, "%02x", hash[i]);
    }
    output[64] = '\0';
}

void computePasswordHash(const char* salt, const char* password, char* output) {
    char salted[128];
    snprintf(salted, sizeof(salted), "%s%s", salt, password);
    sha256Hex(salted, strlen(salted), output);
}

String generateSessionToken() {
    char seed[64];
    snprintf(seed, sizeof(seed), "%lu%lu%d", millis(), esp_random(), analogRead(0));
    char token[65];
    sha256Hex(seed, strlen(seed), token);
    return String(token);
}

int findSession(const String& token) {
    if (token.length() != 64) return -1;
    unsigned long now = millis();
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].active && token.equals(sessions[i].token)) {
            if ((long)(sessions[i].expiresAt - now) > 0) {
                sessions[i].expiresAt = now + SESSION_LIFETIME_MS;
                return i;
            }
            sessions[i].active = false;
            return -1;
        }
    }
    return -1;
}

String createSession() {
    String token = generateSessionToken();
    unsigned long now = millis();
    int slot = -1;
    unsigned long oldest = ULONG_MAX;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!sessions[i].active) { slot = i; break; }
        if (sessions[i].expiresAt < oldest) { oldest = sessions[i].expiresAt; slot = i; }
    }
    if (slot >= 0) {
        strncpy(sessions[slot].token, token.c_str(), 64);
        sessions[slot].token[64] = '\0';
        sessions[slot].expiresAt = now + SESSION_LIFETIME_MS;
        sessions[slot].active = true;
    }
    return token;
}

bool isAuthenticated() {
    if (!server.hasHeader("Cookie")) return false;
    String cookie = server.header("Cookie");
    int idx = cookie.indexOf("session=");
    if (idx < 0) return false;
    int end = cookie.indexOf(';', idx + 8);
    String token = (end < 0) ? cookie.substring(idx + 8) : cookie.substring(idx + 8, end);
    token.trim();
    return findSession(token) >= 0;
}

#if ENABLE_FLOW_SENSOR
void IRAM_ATTR flowPulseISR() {
    portENTER_CRITICAL_ISR(&flowMux);
    pulseCounter++;
    portEXIT_CRITICAL_ISR(&flowMux);
}
#endif

void recoverI2CBus(int sdaPin, int sclPin) {
    Wire.end();
    pinMode(sdaPin, INPUT_PULLUP);
    pinMode(sclPin, OUTPUT);
    digitalWrite(sclPin, HIGH);
    delayMicroseconds(5);
    for (int i = 0; i < 9; i++) {
        digitalWrite(sclPin, LOW); delayMicroseconds(5);
        digitalWrite(sclPin, HIGH); delayMicroseconds(5);
        if (digitalRead(sdaPin) == HIGH) break;
    }
    pinMode(sdaPin, OUTPUT);
    digitalWrite(sdaPin, LOW); delayMicroseconds(5);
    digitalWrite(sclPin, HIGH); delayMicroseconds(5);
    digitalWrite(sdaPin, HIGH); delayMicroseconds(5);
    Wire.begin(sdaPin, sclPin);
    Wire.setClock(100000);
}

bool detectAndInitBME280() {
    if (bme.begin(0x76, &Wire)) { bmeAddress = 0x76; bmeAvailable = true; }
    else if (bme.begin(0x77, &Wire)) { bmeAddress = 0x77; bmeAvailable = true; }
    else {
        recoverI2CBus(I2C_SDA_PIN, I2C_SCL_PIN);
        if (bme.begin(0x76, &Wire)) { bmeAddress = 0x76; bmeAvailable = true; }
        else if (bme.begin(0x77, &Wire)) { bmeAddress = 0x77; bmeAvailable = true; }
        else { bmeAvailable = false; }
    }
    return bmeAvailable;
}

void appendTelemetryToSd(const iot::watermonitor::TelemetryRecord& record) {
    if (!sdMounted || spiMutex == NULL ||
        xSemaphoreTake(spiMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    File file = SD.open("/telemetry.csv", FILE_APPEND);
    if (!file) {
        Serial.println("[WARN] No se pudo abrir /telemetry.csv en SD.");
        xSemaphoreGive(spiMutex);
        return;
    }
    float flowVal = 0.0f;
#if ENABLE_FLOW_SENSOR
    flowVal = record.flowLMin;
#endif
    file.printf("%lu,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%u\n",
                static_cast<unsigned long>(record.uptimeSec),
                record.levelCm, record.levelPercent, flowVal,
                record.dropRateCmH, record.tempC, record.pressureHpa,
                record.humidityPct, record.etoMmDay, record.riskState);
    file.close();
    xSemaphoreGive(spiMutex);
}

float readAcousticLevel(float ambientTempC, float previousLevelCm) {
    digitalWrite(PIN_TRIG, LOW); delayMicroseconds(2);
    digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(10);
    digitalWrite(PIN_TRIG, LOW);

    long durationUs = pulseIn(PIN_ECHO, HIGH, 25000L);
    float measured = iot::watermonitor::measureCorrectedLevel(ambientTempC, durationUs);

    if (measured < 0.0f) {
        echoSensorOk = false;
        return previousLevelCm;
    }
    echoSensorOk = true;
    return measured;
}

int getDayOfYear() {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 100)) {
        ntpSynced = true;
        return timeinfo.tm_yday + 1;
    }
    ntpSynced = false;
    return 180;
}

void initWiFiNetwork() {
    Serial.println("\n[WIFI] Modo Default: STA. Intentando conexion a infraestructura...");
    Serial.printf("[WIFI] SSID: '%s' (Timeout estricto: %lu s)...\n",
                  WIFI_STA_SSID, WIFI_STA_TIMEOUT_MS / 1000UL);

    tft.fillScreen(ILI9341_BLACK);
    tft.setCursor(8, 12);
    tft.setTextSize(2);
    tft.setTextColor(ILI9341_WHITE);
    tft.println("WiFi: Conectando");
    tft.println("Modo: STA");

    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false);
    WiFi.begin(WIFI_STA_SSID, WIFI_STA_PASS);

    unsigned long startAttempt = millis();
    bool connected = false;

    while (millis() - startAttempt < WIFI_STA_TIMEOUT_MS) {
        if (WiFi.status() == WL_CONNECTED) {
            connected = true;
            break;
        }
        delay(250);
        Serial.print(".");
    }
    Serial.println();

    if (connected) {
        apFallbackActive = false;
        Serial.printf("[OK] Conectado a STA exitosamente! IP: %s  RSSI: %d dBm\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
        tft.fillScreen(ILI9341_BLACK);
        tft.setCursor(8, 12);
        tft.setTextSize(2);
        tft.setTextColor(ILI9341_GREEN);
        tft.println("WiFi: STA");
        tft.setTextColor(ILI9341_WHITE);
        tft.println(WiFi.localIP());
        delay(1000);
    } else {
        apFallbackActive = true;
        Serial.println("[WARN] Falla estricta: Timeout de conexion STA agotado.");
        Serial.printf("[WARN] Activando Fallback SoftAP: '%s'...\n", WIFI_AP_SSID);

        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS);

        Serial.printf("[OK] SoftAP Fallback listo: %s  IP: %s\n",
                      WIFI_AP_SSID, WiFi.softAPIP().toString().c_str());

        tft.fillScreen(ILI9341_BLACK);
        tft.setCursor(8, 12);
        tft.setTextSize(2);
        tft.setTextColor(ILI9341_YELLOW);
        tft.println("WiFi: SoftAP");
        tft.setTextColor(ILI9341_WHITE);
        tft.println(WiFi.softAPIP());
        delay(1000);
    }
}

void manageWiFiConnectivity() {
    static unsigned long lastCheckMillis = 0;
    unsigned long now = millis();

    if (now - lastCheckMillis < 2000) return;
    lastCheckMillis = now;

    if (WiFi.status() == WL_CONNECTED) {
        if (apFallbackActive) {
            apFallbackActive = false;
            Serial.printf("[WIFI] Enlace STA restablecido dinamicamente. IP: %s  RSSI: %d dBm\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI());
            if (!ntpSynced) {
                configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
            }
        }
    } else {
        if (!apFallbackActive) {
            apFallbackActive = true;
            Serial.println("\n[WARN] Perdida de enlace STA. Activando Fallback SoftAP dinamicamente...");
            WiFi.mode(WIFI_AP_STA);
            WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS);
            lastStaRetryMillis = now;
        } else if (now - lastStaRetryMillis >= WIFI_RECONNECT_INTERVAL_MS) {
            lastStaRetryMillis = now;
            Serial.printf("[WIFI] Reintentando asociacion a red STA (%s)...\n", WIFI_STA_SSID);
            WiFi.begin(WIFI_STA_SSID, WIFI_STA_PASS);
        }
    }
}

// ============================================================================
// Dashboard Web (SPA PROGMEM)
// ============================================================================
const char HTML_LOGIN[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Login - Nodo IoT</title>
<style>
:root{--bg:#070d1e;--card:#131c36;--b:#263556;--t:#f1f5f9;--sub:#94a3b8;--g:#10b981}
*{box-sizing:border-box;margin:0;padding:0;font-family:system-ui,-apple-system,sans-serif}
body{background:var(--bg);color:var(--t);display:flex;align-items:center;justify-content:center;min-height:100vh}
.box{background:var(--card);padding:28px;border-radius:12px;border:1px solid var(--b);width:310px;text-align:center}
h2{font-size:1.1rem;margin-bottom:6px}.sub{font-size:.75rem;color:var(--sub);margin-bottom:16px}
input{width:100%;padding:10px;margin-bottom:10px;background:#1e293b;border:1px solid var(--b);color:#fff;border-radius:6px;font-size:.9rem}
button{width:100%;padding:10px;background:var(--g);color:#fff;border:none;border-radius:6px;font-weight:600;cursor:pointer}
button:active{transform:scale(.97)}
.err{color:#ef4444;font-size:.8rem;margin-top:10px;display:none}
</style>
</head>
<body>
<div class="box">
  <h2>NODO IOT SABANA CENTRO</h2>
  <div class="sub">Autenticación &bull; WLAN Municipal</div>
  <form onsubmit="return doLogin(event)">
    <input type="text" id="u" placeholder="Usuario" autocomplete="username" required>
    <input type="password" id="p" placeholder="Contraseña" autocomplete="current-password" required>
    <button type="submit">Ingresar</button>
  </form>
  <div id="err" class="err">Credenciales incorrectas</div>
</div>
<script>
async function doLogin(e){
  e.preventDefault();
  const r=await fetch('/api/login',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({user:document.getElementById('u').value,pass:document.getElementById('p').value})});
  if(r.ok)location.href='/';else document.getElementById('err').style.display='block';
  return false;
}
</script>
</body>
</html>
)rawliteral";

const char HTML_SPA_INDEX[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Nodo IoT Sabana Centro - Ch2</title>
<style>
:root{--bg:#070d1e;--card:#131c36;--b:#263556;--t:#f1f5f9;--sub:#94a3b8;--green:#10b981;--yellow:#f59e0b;--red:#ef4444}
*{box-sizing:border-box;margin:0;padding:0;font-family:system-ui,-apple-system,sans-serif}
body{background:var(--bg);color:var(--t);padding:12px;max-width:560px;margin:0 auto}
h1{font-size:1.15rem;text-align:center;letter-spacing:.5px}.sub{text-align:center;font-size:.75rem;color:var(--sub);margin-bottom:10px}
.badge{padding:9px;border-radius:8px;font-weight:700;text-align:center;font-size:.95rem;margin-bottom:10px;letter-spacing:.5px;transition:all .3s}
.b-norm{background:#064e3b;color:#a7f3d0}.b-stress{background:#1e3a8a;color:#bfdbfe}.b-alert{background:#78350f;color:#fde68a}.b-emerg{background:#7f1d1d;color:#fecaca;animation:pulse 1s infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.35}}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-bottom:10px}
.card{background:var(--card);padding:10px;border-radius:8px;border:1px solid var(--b)}
.card-t{font-size:.68rem;color:var(--sub);text-transform:uppercase;margin-bottom:2px}.card-v{font-size:1.25rem;font-weight:700}
.unit{font-size:.75rem;font-weight:400;color:var(--sub)}
.bar-wrap{background:#1e293b;border-radius:99px;height:8px;overflow:hidden;margin-top:5px}
.bar-fill{background:var(--green);height:100%;transition:width .4s}
.chart-box{background:var(--card);padding:10px;border-radius:8px;border:1px solid var(--b);margin-bottom:10px}
.chart-header{display:flex;justify-content:space-between;align-items:center;margin-bottom:6px;font-size:.75rem;color:var(--sub)}
canvas{width:100%;height:180px;background:#0b1329;border-radius:6px;display:block}
.btns{display:grid;grid-template-columns:repeat(4,1fr);gap:6px;margin-top:6px}
button{background:#1e293b;color:#fff;border:1px solid var(--b);padding:8px 4px;border-radius:6px;font-size:.75rem;font-weight:600;cursor:pointer}
button:active{transform:scale(.96)}
.btn-auto{background:#0284c7;grid-column:span 4;border:none}
.btn-silence{background:#d97706;grid-column:span 2;border:none}
.btn-unsilence{background:#475569;grid-column:span 2;border:none}
.btn-logout{background:#7f1d1d;grid-column:span 4;border:none;margin-top:6px}
.info-bar{font-size:.65rem;color:var(--sub);text-align:center;margin:6px 0;padding:4px;background:#0f172a;border-radius:4px}
.footer{text-align:center;font-size:.68rem;color:var(--sub);margin-top:10px}
select{background:#1e293b;color:#fff;border:1px solid var(--b);padding:4px 8px;border-radius:4px;font-size:.72rem}
</style>
</head>
<body>
<h1>NODO IOT SABANA CENTRO</h1>
<div class="sub">Challenge #2 &bull; Dual-Core FreeRTOS &bull; BME280 + ILI9341 &bull; Histéresis</div>
<div id="risk-badge" class="badge b-norm">ESTADO: NORMAL</div>
<div class="grid">
  <div class="card" style="grid-column:span 2">
    <div class="card-t">Nivel de Reservorio</div>
    <div class="card-v"><span id="lvl-cm">--</span> <span class="unit">cm</span> (<span id="lvl-pct">--</span>%)</div>
    <div class="bar-wrap"><div id="lvl-bar" class="bar-fill" style="width:0%"></div></div>
  </div>
  <div class="card">
    <div class="card-t">Tasa de Descenso</div>
    <div class="card-v"><span id="drop">--</span> <span class="unit">cm/h</span></div>
  </div>
  <div class="card">
    <div class="card-t">Temperatura (BME280)</div>
    <div class="card-v"><span id="temp">--</span> <span class="unit">&deg;C</span></div>
    <div class="card-t" style="margin-top:6px">Humedad Relativa</div>
    <div class="card-v"><span id="hum">--</span> <span class="unit">%HR</span></div>
  </div>
  <div class="card">
    <div class="card-t">Presión Atmosférica</div>
    <div class="card-v"><span id="press">--</span> <span class="unit">hPa</span></div>
  </div>
  <div class="card">
    <div class="card-t">ETo Hargreaves-Samani</div>
    <div class="card-v"><span id="eto">--</span> <span class="unit">mm/día</span></div>
  </div>
  <div class="card" style="grid-column:span 2">
    <div class="card-t">Diagnóstico y Salud de Hardware</div>
    <div style="display:flex;justify-content:space-between;font-size:.75rem;padding-top:4px;flex-wrap:wrap;gap:6px">
      <span>BME280: <b id="hw-bme">--</b></span>
      <span>Eco US: <b id="hw-echo">--</b></span>
      <span>MicroSD: <b id="hw-sd">--</b></span>
      <span>RAM: <b id="hw-ram">--</b></span>
      <span>WiFi: <b id="hw-rssi">--</b></span>
    </div>
  </div>
</div>
<div id="info-bar" class="info-bar">Uptime: -- | NTP: -- | Histéresis: activa</div>
<div class="chart-box">
  <div class="chart-header">
    <span>Histórico (Buffer SRAM)</span>
    <select id="chart-var" onchange="drawChart()">
      <option value="eto">Evaporación potencial (mm/día)</option>
      <option value="lp">Nivel (%)</option>
      <option value="tc">Temp (&deg;C)</option>
      <option value="h">Humedad (%)</option>
      <option value="dr">Descenso (cm/h)</option>
      <option value="p">Presión (hPa)</option>
    </select>
  </div>
  <canvas id="chart"></canvas>
</div>
<div class="btns">
  <button onclick="sendCmd('test 0')">Normal</button>
  <button onclick="sendCmd('test 1')">Estrés</button>
  <button onclick="sendCmd('test 2')">Alerta</button>
  <button onclick="sendCmd('test 3')">Emerg.</button>
  <button class="btn-auto" onclick="sendCmd('auto')">Modo Automático (Sensores)</button>
  <button class="btn-silence" onclick="sendCmd('silence 300')">Silenciar 5min</button>
  <button class="btn-unsilence" onclick="sendCmd('unsilence')">Reactivar</button>
  <button class="btn-logout" onclick="logout()">Cerrar Sesión</button>
</div>
<div class="footer">ESP32 Ch2 &bull; Core 0: WebServer &bull; Core 1: Fusión &bull; Histéresis 3/5</div>
<script>
const $=i=>document.getElementById(i);
let histData=[];
const BC=['#10b981','#38bdf8','#14b8a6','#f59e0b','#a855f7','#94a3b8'];
const VARS = ['eto', 'lp', 'tc', 'h', 'dr', 'p'];

function upd(){
  fetch('/api/data')
    .then(r=>{if(r.status===401){location.href='/login';return;}return r.ok?r.json():null;})
    .then(d=>{
      if(!d)return;
      $('lvl-cm').textContent=d.level_cm.toFixed(1);
      $('lvl-pct').textContent=d.level_pct.toFixed(0);
      $('lvl-bar').style.width=Math.max(0,Math.min(100,d.level_pct))+'%';
      $('lvl-bar').style.background=d.risk_state>=2?'var(--red)':d.risk_state==1?'var(--yellow)':'var(--green)';
      $('drop').textContent=d.drop_rate.toFixed(2);
      $('temp').textContent=d.temp_c.toFixed(1);
      $('hum').textContent=d.humidity_pct.toFixed(1);
      $('press').textContent=d.pressure_hpa<0?'N/D':d.pressure_hpa.toFixed(0);
      $('eto').textContent=d.eto_mm_day.toFixed(2);
      
      const b=$('risk-badge');
      const cls=['b-norm','b-stress','b-alert','b-emerg'];
      b.className='badge '+(cls[d.risk_state]||cls[0]);
      b.textContent='ESTADO: '+d.risk_label+(d.override>=0?' [MANUAL]':'')+(d.silenced?' [SILENCIADO]':'');
      
      $('info-bar').textContent=
        'Uptime: '+(d.uptime_s/60|0)+'min | NTP: '+(d.ntp_synced?'OK':'Fallback')+
        ' | Histéresis: '+(d.hysteresis_state>=0?'R'+d.hysteresis_state:'activa')+
        (d.silenced?' | Silencio: '+d.silence_s+'s':'');
        
      if(d.hw){
        const bmeOk=d.hw.sht;
        $('hw-bme').textContent=bmeOk?'ONLINE':'FALLO';
        $('hw-bme').style.color=bmeOk?'var(--green)':'var(--red)';
        $('hw-echo').textContent=d.hw.echo?'OK':'SIN ECO';
        $('hw-echo').style.color=d.hw.echo?'var(--green)':'var(--red)';
        $('hw-sd').textContent=d.hw.sd?'MONTADA':'N/A';
        $('hw-sd').style.color=d.hw.sd?'var(--green)':'var(--yellow)';
        $('hw-ram').textContent=d.hw.heap_kb+' KB';
        const w=$('hw-rssi');
        if(d.hw.wifi_sta){w.textContent='STA ('+d.hw.rssi+' dBm)';w.style.color='var(--green)';}
        else if(d.hw.ap_fallback){w.textContent='AP Fallback';w.style.color='var(--yellow)';}
        else{w.textContent='DESCONECT';w.style.color='var(--red)';}
      }
    })
    .catch(()=>{});
}

function loadHist(){
  fetch('/api/history')
    .then(r=>r.ok?r.json():null)
    .then(d=>{if(d&&d.records){histData=d.records;drawChart();}})
    .catch(()=>{});
}

function drawChart(){
  const c=$('chart'),ctx=c.getContext('2d');
  c.width=c.offsetWidth;c.height=180;
  ctx.fillStyle='#0b1329';ctx.fillRect(0,0,c.width,c.height);
  if(!histData.length)return;
  
  const k=$('chart-var').value;
  const vals=histData.map(r=>r[k]!==undefined?r[k]:0).filter(v=>k!=='p'||v>0);
  if(!vals.length)return;
  
  let mn=Math.min(...vals),mx=Math.max(...vals);
  if(mx===mn){mx+=1;mn=Math.max(0,mn-1);}
  const w=c.width,h=c.height,pad=25;
  const col=BC[VARS.indexOf(k)]||BC[0];
  
  ctx.strokeStyle=col;ctx.fillStyle=col;ctx.lineWidth=1.5;ctx.beginPath();
  for(let i=0;i<vals.length;i++){
    const x=pad+i*(w-pad*2)/(vals.length-1||1);
    const y=h-pad-(vals[i]-mn)/(mx-mn)*(h-pad*2);
    if(i===0)ctx.moveTo(x,y);else ctx.lineTo(x,y);
  }
  ctx.stroke();
  
  if(vals.length===1){
    const y=h-pad-(vals[0]-mn)/(mx-mn)*(h-pad*2);
    ctx.beginPath();ctx.arc(pad,y,3,0,6.28);ctx.fill();
  }
  
// Formateo de los ejes: 2 decimales para evapotranspiración (mm/día), 1 para el resto
  const dec = (k === 'eto') ? 2 : 1;
  ctx.fillStyle = '#94a3b8';
  ctx.font = '10px sans-serif';
  ctx.fillText(mx.toFixed(dec), 2, pad - 4);
  ctx.fillText(mn.toFixed(dec), 2, h - 6);
  ctx.fillText(vals.length + ' muestras', w - 70, h - 6);
}

function sendCmd(c){fetch('/api/cmd?c='+encodeURIComponent(c),{method:'POST'}).catch(()=>{});}
function logout(){fetch('/api/logout',{method:'POST'}).finally(()=>{location.href='/login';});}

setInterval(upd,2000);
setInterval(loadHist,30000);
upd();loadHist();
</script>
</body>
</html>
)rawliteral";

// ============================================================================
// Filtro de Seguridad por Subred
// ============================================================================
bool validateSubnetFilter() {
    WiFiClient client = server.client();
    IPAddress clientIP = client.remoteIP();
    IPAddress mask(255, 255, 255, 0);

    IPAddress apIP = WiFi.softAPIP();
    uint32_t cIp = static_cast<uint32_t>(clientIP);
    uint32_t apIpUint = static_cast<uint32_t>(apIP);
    uint32_t sMask = static_cast<uint32_t>(mask);

    if (apIpUint != 0 && iot::watermonitor::isSubnetAllowed(cIp, apIpUint, sMask)) {
        return true;
    }

    if (WiFi.status() == WL_CONNECTED) {
        IPAddress staIP = WiFi.localIP();
        IPAddress staMask = WiFi.subnetMask();
        uint32_t staIpUint = static_cast<uint32_t>(staIP);
        uint32_t staMaskUint = static_cast<uint32_t>(staMask);
        if (iot::watermonitor::isSubnetAllowed(cIp, staIpUint, staMaskUint)) {
            return true;
        }
    }

    server.send(403, "application/json", "{\"error\":\"Forbidden: Subnet mismatch\"}");
    return false;
}

// ============================================================================
// Manejadores HTTP (Core 0)
// ============================================================================
void processCLICommand(char* cmd);

void handleLogin() {
    if (!validateSubnetFilter()) return;
    server.send_P(200, "text/html", HTML_LOGIN);
}

void handleApiLogin() {
    if (!validateSubnetFilter()) return;

    String user = "";
    String pass = "";

    if (server.hasArg("user") && server.hasArg("pass")) {
        user = server.arg("user");
        pass = server.arg("pass");
    } else if (server.hasArg("plain")) {
        String body = server.arg("plain");
        int userIdx = body.indexOf("\"user\"");
        int passIdx = body.indexOf("\"pass\"");
        if (userIdx >= 0 && passIdx >= 0) {
            auto extractValue = [](const String& s, int startIdx) -> String {
                int colon = s.indexOf(':', startIdx);
                int q1 = s.indexOf('"', colon + 1);
                int q2 = s.indexOf('"', q1 + 1);
                if (q1 >= 0 && q2 > q1) return s.substring(q1 + 1, q2);
                return "";
            };
            user = extractValue(body, userIdx);
            pass = extractValue(body, passIdx);
        }
    }

    if (user.length() == 0 || pass.length() == 0 || user != AUTH_USERNAME) {
        server.send(401, "text/plain", "Unauthorized");
        return;
    }

    char inputHash[65];
    computePasswordHash(AUTH_SALT, pass.c_str(), inputHash);
    if (strcmp(inputHash, storedPasswordHash) != 0) {
        server.send(401, "text/plain", "Unauthorized");
        return;
    }

    String token = createSession();
    server.sendHeader("Set-Cookie", "session=" + token + "; HttpOnly; SameSite=Strict; Path=/; Max-Age=900");
    server.send(200, "text/plain", "OK");
}

void handleLogout() {
    server.sendHeader("Set-Cookie", "session=deleted; HttpOnly; Path=/; Max-Age=0");
    server.send(200, "text/plain", "OK");
}

void handleRoot() {
    if (!validateSubnetFilter()) return;
    if (!isAuthenticated()) {
        server.sendHeader("Location", "/login");
        server.send(302, "text/plain", "");
        return;
    }
    server.send_P(200, "text/html", HTML_SPA_INDEX);
}

void handleApiData() {
    if (!validateSubnetFilter()) return;
    if (!isAuthenticated()) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }

    iot::watermonitor::TelemetryRecord rec{};
    int currentOverride = -1;
    bool silenced = false;
    uint32_t silenceRemainingSec = 0;
    int hystState = -1;

    if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        rec = latestRecord;
        currentOverride = manualRiskOverride;
        silenced = isAlarmSilenced;
        hystState = riskStateMachine.stableState();
        if (isAlarmSilenced) {
            unsigned long elapsed = millis() - silenceStartMillis;
            if (elapsed < silenceDurationMillis) {
                silenceRemainingSec = (silenceDurationMillis - elapsed) / 1000UL;
            } else {
                isAlarmSilenced = false;
                silenceDurationMillis = 0;
                silenced = false;
            }
        }
        xSemaphoreGive(telemetryMutex);
    }

    int effectiveRisk = (currentOverride >= 0) ? currentOverride : rec.riskState;
    int rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
    uint32_t freeHeapKb = ESP.getFreeHeap() / 1024;
    bool staConnected = (WiFi.status() == WL_CONNECTED);

    char json[768];
    snprintf(json, sizeof(json),
        "{\"level_cm\":%.2f,\"level_pct\":%.1f,\"drop_rate\":%.2f,"
        "\"temp_c\":%.2f,\"pressure_hpa\":%.2f,\"humidity_pct\":%.2f,\"eto_mm_day\":%.2f,"
        "\"risk_state\":%d,\"risk_label\":\"%s\",\"override\":%d,"
        "\"silenced\":%s,\"silence_s\":%lu,\"uptime_s\":%lu,"
        "\"ntp_synced\":%s,\"hysteresis_state\":%d,"
        "\"hw\":{\"sht\":%s,\"echo\":%s,\"sd\":%s,\"heap_kb\":%u,\"rssi\":%d,\"wifi_sta\":%s,\"ap_fallback\":%s}}",
        rec.levelCm, rec.levelPercent, rec.dropRateCmH,
        rec.tempC, rec.pressureHpa, rec.humidityPct, rec.etoMmDay,
        effectiveRisk, iot::watermonitor::riskLabel(effectiveRisk),
        currentOverride, (silenced ? "true" : "false"),
        static_cast<unsigned long>(silenceRemainingSec),
        static_cast<unsigned long>(rec.uptimeSec),
        (ntpSynced ? "true" : "false"), hystState,
        (bmeAvailable ? "true" : "false"),
        (echoSensorOk ? "true" : "false"),
        (sdMounted ? "true" : "false"),
        (unsigned int)freeHeapKb, rssi,
        (staConnected ? "true" : "false"),
        (apFallbackActive ? "true" : "false")
    );
    server.send(200, "application/json", json);
}

void handleApiHistory() {
    if (!validateSubnetFilter()) return;
    if (!isAuthenticated()) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }

    static iot::watermonitor::TelemetryRecord snapshot[iot::watermonitor::RING_BUFFER_CAPACITY];
    size_t count = 0;
    if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        count = sramRingBuffer.size();
        for (size_t i = 0; i < count; i++) {
            sramRingBuffer.get(i, snapshot[i]);
        }
        xSemaphoreGive(telemetryMutex);
    }

    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "application/json", "");
    server.sendContent("{\"capacity\":360,\"records\":[");

    for (size_t i = 0; i < count; i++) {
        char itemJson[100];
        snprintf(itemJson, sizeof(itemJson),
            "%s{\"t\":%lu,\"eto\":%.2f,\"lp\":%.0f,\"dr\":%.2f,\"tc\":%.1f,\"h\":%.1f,\"p\":%.0f,\"r\":%u}",
            (i > 0 ? "," : ""),
            static_cast<unsigned long>(snapshot[i].uptimeSec),
            snapshot[i].etoMmDay, snapshot[i].levelPercent,
            snapshot[i].dropRateCmH, snapshot[i].tempC, snapshot[i].humidityPct, snapshot[i].pressureHpa, snapshot[i].riskState
        );
        server.sendContent(itemJson);
    }

    server.sendContent("]}");
    server.sendContent("");
}

void handleApiCmd() {
    if (!validateSubnetFilter()) return;
    if (!isAuthenticated()) {
        server.send(401, "text/plain", "Unauthorized");
        return;
    }
    if (server.hasArg("c")) {
        String cmd = server.arg("c");
        char cmdBuf[48];
        cmd.toCharArray(cmdBuf, sizeof(cmdBuf));
        processCLICommand(cmdBuf);
        server.send(200, "text/plain", "OK");
    } else {
        server.send(400, "text/plain", "Missing c parameter");
    }
}

void handleApiHealth() {
    if (!validateSubnetFilter()) return;
    server.send(200, "application/json", "{\"status\":\"ok\",\"challenge\":2}");
}

// ============================================================================
// Tarea FreeRTOS Core 0: Servidor Web HTTP
// ============================================================================
void Task_WebServer_HTTP(void* pvParameters) {
    (void)pvParameters;

    const char* headerKeys[] = {"Cookie"};
    server.collectHeaders(headerKeys, 1);

    server.on("/login", HTTP_GET, handleLogin);
    server.on("/api/login", HTTP_POST, handleApiLogin);
    server.on("/api/logout", HTTP_POST, handleLogout);
    server.on("/", HTTP_GET, handleRoot);
    server.on("/api/data", HTTP_GET, handleApiData);
    server.on("/api/history", HTTP_GET, handleApiHistory);
    server.on("/api/cmd", HTTP_POST, handleApiCmd);
    server.on("/api/health", HTTP_GET, handleApiHealth);
    server.begin();
    Serial.println("[OK] WebServer HTTP iniciado en puerto 80 (Core 0).");

    for (;;) {
        server.handleClient();
        manageWiFiConnectivity();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ============================================================================
// Tarea FreeRTOS Core 1: Adquisición de Sensores
// ============================================================================
void Task_SensorAcquisition(void* pvParameters) {
    (void)pvParameters;
    TickType_t lastWakeTime = xTaskGetTickCount();
    float prevLevel = iot::watermonitor::TANK_HEIGHT_CM;
    float lastValidTempC = 20.0f;
    float lastValidHumidityPct = 65.0f;
    float lastValidPressureHpa = PRESSURE_UNAVAILABLE_HPA;
    unsigned long lastAcqTime = millis();
    unsigned long lastBmeRetry = 0;

    for (;;) {
        unsigned long now = millis();
        float elapsedSec = (now - lastAcqTime) / 1000.0f;
        if (elapsedSec <= 0.0f) elapsedSec = 2.0f;
        lastAcqTime = now;

        unsigned long pulses = 0;
#if ENABLE_FLOW_SENSOR
        portENTER_CRITICAL(&flowMux);
        pulses = pulseCounter;
        pulseCounter = 0;
        portEXIT_CRITICAL(&flowMux);
#endif

        float ambientTempC = 20.0f;
        float pressureHpa = PRESSURE_UNAVAILABLE_HPA;
        float humidityPct = 65.0f;

        if (bmeAvailable) {
            ambientTempC = bme.readTemperature();
            humidityPct = bme.readHumidity();
            pressureHpa = bme.readPressure() / 100.0f;

            if (isnan(ambientTempC) || isnan(humidityPct) || isnan(pressureHpa) || ambientTempC < -40.0f || ambientTempC > 85.0f) {
                ambientTempC = lastValidTempC;
                humidityPct = lastValidHumidityPct;
                pressureHpa = lastValidPressureHpa;
                bmeAvailable = false;
                lastBmeRetry = now;
            } else {
                lastValidTempC = ambientTempC;
                lastValidHumidityPct = humidityPct;
                lastValidPressureHpa = pressureHpa;
            }
        } else if (now - lastBmeRetry > 3000) {
            lastBmeRetry = now;
            if (detectAndInitBME280()) {
                Serial.println("[OK] BME280 reconectado con éxito tras fallo.");
            }
        }

        float currentLevel = readAcousticLevel(ambientTempC, prevLevel);
        prevLevel = currentLevel;

        if (ambientTempC < tempMin24h) tempMin24h = ambientTempC;
        if (ambientTempC > tempMax24h) tempMax24h = ambientTempC;
        if (now - lastTempResetMillis > TEMP_WINDOW_MS) {
            lastTempResetMillis = now;
            tempMin24h = ambientTempC;
            tempMax24h = ambientTempC;
        }

        RawAcquisitionData raw{ambientTempC, pressureHpa, humidityPct, currentLevel, pulses, elapsedSec};
        xQueueOverwrite(rawSensorQueue, &raw);

        vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}

// ============================================================================
// Tarea FreeRTOS Core 1: Analítica de Borde
// ============================================================================
void Task_EdgeAnalytics(void* pvParameters) {
    (void)pvParameters;
    float previousWaterLevel = iot::watermonitor::TANK_HEIGHT_CM;
    bool isFirstAnalyticsRun = true;
    unsigned long lastSdPersist = 0;

    for (;;) {
        RawAcquisitionData data;
        xQueueReceive(rawSensorQueue, &data, portMAX_DELAY);

#if ENABLE_FLOW_SENSOR
        float flowFactor = currentFlowFactor;
        float flowLMin = iot::watermonitor::computeInstantFlowLMin(data.pulses, data.elapsedSec, flowFactor);
#endif

        float levelPercent = (data.waterLevelCm / iot::watermonitor::TANK_HEIGHT_CM) * 100.0f;
        if (isFirstAnalyticsRun) {
            previousWaterLevel = data.waterLevelCm;
            isFirstAnalyticsRun = false;
        }

        float dropRateCmH = (previousWaterLevel - data.waterLevelCm) * (3600.0f / data.elapsedSec);
        if (dropRateCmH < 0.0f) dropRateCmH = 0.0f;
        previousWaterLevel = data.waterLevelCm;

        int dayOfYear = getDayOfYear();
        float eto = iot::watermonitor::calculateHargreavesETo(dayOfYear, tempMin24h, tempMax24h);

#if ENABLE_FLOW_SENSOR
        int rawRisk = iot::watermonitor::evaluateRisk(levelPercent, dropRateCmH, eto, flowLMin);
#else
        int rawRisk = iot::watermonitor::evaluateRisk(levelPercent, dropRateCmH, eto);
#endif

        iot::watermonitor::TelemetryRecord rec{};
        rec.uptimeSec = millis() / 1000;
        rec.levelCm = data.waterLevelCm;
        rec.levelPercent = levelPercent;
#if ENABLE_FLOW_SENSOR
        rec.flowLMin = flowLMin;
#endif
        rec.dropRateCmH = dropRateCmH;
        rec.tempC = data.ambientTempC;
        rec.pressureHpa = data.pressureHpa;
        rec.humidityPct = data.humidityPct;
        rec.etoMmDay = eto;

        if (xSemaphoreTake(telemetryMutex, portMAX_DELAY) == pdTRUE) {
            rec.riskState = static_cast<uint8_t>(riskStateMachine.update(rawRisk));
            latestRecord = rec;
            sramRingBuffer.push(rec);
            xSemaphoreGive(telemetryMutex);
        }

        unsigned long now = millis();
        if (lastSdPersist == 0 || now - lastSdPersist >= HISTORY_PERSIST_MS) {
            appendTelemetryToSd(rec);
            lastSdPersist = now;
        }
    }
}

// ============================================================================
// Renderizado ILI9341 Anti-Flicker con Relleno Opaco (Zero Glitch)
// ============================================================================
static int lastRenderedRisk = -1;
static unsigned long lastTftRenderMillis = 0;
static bool isFirstTftRender = true;

void renderTftStatusPage(int effectiveRisk, const iot::watermonitor::TelemetryRecord& rec) {
    if (spiMutex == NULL || xSemaphoreTake(spiMutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }

    const unsigned long now = millis();
    if (now - lastTftRenderMillis < 250) {
        xSemaphoreGive(spiMutex);
        return;
    }
    lastTftRenderMillis = now;

    // Primer arranque: limpia pantalla y dibuja encabezado estático
    if (isFirstTftRender) {
        tft.fillScreen(ILI9341_BLACK);
        tft.setTextSize(2);
        tft.setTextColor(ILI9341_WHITE, ILI9341_BLACK);
        tft.setCursor(6, 5);
        tft.print("MONITOR HIDRICO");
        lastRenderedRisk = -1; // Forzar redibujado de banner
        isFirstTftRender = false;
    }

    const char* riskLabel = iot::watermonitor::riskLabel(effectiveRisk);
    const String ip = (WiFi.status() == WL_CONNECTED)
                        ? WiFi.localIP().toString()
                        : ((apFallbackActive || (WiFi.getMode() & WIFI_MODE_AP))
                           ? WiFi.softAPIP().toString() : "Conectando...");
    const char* buzzer = isAlarmSilenced ? "MUT" : (effectiveRisk >= 2 ? "ON" : "OFF");

    // 1. Diagnóstico de red y buzzer (Tamaño 1, con relleno opaco)
    tft.setTextSize(1);
    tft.setTextColor(ILI9341_WHITE, ILI9341_BLACK);
    tft.setCursor(6, 28);
    tft.printf("%s %-16s RSSI: %-4d", (WiFi.status() == WL_CONNECTED ? "IP:" : "AP:"), ip.c_str(), (WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0));
    tft.setCursor(6, 40);
    tft.printf("Buzzer: %-3s %s", buzzer, (manualRiskOverride >= 0 ? "[TEST MANUAL]   " : "                "));

    // 2. Banner de Riesgo: Dirty-Checking (Solo redibuja si cambia de estado)
    if (effectiveRisk != lastRenderedRisk) {
        lastRenderedRisk = effectiveRisk;
        const uint16_t riskColor = (effectiveRisk == 0) ? ILI9341_GREEN :
                                   (effectiveRisk == 1) ? ILI9341_CYAN :
                                   (effectiveRisk == 2) ? ILI9341_ORANGE : ILI9341_RED;

        tft.fillRoundRect(4, 56, 232, 38, 4, riskColor);
        tft.setTextColor(ILI9341_BLACK, riskColor);
        tft.setTextSize(2);
        tft.setCursor(12, 68);
        tft.printf("RIESGO: %-10s", riskLabel);
    }

    // 3. Telemetría Principal: Relleno Opaco Texto + Fondo Negro (Anti-Flicker)
    // El texto completo con espacios fijos sobreescribe los valores anteriores sin borrar la pantalla
    tft.setTextColor(ILI9341_WHITE, ILI9341_BLACK);
    tft.setTextSize(2);

    tft.setCursor(8, 110); tft.printf("Nivel:    %5.1f cm  ", rec.levelCm);
    tft.setCursor(8, 132); tft.printf("Llenado:  %5.1f %%   ", rec.levelPercent);
#if ENABLE_FLOW_SENSOR
    tft.setCursor(8, 154); tft.printf("Caudal:   %5.2f L/m ", rec.flowLMin);
#else
    tft.setCursor(8, 154); tft.print("Caudal:   N/D (Poda)  ");
#endif
    tft.setCursor(8, 176); tft.printf("Descenso: %5.1f cm/h", rec.dropRateCmH);
    tft.setCursor(8, 198); tft.printf("Temp:     %5.1f C   ", rec.tempC);
    tft.setCursor(8, 220); tft.printf("Hum:      %5.1f %%   ", rec.humidityPct);
    tft.setCursor(8, 242); tft.printf("ETo:      %5.2f mm/d", rec.etoMmDay);

    // 4. Barra Inferior de Diagnósticos (Tamaño 1)
    tft.setTextSize(1);
    tft.setCursor(8, 270);
    tft.printf("BME280: %-3s  SD: %-3s            ", (bmeAvailable ? "OK" : "N/A"), (sdMounted ? "OK" : "N/A"));
    
    tft.setCursor(8, 285);
    if (rec.pressureHpa < 0.0f) {
        tft.print("Presion: N/D                 ");
    } else {
        tft.printf("Presion: %-6.1f hPa          ", rec.pressureHpa);
    }

    tft.setCursor(8, 302);
    tft.printf("Uptime: %-6lu s  RAM: %-3u KB", static_cast<unsigned long>(rec.uptimeSec), (unsigned int)(ESP.getFreeHeap() / 1024));

    xSemaphoreGive(spiMutex);
}

// ============================================================================
// Tarea FreeRTOS Core 1: HMI Local
// ============================================================================
void Task_HMI_Local(void* pvParameters) {
    (void)pvParameters;

    for (;;) {
        unsigned long now = millis();

        int currentOverride = -1;
        int riskState = 0;
        iot::watermonitor::TelemetryRecord record{};
        if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            currentOverride = manualRiskOverride;
            riskState = latestRecord.riskState;
            record = latestRecord;
            xSemaphoreGive(telemetryMutex);
        }
        int effectiveRisk = (currentOverride >= 0) ? currentOverride : riskState;

        // Silencio antes de redibujar el TFT (SPI) para no sumar su latencia al apagado
        if (isAlarmSilenced || effectiveRisk < 2) {
            digitalWrite(PIN_BUZZER, LOW);
        }

        renderTftStatusPage(effectiveRisk, record);

        // Watchdog de Silenciamiento
        if (isAlarmSilenced) {
            if ((now - silenceStartMillis) >= silenceDurationMillis) {
                if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    isAlarmSilenced = false;
                    silenceDurationMillis = 0;
                    xSemaphoreGive(telemetryMutex);
                }
                Serial.println("\n[WATCHDOG] Periodo de silenciamiento finalizado. Buzzer reactivado.");
            }
        }

        // Actuación Óptica y Acústica In-Situ
        // Actuación Óptica y Acústica In-Situ
        switch (effectiveRisk) {
            case 0: // NORMAL: Solo Verde encendido, silencio
                digitalWrite(PIN_LED_RED, LOW);
                digitalWrite(PIN_LED_GREEN, HIGH);
                digitalWrite(PIN_LED_BLUE, LOW);
                digitalWrite(PIN_BUZZER, LOW); 
                break;

            case 1: // ESTRÉS MODERADO: Solo Azul encendido, silencio
                digitalWrite(PIN_LED_RED, LOW);
                digitalWrite(PIN_LED_GREEN, LOW);
                digitalWrite(PIN_LED_BLUE, HIGH);
                digitalWrite(PIN_BUZZER, LOW); 
                break;

            case 2: // ALERTA SEVERA: Solo Rojo fijo, beep intermitente a 1200 Hz
                digitalWrite(PIN_LED_RED, HIGH);
                digitalWrite(PIN_LED_GREEN, LOW);
                digitalWrite(PIN_LED_BLUE, LOW);
                if (!isAlarmSilenced && ((now % 1000) < 150)) {
                digitalWrite(PIN_BUZZER, HIGH); 
                } else {
                digitalWrite(PIN_BUZZER, LOW); 
                }
                break;

            case 3: {
                bool strobe = (now / 150) % 2;
                digitalWrite(PIN_LED_RED, strobe);
                digitalWrite(PIN_LED_GREEN, LOW);
                digitalWrite(PIN_LED_BLUE, !strobe);
                if (!isAlarmSilenced) {
                    digitalWrite(PIN_BUZZER, strobe); 
                } else {
                    digitalWrite(PIN_BUZZER, LOW); 
                }
                break;
            }
        }

        // Espera 50 ms o despierta de inmediato con xTaskNotifyGive (silenciar / reactivar alarma)
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
    }
}

// ============================================================================
// Silenciamiento de alarma y cliente MQTT (agua/<nodo_id>/...)
// ============================================================================
void queueAck(const char* reqId, const char* source, bool muted) {
    if (mqttAckQueue == NULL) return;
    AckMsg m{};
    strlcpy(m.reqId, reqId ? reqId : "", sizeof(m.reqId));
    strlcpy(m.source, (source && source[0]) ? source : "local", sizeof(m.source));
    m.muted = muted;
    xQueueSend(mqttAckQueue, &m, 0);
}

// Apaga el pin del buzzer desde el contexto llamador y despierta la tarea HMI.
bool setAlarmSilence(bool silence, uint32_t sec) {
    if (silence) digitalWrite(PIN_BUZZER, LOW);
    bool applied = false;
    if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        isAlarmSilenced = silence;
        silenceStartMillis = millis();
        silenceDurationMillis = silence ? sec * 1000UL : 0;
        xSemaphoreGive(telemetryMutex);
        applied = true;
    }
    if (xHmiTaskHandle != NULL) xTaskNotifyGive(xHmiTaskHandle);
    return applied;
}

static bool jsonGetString(const char* json, const char* key, char* out, size_t outLen) {
    char needle[24];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char* p = strstr(json, needle);
    if (!p) return false;
    p = strchr(p + strlen(needle), ':');
    if (!p) return false;
    p++;
    while (*p == ' ') p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < outLen - 1) out[i++] = *p++;
    out[i] = '\0';
    return true;
}

static long jsonGetNumber(const char* json, const char* key, long fallback) {
    char needle[24];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char* p = strstr(json, needle);
    if (!p) return fallback;
    p = strchr(p + strlen(needle), ':');
    if (!p) return fallback;
    return strtol(p + 1, NULL, 10);
}

void mqttOnMessage(char* topic, byte* payload, unsigned int length) {
    (void)topic;
    char buf[160];
    size_t n = (length < sizeof(buf) - 1) ? length : sizeof(buf) - 1;
    memcpy(buf, payload, n);
    buf[n] = '\0';

    char cmd[24] = {}, reqId[32] = {}, source[12] = {};
    if (!jsonGetString(buf, "cmd", cmd, sizeof(cmd))) return;
    jsonGetString(buf, "req_id", reqId, sizeof(reqId));
    jsonGetString(buf, "source", source, sizeof(source));

    if (strcmp(cmd, "SILENCE_ALARM") == 0) {
        long dur = jsonGetNumber(buf, "duration_sec", DEFAULT_SILENCE_SEC);
        if (dur < 1 || dur > 3600) dur = DEFAULT_SILENCE_SEC;
        bool ok = setAlarmSilence(true, static_cast<uint32_t>(dur));
        queueAck(reqId, source, ok);
    } else if (strcmp(cmd, "UNSILENCE_ALARM") == 0) {
        setAlarmSilence(false, 0);
        queueAck(reqId, source, false);
    }
}

static bool mqttConnectBroker() {
    const bool hasAuth = MQTT_USERNAME[0] != '\0';
    if (!mqttClient.connect(mqttClientId,
                            hasAuth ? MQTT_USERNAME : NULL,
                            hasAuth ? MQTT_PASSWORD : NULL,
                            topicEstado, 1, true, "{\"status\":\"offline\"}")) {
        return false;
    }
    mqttClient.subscribe(topicComando, 1);
    mqttClient.publish(topicEstado, "{\"status\":\"online\"}", true);
    return true;
}

void Task_MQTT(void* pvParameters) {
    (void)pvParameters;
    mqttClient.setServer(MQTT_BROKER_HOST, MQTT_BROKER_PORT);
    mqttClient.setBufferSize(512);
    mqttClient.setKeepAlive(15);
    mqttClient.setSocketTimeout(2);
    mqttClient.setCallback(mqttOnMessage);

    unsigned long lastTelemetry = 0;
    unsigned long lastAttempt = 0;
    int lastAlertRisk = -1;
    bool lastEchoOk = true;

    for (;;) {
        if (WiFi.status() != WL_CONNECTED) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (!mqttClient.connected()) {
            const unsigned long now = millis();
            if (lastAttempt == 0 || now - lastAttempt >= 3000) {
                lastAttempt = now;
                if (mqttConnectBroker()) {
                    Serial.println("[MQTT] Conectado al broker; LWT y suscripcion a comando activos.");
                    lastAlertRisk = -1;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        mqttClient.loop();

        AckMsg ack;
        while (xQueueReceive(mqttAckQueue, &ack, 0) == pdTRUE) {
            char json[128];
            snprintf(json, sizeof(json),
                     "{\"ack\":true,\"ack_confirmed\":true,\"buzzer_muted\":%s,\"req_id\":\"%s\",\"source\":\"%s\"}",
                     ack.muted ? "true" : "false", ack.reqId, ack.source);
            mqttClient.publish(topicConfirmacion, json, true);
        }

        iot::watermonitor::TelemetryRecord rec{};
        bool silenced = false;
        if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            rec = latestRecord;
            silenced = isAlarmSilenced;
            xSemaphoreGive(telemetryMutex);
        }

        const unsigned long now = millis();
        if (now - lastTelemetry >= MQTT_TELEMETRY_PERIOD_MS) {
            lastTelemetry = now;
            char ts[24] = "";
            time_t epoch = time(nullptr);
            if (epoch > 1700000000L) {
                struct tm g;
                gmtime_r(&epoch, &g);
                strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &g);
            }
            char json[320];
            snprintf(json, sizeof(json),
                "{\"timestamp\":\"%s\",\"level_cm\":%.2f,\"level_pct\":%.1f,\"drop_rate_cm_h\":%.2f,"
                "\"temp_c\":%.2f,\"humidity_pct\":%.1f,\"pressure_hpa\":%.1f,\"eto_mm_day\":%.2f,"
                "\"risk_state\":%u,\"alarm_silenced\":%s}",
                ts, rec.levelCm, rec.levelPercent, rec.dropRateCmH,
                rec.tempC, rec.humidityPct, rec.pressureHpa, rec.etoMmDay,
                static_cast<unsigned>(rec.riskState), silenced ? "true" : "false");
            mqttClient.publish(topicTelemetria, json, false);
        }

        if (static_cast<int>(rec.riskState) != lastAlertRisk) {
            char json[96];
            snprintf(json, sizeof(json), "{\"event\":\"RISK_TRANSITION\",\"risk_state\":%u,\"label\":\"%s\"}",
                     static_cast<unsigned>(rec.riskState), iot::watermonitor::riskLabel(rec.riskState));
            if (mqttClient.publish(topicAlerta, json, true)) {
                lastAlertRisk = static_cast<int>(rec.riskState);
            }
        }
        if (echoSensorOk != lastEchoOk) {
            mqttClient.publish(topicAlerta,
                echoSensorOk ? "{\"event\":\"LEVEL_SENSOR_OK\"}" : "{\"event\":\"LEVEL_SENSOR_ERROR\"}", true);
            lastEchoOk = echoSensorOk;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ============================================================================
// Consola CLI Embebida
// ============================================================================
void processCLICommand(char* cmd) {
    while (*cmd == ' ') cmd++;
    if (strlen(cmd) == 0) {
        Serial.print("SabanaNode> ");
        return;
    }

    if (strcmp(cmd, "help") == 0) {
        Serial.println("\n------------------------------------------------------------");
        Serial.println("  CONSOLA DE SERVICIO - NODO IOT SABANA CENTRO (CH2)        ");
        Serial.println("------------------------------------------------------------");
        Serial.println("  help            : Muestra este menu de comandos");
        Serial.println("  status          : Telemetria, memoria FreeRTOS y BME280/SD");
        Serial.println("  test <0-3>      : Fuerza estado de riesgo");
        Serial.println("  auto            : Retorna al modo automatico");
        Serial.println("  silence <seg>   : Silencia buzzer por N segundos (ACK < 50 ms)");
        Serial.println("  unsilence       : Reactiva buzzer inmediatamente");
#if ENABLE_FLOW_SENSOR
        Serial.println("  flow <factor>   : Ajustar factor caudalimetro");
#endif
        Serial.println("  ntp             : Muestra estado NTP y dia juliano");
        Serial.println("  mqtt            : Diagnostico de conexion al broker Mosquitto");
        Serial.println("  wifi            : Diagnostico de red (STA / Fallback SoftAP)");
        Serial.println("------------------------------------------------------------");
    }
    else if (strcmp(cmd, "status") == 0) {
        iot::watermonitor::TelemetryRecord rec{};
        if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            rec = latestRecord;
            xSemaphoreGive(telemetryMutex);
        }
        float flowVal = 0.0f;
#if ENABLE_FLOW_SENSOR
        flowVal = rec.flowLMin;
#endif
        Serial.printf("\n[STATUS] Nivel: %.1f cm (%.0f%%)  Caudal: %.2f L/min  Desc: %.2f cm/h\n",
                      rec.levelCm, rec.levelPercent, flowVal, rec.dropRateCmH);
        Serial.printf("[STATUS] Temp: %.1f C  Humedad: %.1f%%  Presion: %.0f hPa  ETo: %.2f mm/d\n",
                      rec.tempC, rec.humidityPct, rec.pressureHpa, rec.etoMmDay);
        Serial.printf("[STATUS] Riesgo: %s (%d)  Override: %d  Silenciado: %s\n",
                      iot::watermonitor::riskLabel(rec.riskState), rec.riskState,
                      manualRiskOverride, isAlarmSilenced ? "SI" : "NO");
        Serial.printf("[STATUS] Buffer historico: %d/%d  Uptime: %lu s  BME280: %s (0x%02X)  SD: %s\n",
                      (int)sramRingBuffer.size(), (int)sramRingBuffer.capacity(),
                      rec.uptimeSec, bmeAvailable ? "OK" : "N/A", bmeAddress,
                      sdMounted ? "OK" : "N/A");
        Serial.printf("[STATUS] Heap libre: %d bytes  NTP: %s  Dia: %d\n",
                      ESP.getFreeHeap(), ntpSynced ? "Sincronizado" : "Fallback", getDayOfYear());
    }
    else if (strncmp(cmd, "test ", 5) == 0) {
        int val = atoi(cmd + 5);
        if (val >= 0 && val <= 3) {
            manualRiskOverride = val;
            Serial.printf("[OK] Riesgo forzado a %d (%s).\n", val, iot::watermonitor::riskLabel(val));
        } else {
            Serial.println("[ERROR] Rango permitido: 0 a 3.");
        }
    }
    else if (strcmp(cmd, "auto") == 0) {
        manualRiskOverride = -1;
        Serial.println("[OK] Modo automatico sensorial reactivado.");
    }
    else if (strncmp(cmd, "silence ", 8) == 0) {
        uint32_t sec = atoi(cmd + 8);
        if (sec == 0) sec = DEFAULT_SILENCE_SEC;
        bool ok = setAlarmSilence(true, sec);
        queueAck("", "local", ok);
        Serial.printf("[OK] Buzzer silenciado por %lu segundos.\n", (unsigned long)sec);
    }
    else if (strcmp(cmd, "unsilence") == 0) {
        setAlarmSilence(false, 0);
        queueAck("", "local", false);
        Serial.println("[OK] Buzzer reactivado manualmente.");
    }
#if ENABLE_FLOW_SENSOR
    else if (strncmp(cmd, "flow ", 5) == 0) {
        float f = atof(cmd + 5);
        if (f >= 0.5f && f <= 50.0f) {
            currentFlowFactor = f;
            Serial.printf("[OK] Factor caudalimetro ajustado a %.2f.\n", f);
        } else {
            Serial.println("[ERROR] Factor fuera de rango (0.5 a 50.0).");
        }
    }
#endif
    else if (strcmp(cmd, "mqtt") == 0) {
        Serial.printf("[MQTT] Estado: %s  Broker: %s:%d\n",
                      mqttClient.connected() ? "CONECTADO" : "DESCONECTADO",
                      MQTT_BROKER_HOST, MQTT_BROKER_PORT);
        Serial.printf("[MQTT] Client ID: %s  Topico base: agua/%s/...\n", mqttClientId, MQTT_NODE_ID);
    }
    else if (strcmp(cmd, "ntp") == 0) {
        Serial.printf("[NTP] Sincronizado: %s  Dia Juliano: %d\n",
                      ntpSynced ? "SI" : "NO (fallback=180)", getDayOfYear());
        Serial.printf("[NTP] Tmin24h: %.1f C  Tmax24h: %.1f C\n", tempMin24h, tempMax24h);
    }
    else if (strcmp(cmd, "hysteresis") == 0) {
        int state = 0;
        if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            state = riskStateMachine.stableState();
            xSemaphoreGive(telemetryMutex);
        }
        Serial.printf("[HYSTERESIS] Estado estable: %d (%s)\n", state, iot::watermonitor::riskLabel(state));
        Serial.printf("[HYSTERESIS] Umbral escalamiento: %d muestras  Des-escalamiento: %d muestras\n",
                      iot::watermonitor::RiskStateMachine::ESCALATE_THRESHOLD,
                      iot::watermonitor::RiskStateMachine::DEESCALATE_THRESHOLD);
    }
    else if (strcmp(cmd, "wifi") == 0) {
        bool staConn = (WiFi.status() == WL_CONNECTED);
        Serial.println("\n------------------------------------------------------------");
        Serial.println("  ESTADO DE CONECTIVIDAD WI-FI (DEFAULT STA / FALLBACK AP)  ");
        Serial.println("------------------------------------------------------------");
        Serial.printf("  Modo Wi-Fi Actual    : %s\n",
                      staConn ? (apFallbackActive ? "STA + SoftAP (Fallback)" : "STA (Estacion)")
                              : (apFallbackActive ? "SoftAP (Fallback Activo)" : "STA (Desconectado)"));
        Serial.printf("  Estado STA           : %s\n", staConn ? "CONECTADO" : "DESCONECTADO");
        if (staConn) {
            Serial.printf("  STA SSID             : %s\n", WiFi.SSID().c_str());
            Serial.printf("  STA IP               : %s\n", WiFi.localIP().toString().c_str());
            Serial.printf("  STA Mascara          : %s\n", WiFi.subnetMask().toString().c_str());
            Serial.printf("  STA Gateway          : %s\n", WiFi.gatewayIP().toString().c_str());
            Serial.printf("  STA RSSI             : %d dBm\n", WiFi.RSSI());
        }
        Serial.printf("  Fallback SoftAP      : %s\n", apFallbackActive ? "ACTIVO" : "INACTIVO");
        if (apFallbackActive || (WiFi.getMode() & WIFI_MODE_AP)) {
            Serial.printf("  SoftAP SSID          : %s\n", WIFI_AP_SSID);
            Serial.printf("  SoftAP IP            : %s\n", WiFi.softAPIP().toString().c_str());
            Serial.printf("  Clientes Conectados  : %d\n", WiFi.softAPgetStationNum());
        }
        Serial.println("------------------------------------------------------------");
    }
    else {
        Serial.printf("[ERROR] Comando desconocido: '%s'. Escribe 'help'.\n", cmd);
    }
    Serial.print("SabanaNode> ");
}

void handleSerialCLI() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (cliIndex > 0) {
                cliBuffer[cliIndex] = '\0';
                Serial.println();
                processCLICommand(cliBuffer);
                cliIndex = 0;
            } else {
                Serial.print("\nSabanaNode> ");
            }
        } else if (c == '\b' || c == 127) {
            if (cliIndex > 0) {
                cliIndex--;
                Serial.print("\b \b");
            }
        } else if (cliIndex < sizeof(cliBuffer) - 1) {
            cliBuffer[cliIndex++] = c;
            Serial.write(c);
        }
    }
}

// ============================================================================
// SETUP
// ============================================================================
void setup() {
    Serial.begin(115200);
    delay(200);

    Serial.println("\n========================================================");
    Serial.println(" NODO IOT SABANA CENTRO - CHALLENGE #2                  ");
    Serial.println(" Arquitectura Dual-Core FreeRTOS + WebServer (Core 0)  ");
    Serial.println(" BME280 + ILI9341 + SD + Histeresis + Auth SHA-256 + NTP");
    Serial.println("========================================================");

    pinMode(PIN_TRIG, OUTPUT);
    pinMode(PIN_ECHO, INPUT);
#if ENABLE_FLOW_SENSOR
    pinMode(PIN_FLOW_HALL, INPUT_PULLUP);
#endif
    pinMode(PIN_BUZZER, OUTPUT);
    pinMode(PIN_LED_RED, OUTPUT);
    pinMode(PIN_LED_GREEN, OUTPUT);
    pinMode(PIN_LED_BLUE, OUTPUT);

    digitalWrite(PIN_LED_RED, LOW);
    digitalWrite(PIN_LED_GREEN, LOW);
    digitalWrite(PIN_LED_BLUE, LOW);
    noTone(PIN_BUZZER);

#if ENABLE_FLOW_SENSOR
    attachInterrupt(digitalPinToInterrupt(PIN_FLOW_HALL), flowPulseISR, FALLING);
#endif

    pinMode(PIN_TFT_BL, OUTPUT);
    digitalWrite(PIN_TFT_BL, HIGH);
    pinMode(PIN_TFT_CS, OUTPUT);
    pinMode(PIN_SD_CS, OUTPUT);
    digitalWrite(PIN_TFT_CS, HIGH);
    digitalWrite(PIN_SD_CS, HIGH);

    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setClock(100000);
    Wire.setTimeOut(25);
    SPI.begin(PIN_TFT_SCK, PIN_TFT_MISO, PIN_TFT_MOSI, PIN_TFT_CS);

    tft.begin();
    tft.setRotation(0);
    tft.fillScreen(ILI9341_BLACK);
    tft.setTextColor(ILI9341_WHITE);
    tft.setTextSize(2);
    tft.setCursor(8, 12);
    tft.println("MONITOR HIDRICO");
    tft.println("INICIANDO...");

    if (detectAndInitBME280()) {
        Serial.printf("[OK] Sensor BME280 detectado en 0x%02X.\n", bmeAddress);
    } else {
        Serial.println("[WARN] BME280 no detectado. Valores de respaldo.");
    }

    if (SD.begin(PIN_SD_CS, SPI)) {
        sdMounted = true;
        if (!SD.exists("/telemetry.csv")) {
            File file = SD.open("/telemetry.csv", FILE_WRITE);
            if (file) {
                file.println("uptime_s,level_cm,level_pct,flow_l_min,drop_cm_h,temp_c,pressure_hpa,humidity_pct,eto_mm_day,risk");
                file.close();
            }
        }
        Serial.println("[OK] SD montada; persistencia CSV habilitada.");
    } else {
        Serial.println("[WARN] SD no disponible; se conserva SRAM como fuente de tiempo real.");
    }

    computePasswordHash(AUTH_SALT, "sabana2026", storedPasswordHash);
    Serial.println("[OK] Hash SHA-256 de credenciales calculado.");

    delay(500);

    initWiFiNetwork();

    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
    Serial.println("[OK] NTP configurado (UTC-5 Colombia).");

    lastTempResetMillis = millis();

    telemetryMutex = xSemaphoreCreateMutex();
    spiMutex = xSemaphoreCreateMutex();
    rawSensorQueue = xQueueCreate(1, sizeof(RawAcquisitionData));
    mqttAckQueue = xQueueCreate(4, sizeof(AckMsg));

    snprintf(mqttClientId, sizeof(mqttClientId), "sabana-%s", MQTT_NODE_ID);
    snprintf(topicTelemetria, sizeof(topicTelemetria), "agua/%s/telemetria", MQTT_NODE_ID);
    snprintf(topicAlerta, sizeof(topicAlerta), "agua/%s/alerta", MQTT_NODE_ID);
    snprintf(topicEstado, sizeof(topicEstado), "agua/%s/estado", MQTT_NODE_ID);
    snprintf(topicComando, sizeof(topicComando), "agua/%s/comando", MQTT_NODE_ID);
    snprintf(topicConfirmacion, sizeof(topicConfirmacion), "agua/%s/confirmacion", MQTT_NODE_ID);

    if (telemetryMutex != NULL) {
        latestRecord.uptimeSec = 0;
        latestRecord.levelCm = iot::watermonitor::TANK_HEIGHT_CM;
        latestRecord.levelPercent = 100.0f;
#if ENABLE_FLOW_SENSOR
        latestRecord.flowLMin = 0.0f;
#endif
        latestRecord.dropRateCmH = 0.0f;
        latestRecord.tempC = 20.0f;
        latestRecord.pressureHpa = PRESSURE_UNAVAILABLE_HPA;
        latestRecord.humidityPct = 65.0f;
        latestRecord.etoMmDay = 3.8f;
        latestRecord.riskState = 0;
    }

    xTaskCreatePinnedToCore(Task_WebServer_HTTP, "WebServerTask", 8192, NULL, 1, &xWebServerTaskHandle, 0);
    xTaskCreatePinnedToCore(Task_EdgeAnalytics, "AnalyticsTask", 4096, NULL, 2, &xAnalyticsTaskHandle, 1);
    xTaskCreatePinnedToCore(Task_SensorAcquisition, "SensorTask", 4096, NULL, 2, &xSensorTaskHandle, 1);
    xTaskCreatePinnedToCore(Task_HMI_Local, "HmiTask", 4096, NULL, 1, &xHmiTaskHandle, 1);
    xTaskCreatePinnedToCore(Task_MQTT, "MqttTask", 6144, NULL, 1, &xMqttTaskHandle, 1);

    Serial.println("[OK] Tareas FreeRTOS creadas en Core 0 y Core 1.");
    Serial.println("[OK] Sistema listo. Escribe 'help' para comandos.");
    Serial.print("SabanaNode> ");
}

// ============================================================================
// LOOP PRINCIPAL
// ============================================================================
void loop() {
    handleSerialCLI();
    vTaskDelay(pdMS_TO_TICKS(50));
}