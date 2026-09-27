#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <Adafruit_SHT31.h>
#include <LittleFS.h>
#include <time.h>
#include <mbedtls/sha256.h>
#include "WaterMonitorCore.h"

// ============================================================================
// SISTEMA IOT DE BORDE: MONITOREO HIDROMETEOROLÓGICO Y ALERTA TEMPRANA
// Sabana Centro, Cundinamarca - Latitud: 4.81° N (2,600 m.s.n.m.)
// Challenge #2: Arquitectura Concurrente Dual-Core FreeRTOS (Core 0 / Core 1)
//   + WebServer (Core 0) + Autenticación SHA-256 + LittleFS + NTP + Histéresis
// Hardware: ESP32 DevKit v1 + HC-SR04 + SHT30 + ILI9341 SPI + SD + Buzzer + LEDs
// ============================================================================

// Mapeo de Pines de Hardware
#define PIN_TRIG        4
#define PIN_ECHO        34
#define PIN_FLOW_HALL   14
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
// Pre-computed: SHA256("SabanaCentro" + "sabana2026") — se verifica en runtime
const char* AUTH_SALT = "SabanaCentro";
const char* AUTH_PASSWORD_HASH = ""; // Se calcula al arrancar para comparación

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
constexpr unsigned long WIFI_RECONNECT_INTERVAL_MS = 15000; // 15s intervalo de reintento en Core 0

// Estado de Conectividad Wi-Fi
bool apFallbackActive = false;
unsigned long lastStaRetryMillis = 0;

// Instancias de Periféricos y Servidor Web Local en puerto 80 (Core 0)
Adafruit_SHT31  sht30 = Adafruit_SHT31();
Adafruit_ILI9341 tft(PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
WebServer       server(80);

// Sincronización y Primitivas FreeRTOS
static portMUX_TYPE flowMux = portMUX_INITIALIZER_UNLOCKED;
SemaphoreHandle_t telemetryMutex = NULL;
SemaphoreHandle_t spiMutex = NULL;
TaskHandle_t xWebServerTaskHandle = NULL;
TaskHandle_t xSensorTaskHandle = NULL;
TaskHandle_t xAnalyticsTaskHandle = NULL;
TaskHandle_t xHmiTaskHandle = NULL;

// Buffer Circular en SRAM de 360 Muestras + Máquina de Histéresis
iot::watermonitor::SramRingBuffer sramRingBuffer;
iot::watermonitor::TelemetryRecord latestRecord{};
iot::watermonitor::RiskStateMachine riskStateMachine;

// Variables Volátiles de Interrupción
volatile unsigned long pulseCounter = 0;

// Variables de Estado Compartido
float currentFlowFactor = iot::watermonitor::FLOW_FACTOR;
int manualRiskOverride = -1;
bool sht30Available = false;
uint8_t sht30Address = 0x44;
bool ntpSynced = false;
bool echoSensorOk = true;
bool littleFsMounted = false;
bool sdMounted = false;
File telemetryLog;

// Perro Guardián de Silenciamiento Remoto
bool isAlarmSilenced = false;
unsigned long silenceStartMillis = 0;
unsigned long silenceDurationMillis = 0;

// Ventana Térmica 24h para Hargreaves-Samani (Tmax/Tmin reales)
float tempMin24h = 100.0f;   // Se inicializa alto para que la primera lectura lo establezca
float tempMax24h = -100.0f;  // Se inicializa bajo para que la primera lectura lo establezca
unsigned long lastTempResetMillis = 0;
constexpr unsigned long TEMP_WINDOW_MS = 86400000UL; // 24h en ms

// Autenticación: Sesión activa (máx 4 simultáneas)
struct SessionEntry {
    char token[65]; // SHA-256 hex = 64 chars + null
    unsigned long expiresAt; // millis() de expiración
    bool active;
};
constexpr int MAX_SESSIONS = 4;
SessionEntry sessions[MAX_SESSIONS] = {};
constexpr unsigned long SESSION_LIFETIME_MS = 900000; // 15 min

// Consola Serial CLI
char cliBuffer[64];
uint8_t cliIndex = 0;

// Estructura para intercambio sensor → analytics
struct RawAcquisitionData {
    float ambientTempC;
    float pressureHpa;
    float humidityPct;
    float waterLevelCm;
    unsigned long pulses;
    float elapsedSec;
} rawSensorData;

// Hash SHA-256 pre-computado del password para comparación
char storedPasswordHash[65] = {};

// ============================================================================
// Utilidades de Criptografía (mbedTLS ya incluido en ESP-IDF)
// ============================================================================
void sha256Hex(const char* input, size_t len, char* output) {
    uint8_t hash[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0); // 0 = SHA-256
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

// ============================================================================
// Gestión de Sesiones
// ============================================================================
int findSession(const String& token) {
    unsigned long now = millis();
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].active && strcmp(sessions[i].token, token.c_str()) == 0) {
            if (now < sessions[i].expiresAt || (now - sessions[i].expiresAt) > 0x7FFFFFFFUL) {
                // Renovar sesión en cada request
                sessions[i].expiresAt = now + SESSION_LIFETIME_MS;
                return i;
            } else {
                sessions[i].active = false;
                return -1;
            }
        }
    }
    return -1;
}

String createSession() {
    String token = generateSessionToken();
    unsigned long now = millis();
    // Buscar slot libre o el más antiguo
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

bool isValidSessionTokenFormat(const String& token) {
    if (token.length() != 64) return false;
    for (size_t i = 0; i < token.length(); i++) {
        char c = token[i];
        bool isHex = (c >= '0' && c <= '9') ||
                     (c >= 'a' && c <= 'f') ||
                     (c >= 'A' && c <= 'F');
        if (!isHex) return false;
    }
    return true;
}

bool isAuthenticated() {
    if (!server.hasHeader("Cookie")) return false;

    String cookieHeader = server.header("Cookie");
    int start = 0;
    while (start < cookieHeader.length()) {
        int end = cookieHeader.indexOf(';', start);
        if (end < 0) end = cookieHeader.length();

        String pair = cookieHeader.substring(start, end);
        pair.trim();
        int equals = pair.indexOf('=');
        if (equals > 0 && pair.substring(0, equals) == "session") {
            String token = pair.substring(equals + 1);
            token.trim();
            return isValidSessionTokenFormat(token) && findSession(token) >= 0;
        }
        start = end + 1;
    }
    return false;
}

// ============================================================================
// Rutina de Servicio de Interrupción (ISR) - Caudalímetro Hall
// ============================================================================
void IRAM_ATTR flowPulseISR() {
    portENTER_CRITICAL_ISR(&flowMux);
    pulseCounter++;
    portEXIT_CRITICAL_ISR(&flowMux);
}

// ============================================================================
// Recuperación Canónica del Bus I2C (9 Ciclos de Reloj SCL)
// ============================================================================
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

// ============================================================================
// Detección de dirección I2C del SHT30 (0x44 / 0x45) con recuperación
// ============================================================================
bool detectAndInitSHT30() {
    if (sht30.begin(0x44)) { sht30Address = 0x44; sht30Available = true; }
    else if (sht30.begin(0x45)) { sht30Address = 0x45; sht30Available = true; }
    else {
        recoverI2CBus(I2C_SDA_PIN, I2C_SCL_PIN);
        if (sht30.begin(0x44)) { sht30Address = 0x44; sht30Available = true; }
        else if (sht30.begin(0x45)) { sht30Address = 0x45; sht30Available = true; }
        else { sht30Available = false; }
    }
    if (sht30Available) {
        sht30.heater(false);
    }
    return sht30Available;
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
    file.printf("%lu,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%u\n",
                static_cast<unsigned long>(record.uptimeSec),
                record.levelCm, record.levelPercent, record.flowLMin,
                record.dropRateCmH, record.tempC, record.pressureHpa,
                record.humidityPct, record.etoMmDay, record.riskState);
    file.close();
    xSemaphoreGive(spiMutex);
}

// ============================================================================
// Medición de Nivel Ultrasónico con Compensación Termo-Acústica (Fail-Safe)
// ============================================================================
float readAcousticLevel(float ambientTempC, float previousLevelCm) {
    digitalWrite(PIN_TRIG, LOW); delayMicroseconds(2);
    digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(10);
    digitalWrite(PIN_TRIG, LOW);

    long durationUs = pulseIn(PIN_ECHO, HIGH, 25000L);
    float measured = iot::watermonitor::measureCorrectedLevel(ambientTempC, durationUs);

    // CORRECCIÓN Ch2: Fail-safe — eco inválido mantiene último valor válido y reporta estado
    if (measured < 0.0f) {
        echoSensorOk = false;
        return previousLevelCm;
    }
    echoSensorOk = true;
    return measured;
}

// ============================================================================
// Obtener Día del Año Real (NTP) o Fallback
// ============================================================================
int getDayOfYear() {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 100)) {
        ntpSynced = true;
        return timeinfo.tm_yday + 1; // tm_yday es 0-based
    }
    ntpSynced = false;
    return 180; // Fallback si NTP no está disponible
}

// ============================================================================
// Inicialización Resiliente de Red: Default STA con Fallback a SoftAP
// ============================================================================
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
        delay(1200);
    } else {
        // Falla estricta: Timeout agotado
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
        delay(1200);
    }
}

// ============================================================================
// Supervisión Dinámica de Red (Ejecutada periódicamente en Core 0)
// ============================================================================
void manageWiFiConnectivity() {
    static unsigned long lastCheckMillis = 0;
    unsigned long now = millis();

    if (now - lastCheckMillis < 2000) {
        return;
    }
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
            Serial.printf("[OK] SoftAP Fallback activo: %s  IP: %s\n",
                          WIFI_AP_SSID, WiFi.softAPIP().toString().c_str());
            lastStaRetryMillis = now;
        } else if (now - lastStaRetryMillis >= WIFI_RECONNECT_INTERVAL_MS) {
            lastStaRetryMillis = now;
            Serial.printf("[WIFI] Reintentando asociacion a red STA (%s)...\n", WIFI_STA_SSID);
            WiFi.begin(WIFI_STA_SSID, WIFI_STA_PASS);
        }
    }
}

// ============================================================================
// Single Page Application (SPA) Web Dashboard en PROGMEM
// Con Autenticación por Sesión, Gráficas Interactivas HTML5 Canvas
// ============================================================================
const char HTML_LOGIN[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Login - Nodo IoT Sabana Centro</title>
<style>
:root{--bg:#070d1e;--card:#131c36;--border:#263556;--text:#f1f5f9;--sub:#94a3b8;--green:#10b981}
*{box-sizing:border-box;margin:0;padding:0;font-family:-apple-system,BlinkMacSystemFont,Segoe UI,Roboto,sans-serif}
body{background:var(--bg);color:var(--text);display:flex;align-items:center;justify-content:center;min-height:100vh}
.login-box{background:var(--card);padding:32px;border-radius:12px;border:1px solid var(--border);width:320px}
h2{text-align:center;margin-bottom:20px;font-size:1.1rem}
.sub{text-align:center;font-size:0.75rem;color:var(--sub);margin-bottom:16px}
input{width:100%;padding:10px;margin-bottom:12px;background:#1e293b;border:1px solid var(--border);color:#fff;border-radius:6px;font-size:0.9rem}
button{width:100%;padding:10px;background:var(--green);color:#fff;border:none;border-radius:6px;font-size:0.95rem;font-weight:600;cursor:pointer}
button:active{transform:scale(0.97)}
.err{color:#ef4444;font-size:0.8rem;text-align:center;margin-top:8px;display:none}
</style>
</head>
<body>
<div class="login-box">
  <h2>NODO IOT SABANA CENTRO</h2>
  <div class="sub">Autenticacion Requerida &bull; WLAN Municipal</div>
  <form id="lf" onsubmit="return doLogin(event)">
    <input type="text" id="user" placeholder="Usuario" autocomplete="username" required>
    <input type="password" id="pass" placeholder="Contrasena" autocomplete="current-password" required>
    <button type="submit">Ingresar</button>
  </form>
  <div id="err" class="err">Credenciales incorrectas</div>
</div>
<script>
async function doLogin(e){
  e.preventDefault();
  const r=await fetch('/api/login',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({user:document.getElementById('user').value,pass:document.getElementById('pass').value})});
  if(r.ok){location.href='/';}else{document.getElementById('err').style.display='block';}
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
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Nodo IoT Sabana Centro - Ch2</title>
<style>
:root{--bg:#070d1e;--card:#131c36;--border:#263556;--text:#f1f5f9;--sub:#94a3b8;--green:#10b981;--blue:#38bdf8;--yellow:#f59e0b;--red:#ef4444;--teal:#14b8a6}
*{box-sizing:border-box;margin:0;padding:0;font-family:-apple-system,BlinkMacSystemFont,Segoe UI,Roboto,sans-serif}
body{background:var(--bg);color:var(--text);padding:12px;max-width:560px;margin:0 auto}
h1{font-size:1.15rem;text-align:center;letter-spacing:0.5px}
.sub{text-align:center;font-size:0.75rem;color:var(--sub);margin-bottom:10px}
.badge{padding:9px;border-radius:8px;font-weight:bold;text-align:center;font-size:0.95rem;margin-bottom:10px;letter-spacing:0.5px;transition:all .3s}
.b-norm{background:#064e3b;color:#a7f3d0}
.b-stress{background:#1e3a8a;color:#bfdbfe}
.b-alert{background:#78350f;color:#fde68a}
.b-emerg{background:#7f1d1d;color:#fecaca;animation:pulse 1s infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:0.35}}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-bottom:10px}
.card{background:var(--card);padding:10px;border-radius:8px;border:1px solid var(--border)}
.card-t{font-size:0.68rem;color:var(--sub);text-transform:uppercase;margin-bottom:2px}
.card-v{font-size:1.25rem;font-weight:bold}
.unit{font-size:0.75rem;font-weight:normal;color:var(--sub)}
.bar-wrap{background:#1e293b;border-radius:99px;height:8px;overflow:hidden;margin-top:5px}
.bar-fill{background:var(--green);height:100%;transition:width .4s}
.chart-box{background:var(--card);padding:10px;border-radius:8px;border:1px solid var(--border);margin-bottom:10px}
.chart-header{display:flex;justify-content:space-between;align-items:center;margin-bottom:6px;font-size:0.75rem;color:var(--sub)}
canvas{width:100%;height:180px;background:#0b1329;border-radius:6px;display:block}
.btns{display:grid;grid-template-columns:repeat(4,1fr);gap:6px;margin-top:6px}
button{background:#1e293b;color:#fff;border:1px solid var(--border);padding:8px 4px;border-radius:6px;font-size:0.75rem;font-weight:600;cursor:pointer}
button:active{transform:scale(0.96)}
.btn-auto{background:#0284c7;grid-column:span 4;border:none}
.btn-silence{background:#d97706;grid-column:span 2;border:none}
.btn-unsilence{background:#475569;grid-column:span 2;border:none}
.btn-logout{background:#7f1d1d;grid-column:span 4;border:none;margin-top:6px}
.info-bar{font-size:0.65rem;color:var(--sub);text-align:center;margin:6px 0;padding:4px;background:#0f172a;border-radius:4px}
.footer{text-align:center;font-size:0.68rem;color:var(--sub);margin-top:10px}
select{background:#1e293b;color:#fff;border:1px solid var(--border);padding:4px 8px;border-radius:4px;font-size:0.72rem}
</style>
</head>
<body>
<h1>NODO IOT SABANA CENTRO</h1>
<div class="sub">Challenge #2 &bull; Dual-Core FreeRTOS &bull; SHT30 + ILI9341 &bull; Histeresis</div>
<div id="risk-badge" class="badge b-norm">ESTADO: NORMAL</div>
<div class="grid">
  <div class="card" style="grid-column:span 2">
    <div class="card-t">Nivel de Reservorio</div>
    <div class="card-v"><span id="lvl-cm">--</span> <span class="unit">cm</span> (<span id="lvl-pct">--</span>%)</div>
    <div class="bar-wrap"><div id="lvl-bar" class="bar-fill" style="width:0%"></div></div>
  </div>
  <div class="card">
    <div class="card-t">Caudal Instant&aacute;neo</div>
    <div class="card-v"><span id="flow">--</span> <span class="unit">L/min</span></div>
    <div class="card-t">Tasa de Descenso</div>
    <div class="card-v"><span id="drop">--</span> <span class="unit">cm/h</span></div>
  </div>
  <div class="card">
    <div class="card-t">Temperatura (SHT30)</div>
    <div class="card-v"><span id="temp">--</span> <span class="unit">&deg;C</span></div>
    <div class="card-t">Humedad Relativa</div>
    <div class="card-v"><span id="hum">--</span> <span class="unit">%HR</span></div>
  </div>
  <div class="card">
    <div class="card-t">Presi&oacute;n Atmosf&eacute;rica</div>
    <div class="card-v"><span id="press">--</span> <span class="unit">hPa</span></div>
  </div>
  <div class="card">
    <div class="card-t">ETo Hargreaves-Samani</div>
    <div class="card-v"><span id="eto">--</span> <span class="unit">mm/d&iacute;a</span></div>
  </div>
  <div class="card" style="grid-column:span 2">
    <div class="card-t">Diagn&oacute;stico y Salud de Hardware</div>
    <div style="display:flex;justify-content:space-between;font-size:0.75rem;padding-top:4px;flex-wrap:wrap;gap:6px">
      <span>SHT30: <b id="hw-sht">--</b></span>
      <span>Eco US: <b id="hw-echo">--</b></span>
      <span>LittleFS: <b id="hw-fs">--</b></span>
      <span>SD: <b id="hw-sd">--</b></span>
      <span>RAM: <b id="hw-ram">--</b></span>
      <span>WiFi: <b id="hw-rssi">--</b></span>
    </div>
  </div>
</div>
<div id="info-bar" class="info-bar">Uptime: -- | NTP: -- | Histeresis: activa</div>
<div class="chart-box">
  <div class="chart-header">
    <span>Historico (ultimas muestras)</span>
    <select id="chart-var" onchange="drawChart()">
      <option value="l">Nivel (cm)</option>
      <option value="tc">Temp (C)</option>
      <option value="h">Humedad (%)</option>
      <option value="q">Caudal (L/min)</option>
      <option value="p">Presion (hPa)</option>
    </select>
  </div>
  <canvas id="chart"></canvas>
</div>
<div class="btns">
  <button onclick="sendCmd('test 0')">Normal</button>
  <button onclick="sendCmd('test 1')">Estr&eacute;s</button>
  <button onclick="sendCmd('test 2')">Alerta</button>
  <button onclick="sendCmd('test 3')">Emerg.</button>
  <button class="btn-auto" onclick="sendCmd('auto')">Modo Autom&aacute;tico (Sensores)</button>
  <button class="btn-silence" onclick="sendCmd('silence 300')">Silenciar 5min</button>
  <button class="btn-unsilence" onclick="sendCmd('unsilence')">Reactivar</button>
  <button class="btn-logout" onclick="logout()">Cerrar Sesi&oacute;n</button>
</div>
<div class="footer">ESP32 Ch2 &bull; Core 0: WebServer &bull; Core 1: Fusion &bull; Histeresis 3/5</div>
<script>
let histData=[];
const BC=['#10b981','#38bdf8','#14b8a6','#f59e0b','#94a3b8'];
function upd(){
  fetch('/api/data').then(r=>{if(!r.ok)throw r;return r.json()}).then(d=>{
    document.getElementById('lvl-cm').textContent=d.level_cm.toFixed(1);
    document.getElementById('lvl-pct').textContent=d.level_pct.toFixed(0);
    document.getElementById('lvl-bar').style.width=Math.max(0,Math.min(100,d.level_pct))+'%';
    document.getElementById('flow').textContent=d.flow_lmin.toFixed(2);
    document.getElementById('drop').textContent=d.drop_rate.toFixed(2);
    document.getElementById('temp').textContent=d.temp_c.toFixed(1);
    document.getElementById('hum').textContent=d.humidity_pct.toFixed(1);
    document.getElementById('press').textContent=d.pressure_hpa<0?'N/D':d.pressure_hpa.toFixed(0);
    document.getElementById('eto').textContent=d.eto_mm_day.toFixed(2);
    let b=document.getElementById('risk-badge');
    let cls=['b-norm','b-stress','b-alert','b-emerg'];
    b.className='badge '+(cls[d.risk_state]||cls[0]);
    b.textContent='ESTADO: '+d.risk_label+(d.override>=0?' [MANUAL]':'')+(d.silenced?' [SILENCIADO]':'');
    let bar=document.getElementById('lvl-bar');
    bar.style.background=d.risk_state>=2?'var(--red)':d.risk_state==1?'var(--yellow)':'var(--green)';
    document.getElementById('info-bar').textContent=
      'Uptime: '+(d.uptime_s/60|0)+'min | NTP: '+(d.ntp_synced?'OK':'Fallback')+
      ' | Histeresis: '+(d.hysteresis_state>=0?'R'+d.hysteresis_state:'activa')+
      (d.silenced?' | Silencio: '+d.silence_s+'s':'');
    if(d.hw){
      let shtEl=document.getElementById('hw-sht');
      shtEl.textContent=d.hw.sht?'ONLINE':'FALLO';
      shtEl.style.color=d.hw.sht?'var(--green)':'var(--red)';
      let echoEl=document.getElementById('hw-echo');
      echoEl.textContent=d.hw.echo?'OK':'SIN ECO';
      echoEl.style.color=d.hw.echo?'var(--green)':'var(--red)';
      let fsEl=document.getElementById('hw-fs');
      fsEl.textContent=d.hw.fs?'MONTADO':'N/A';
      fsEl.style.color=d.hw.fs?'var(--green)':'var(--yellow)';
      let sdEl=document.getElementById('hw-sd');
      sdEl.textContent=d.hw.sd?'MONTADA':'N/A';
      sdEl.style.color=d.hw.sd?'var(--green)':'var(--yellow)';
      document.getElementById('hw-ram').textContent=d.hw.heap_kb+' KB';
      let wifiEl=document.getElementById('hw-rssi');
      if(d.hw.wifi_sta){
        wifiEl.textContent='STA ('+d.hw.rssi+' dBm)';
        wifiEl.style.color='var(--green)';
      }else if(d.hw.ap_fallback){
        wifiEl.textContent='AP Fallback';
        wifiEl.style.color='var(--yellow)';
      }else{
        wifiEl.textContent='DESCONECT';
        wifiEl.style.color='var(--red)';
      }
    }
  }).catch(()=>{location.href='/login';});
}
function loadHist(){
  fetch('/api/history').then(r=>r.json()).then(d=>{histData=d.records||[];drawChart();}).catch(()=>{});
}
function drawChart(){
  let c=document.getElementById('chart'),ctx=c.getContext('2d');
  c.width=c.offsetWidth;c.height=180;
  ctx.fillStyle='#0b1329';ctx.fillRect(0,0,c.width,c.height);
  if(!histData.length)return;
  let k=document.getElementById('chart-var').value;
  let vals=histData.map(r=>r[k]||0);
  let mn=Math.min(...vals),mx=Math.max(...vals);
  if(mx===mn)mx=mn+1;
  let w=c.width,h=c.height,pad=20;
  ctx.strokeStyle=BC[['l','tc','h','q','p'].indexOf(k)]||BC[0];
  ctx.lineWidth=1.5;ctx.beginPath();
  for(let i=0;i<vals.length;i++){
    let x=pad+i*(w-pad*2)/(vals.length-1||1);
    let y=h-pad-(vals[i]-mn)/(mx-mn)*(h-pad*2);
    i===0?ctx.moveTo(x,y):ctx.lineTo(x,y);
  }
  ctx.stroke();
  ctx.fillStyle='#94a3b8';ctx.font='10px sans-serif';
  ctx.fillText(mx.toFixed(1),2,pad-2);
  ctx.fillText(mn.toFixed(1),2,h-4);
  ctx.fillText(vals.length+' muestras',w-80,h-4);
}
function sendCmd(c){fetch('/api/cmd?c='+encodeURIComponent(c),{method:'POST'}).catch(()=>{});}
function logout(){fetch('/api/logout',{method:'POST'}).then(()=>{location.href='/login';}).catch(()=>{location.href='/login';});}
setInterval(upd,2000);
setInterval(loadHist,30000);
upd();loadHist();
</script>
</body>
</html>
)rawliteral";

// ============================================================================
// Filtro de Seguridad por Subred (Verificación IP)
// ============================================================================
bool validateSubnetFilter() {
    WiFiClient client = server.client();
    IPAddress clientIP = client.remoteIP();
    IPAddress mask(255, 255, 255, 0);

    // 1. Validar contra la subred del SoftAP local (192.168.4.0/24) si está activo
    IPAddress apIP = WiFi.softAPIP();
    uint32_t cIp = static_cast<uint32_t>(clientIP);
    uint32_t apIpUint = static_cast<uint32_t>(apIP);
    uint32_t sMask = static_cast<uint32_t>(mask);

    if (apIpUint != 0 && iot::watermonitor::isSubnetAllowed(cIp, apIpUint, sMask)) {
        return true;
    }

    // 2. Validar contra la subred STA
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
// Manejadores HTTP (Core 0 — WebServer)
// ============================================================================

// Forward declaration para CLI
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

    // Verificar password con SHA-256 + salt
    char inputHash[65];
    computePasswordHash(AUTH_SALT, pass.c_str(), inputHash);
    if (strcmp(inputHash, storedPasswordHash) != 0) {
        server.send(401, "text/plain", "Unauthorized");
        return;
    }

    // Crear sesión
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

    char json[576];
    snprintf(json, sizeof(json),
        "{\"level_cm\":%.2f,\"level_pct\":%.1f,\"flow_lmin\":%.2f,\"drop_rate\":%.2f,"
        "\"temp_c\":%.2f,\"pressure_hpa\":%.2f,\"humidity_pct\":%.2f,\"eto_mm_day\":%.2f,"
        "\"risk_state\":%d,\"risk_label\":\"%s\",\"override\":%d,"
        "\"silenced\":%s,\"silence_s\":%lu,\"uptime_s\":%lu,"
        "\"ntp_synced\":%s,\"hysteresis_state\":%d,"
        "\"hw\":{\"sht\":%s,\"echo\":%s,\"fs\":%s,\"sd\":%s,\"heap_kb\":%u,\"rssi\":%d,\"wifi_sta\":%s,\"ap_fallback\":%s}}",
        rec.levelCm, rec.levelPercent, rec.flowLMin, rec.dropRateCmH,
        rec.tempC, rec.pressureHpa, rec.humidityPct, rec.etoMmDay,
        effectiveRisk, iot::watermonitor::riskLabel(effectiveRisk),
        currentOverride, (silenced ? "true" : "false"),
        static_cast<unsigned long>(silenceRemainingSec),
        static_cast<unsigned long>(rec.uptimeSec),
        (ntpSynced ? "true" : "false"), hystState,
        (sht30Available ? "true" : "false"),
        (echoSensorOk ? "true" : "false"),
        (littleFsMounted ? "true" : "false"),
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

    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "application/json", "");
    server.sendContent("{\"capacity\":360,\"records\":[");

    size_t count = 0;
    if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        count = sramRingBuffer.size();
        xSemaphoreGive(telemetryMutex);
    }

    for (size_t i = 0; i < count; i++) {
        iot::watermonitor::TelemetryRecord item{};
        bool gotRecord = false;
        if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            gotRecord = sramRingBuffer.get(i, item);
            xSemaphoreGive(telemetryMutex);
        }
        if (gotRecord) {
            char itemJson[100];
            snprintf(itemJson, sizeof(itemJson),
                "%s{\"t\":%lu,\"l\":%.1f,\"lp\":%.0f,\"q\":%.1f,\"tc\":%.1f,\"h\":%.1f,\"p\":%.0f,\"r\":%u}",
                (i > 0 ? "," : ""),
                static_cast<unsigned long>(item.uptimeSec),
                item.levelCm, item.levelPercent,
                item.flowLMin, item.tempC, item.humidityPct, item.pressureHpa, item.riskState
            );
            server.sendContent(itemJson);
        }
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
// Tarea FreeRTOS Core 0: Servidor Web HTTP lwIP
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
// Tarea FreeRTOS Core 1: Adquisición de Sensores (HC-SR04, Hall, SHT30)
// ============================================================================
void Task_SensorAcquisition(void* pvParameters) {
    (void)pvParameters;
    TickType_t lastWakeTime = xTaskGetTickCount();
    float prevLevel = iot::watermonitor::TANK_HEIGHT_CM;
    float lastValidTempC = 20.0f;
    float lastValidHumidityPct = 65.0f;
    unsigned long lastAcqTime = millis();
    unsigned long lastShtRetry = 0;
    unsigned long lastSdPersist = 0;

    for (;;) {
        unsigned long now = millis();
        float elapsedSec = (now - lastAcqTime) / 1000.0f;
        if (elapsedSec <= 0.0f) elapsedSec = 2.0f;
        lastAcqTime = now;

        // 1. Conteo atómico de pulsos Hall con spinlock
        portENTER_CRITICAL(&flowMux);
        unsigned long pulses = pulseCounter;
        pulseCounter = 0;
        portEXIT_CRITICAL(&flowMux);

        // 2. Lectura SHT30 con reintento periódico. El SHT30 no mide presión.
        float ambientTempC = 20.0f;
        float pressureHpa = PRESSURE_UNAVAILABLE_HPA;
        float humidityPct = 65.0f;

        if (sht30Available) {
            ambientTempC = sht30.readTemperature();
            humidityPct = sht30.readHumidity();
            if (isnan(ambientTempC) || isnan(humidityPct)) {
                ambientTempC = lastValidTempC;
                humidityPct = lastValidHumidityPct;
                sht30Available = false;
            } else {
                lastValidTempC = ambientTempC;
                lastValidHumidityPct = humidityPct;
            }
        } else if (now - lastShtRetry > 30000) {
            lastShtRetry = now;
            detectAndInitSHT30();
        }

        // 3. Medición de Nivel Acústico (Fail-Safe: mantiene último válido)
        float currentLevel = readAcousticLevel(ambientTempC, prevLevel);
        prevLevel = currentLevel;

        // 4. Actualizar ventana térmica 24h para Hargreaves-Samani
        if (ambientTempC < tempMin24h) tempMin24h = ambientTempC;
        if (ambientTempC > tempMax24h) tempMax24h = ambientTempC;
        // Reset ventana cada 24h
        if (now - lastTempResetMillis > TEMP_WINDOW_MS) {
            lastTempResetMillis = now;
            tempMin24h = ambientTempC;
            tempMax24h = ambientTempC;
        }

        // 5. Publicar datos crudos para Task_EdgeAnalytics
        rawSensorData.ambientTempC = ambientTempC;
        rawSensorData.pressureHpa = pressureHpa;
        rawSensorData.humidityPct = humidityPct;
        rawSensorData.waterLevelCm = currentLevel;
        rawSensorData.pulses = pulses;
        rawSensorData.elapsedSec = elapsedSec;

        if (xAnalyticsTaskHandle != NULL) {
            xTaskNotifyGive(xAnalyticsTaskHandle);
        }

        vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}

// ============================================================================
// Tarea FreeRTOS Core 1: Analítica de Borde, ETo y Matriz de Riesgo con Histéresis
// ============================================================================
void Task_EdgeAnalytics(void* pvParameters) {
    (void)pvParameters;
    float previousWaterLevel = iot::watermonitor::TANK_HEIGHT_CM;
    bool isFirstAnalyticsRun = true;
    unsigned long lastSdPersist = 0;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        RawAcquisitionData data = rawSensorData;

        // 1. Caudal volumétrico
        float flowFactor = currentFlowFactor;
        float flowLMin = iot::watermonitor::computeInstantFlowLMin(data.pulses, data.elapsedSec, flowFactor);

        // 2. Nivel relativo y tasa de descenso
        float levelPercent = (data.waterLevelCm / iot::watermonitor::TANK_HEIGHT_CM) * 100.0f;
        if (isFirstAnalyticsRun) {
            previousWaterLevel = data.waterLevelCm;
            isFirstAnalyticsRun = false;
        }

        float dropRateCmH = (previousWaterLevel - data.waterLevelCm) * (3600.0f / data.elapsedSec);
        if (dropRateCmH < 0.0f) dropRateCmH = 0.0f;
        previousWaterLevel = data.waterLevelCm;

        // 3. Evapotranspiración ETo Hargreaves-Samani con día juliano REAL (NTP)
        int dayOfYear = getDayOfYear();
        float eto = iot::watermonitor::calculateHargreavesETo(dayOfYear, tempMin24h, tempMax24h);

        // 4. Evaluación de Riesgo Instantáneo
        int rawRisk = iot::watermonitor::evaluateRisk(levelPercent, dropRateCmH, eto, flowLMin);

        // 5. Histéresis Temporal (RiskStateMachine)
        int stableRisk = 0;
        if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            stableRisk = riskStateMachine.update(rawRisk);
            xSemaphoreGive(telemetryMutex);
        }

        // 6. Empaquetar y almacenar en SramRingBuffer (con Mutex)
        iot::watermonitor::TelemetryRecord rec{};
        rec.uptimeSec = millis() / 1000;
        rec.levelCm = data.waterLevelCm;
        rec.levelPercent = levelPercent;
        rec.flowLMin = flowLMin;
        rec.dropRateCmH = dropRateCmH;
        rec.tempC = data.ambientTempC;
        rec.pressureHpa = data.pressureHpa;
        rec.humidityPct = data.humidityPct;
        rec.etoMmDay = eto;
        rec.riskState = static_cast<uint8_t>(stableRisk);

        if (xSemaphoreTake(telemetryMutex, portMAX_DELAY) == pdTRUE) {
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
// Renderizado del Display ILI9341: página fija densa (240x320, RGB565)
// ============================================================================
unsigned long lastTftRenderMillis = 0;

void renderTftStatusPage(int effectiveRisk, const iot::watermonitor::TelemetryRecord& rec) {
    if (spiMutex == NULL ||
        xSemaphoreTake(spiMutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    const unsigned long now = millis();
    if (now - lastTftRenderMillis < 250) {
        xSemaphoreGive(spiMutex);
        return;
    }
    lastTftRenderMillis = now;

    const uint16_t riskColor = effectiveRisk == 0 ? ILI9341_GREEN :
                               effectiveRisk == 1 ? ILI9341_CYAN :
                               effectiveRisk == 2 ? ILI9341_ORANGE : ILI9341_RED;
    const char* riskLabel = iot::watermonitor::riskLabel(effectiveRisk);
    const String ip = WiFi.status() == WL_CONNECTED
                        ? WiFi.localIP().toString()
                        : ((apFallbackActive || (WiFi.getMode() & WIFI_MODE_AP))
                           ? WiFi.softAPIP().toString() : "conectando");
    const char* buzzer = isAlarmSilenced ? "MUT" : (effectiveRisk >= 2 ? "ON" : "OFF");

    tft.fillRect(0, 0, 240, 320, ILI9341_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(6, 5);
    tft.print("MONITOR HIDRICO");
    tft.setTextSize(1);
    tft.setCursor(6, 30);
    tft.print(WiFi.status() == WL_CONNECTED ? "IP: " : "AP: ");
    tft.print(ip);
    tft.setCursor(6, 43);
    tft.print("Buzzer: ");
    tft.print(buzzer);
    if (manualRiskOverride >= 0) tft.print("  TEST");

    tft.fillRoundRect(4, 58, 232, 38, 4, riskColor);
    tft.setTextColor(ILI9341_BLACK);
    tft.setTextSize(2);
    tft.setCursor(12, 69);
    tft.print("RIESGO: ");
    tft.print(riskLabel);

    tft.setTextColor(ILI9341_WHITE);
    tft.setTextSize(2);
    tft.setCursor(8, 110);
    tft.printf("Nivel: %5.1f cm", rec.levelCm);
    tft.setCursor(8, 132);
    tft.printf("Llenado: %4.1f %%", rec.levelPercent);
    tft.setCursor(8, 154);
    tft.printf("Caudal: %5.2f L/m", rec.flowLMin);
    tft.setCursor(8, 176);
    tft.printf("Descenso: %4.1f cm/h", rec.dropRateCmH);
    tft.setCursor(8, 198);
    tft.printf("Temp: %5.1f C", rec.tempC);
    tft.setCursor(8, 220);
    tft.printf("Hum:  %5.1f %%", rec.humidityPct);
    tft.setCursor(8, 242);
    tft.printf("ETo:  %5.2f mm/d", rec.etoMmDay);

    tft.setTextSize(1);
    tft.setCursor(8, 270);
    tft.print("SHT30: ");
    tft.print(sht30Available ? "OK" : "N/A");
    tft.print("  SD: ");
    tft.print(sdMounted ? "OK" : "N/A");
    tft.setCursor(8, 285);
    tft.print("Presion: ");
    tft.print(rec.pressureHpa < 0 ? "N/D (SHT30)" : String(rec.pressureHpa, 1) + " hPa");
    tft.setCursor(8, 302);
    tft.printf("Uptime: %lu s", static_cast<unsigned long>(rec.uptimeSec));
    xSemaphoreGive(spiMutex);
}

// ============================================================================
// Tarea FreeRTOS Core 1: HMI Local (ILI9341, LEDs, Buzzer con Watchdog)
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
        switch (effectiveRisk) {
            case 0:
                digitalWrite(PIN_LED_RED, LOW);
                digitalWrite(PIN_LED_GREEN, HIGH);
                digitalWrite(PIN_LED_BLUE, LOW);
                noTone(PIN_BUZZER);
                break;
            case 1:
                digitalWrite(PIN_LED_RED, LOW);
                digitalWrite(PIN_LED_GREEN, LOW);
                digitalWrite(PIN_LED_BLUE, HIGH);
                noTone(PIN_BUZZER);
                break;
            case 2:
                digitalWrite(PIN_LED_RED, HIGH);
                digitalWrite(PIN_LED_GREEN, LOW);
                digitalWrite(PIN_LED_BLUE, LOW);
                if (!isAlarmSilenced && ((now % 1000) < 150)) {
                    tone(PIN_BUZZER, 1200);
                } else {
                    noTone(PIN_BUZZER);
                }
                break;
            case 3: {
                bool strobe = (now / 120) % 2;
                digitalWrite(PIN_LED_RED, strobe);
                digitalWrite(PIN_LED_GREEN, LOW);
                digitalWrite(PIN_LED_BLUE, !strobe);
                if (!isAlarmSilenced) {
                    tone(PIN_BUZZER, 2800);
                } else {
                    noTone(PIN_BUZZER);
                }
                break;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ============================================================================
// Consola de Servicio CLI Embebida (UART0 @ 115200)
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
        Serial.println("  status          : Telemetria, memoria FreeRTOS y SHT30/SD");
        Serial.println("  test <0-3>      : Fuerza estado de riesgo");
        Serial.println("  auto            : Retorna al modo automatico");
        Serial.println("  silence <seg>   : Silencia buzzer por N segundos");
        Serial.println("  unsilence       : Reactiva buzzer inmediatamente");
        Serial.println("  flow <factor>   : Ajustar factor caudalimetro");
        Serial.println("  ntp             : Muestra estado NTP y dia juliano");
        Serial.println("  hysteresis      : Muestra estado de maquina de histeresis");
        Serial.println("  wifi            : Diagnostico de red (STA / Fallback SoftAP)");
        Serial.println("------------------------------------------------------------");
    }
    else if (strcmp(cmd, "status") == 0) {
        iot::watermonitor::TelemetryRecord rec{};
        if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            rec = latestRecord;
            xSemaphoreGive(telemetryMutex);
        }
        Serial.printf("\n[STATUS] Nivel: %.1f cm (%.0f%%)  Caudal: %.2f L/min  Desc: %.2f cm/h\n",
                      rec.levelCm, rec.levelPercent, rec.flowLMin, rec.dropRateCmH);
        Serial.printf("[STATUS] Temp: %.1f C  Humedad: %.1f%%  Presion: %.0f hPa  ETo: %.2f mm/d\n",
                      rec.tempC, rec.humidityPct, rec.pressureHpa, rec.etoMmDay);
        Serial.printf("[STATUS] Riesgo: %s (%d)  Override: %d  Silenciado: %s\n",
                      iot::watermonitor::riskLabel(rec.riskState), rec.riskState,
                      manualRiskOverride, isAlarmSilenced ? "SI" : "NO");
        Serial.printf("[STATUS] Buffer historico: %d/%d  Uptime: %lu s  SHT30: %s (0x%02X)  SD: %s\n",
                      (int)sramRingBuffer.size(), (int)sramRingBuffer.capacity(),
                      rec.uptimeSec, sht30Available ? "OK" : "N/A", sht30Address,
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
        if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            isAlarmSilenced = true;
            silenceStartMillis = millis();
            silenceDurationMillis = sec * 1000UL;
            xSemaphoreGive(telemetryMutex);
        }
        Serial.printf("[OK] Buzzer silenciado por %lu segundos.\n", (unsigned long)sec);
    }
    else if (strcmp(cmd, "unsilence") == 0) {
        if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            isAlarmSilenced = false;
            silenceDurationMillis = 0;
            xSemaphoreGive(telemetryMutex);
        }
        Serial.println("[OK] Buzzer reactivado manualmente.");
    }
    else if (strncmp(cmd, "flow ", 5) == 0) {
        float f = atof(cmd + 5);
        if (f >= 0.5f && f <= 50.0f) {
            currentFlowFactor = f;
            Serial.printf("[OK] Factor caudalimetro ajustado a %.2f.\n", f);
        } else {
            Serial.println("[ERROR] Factor fuera de rango (0.5 a 50.0).");
        }
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
    Serial.println(" SHT30 + ILI9341 + SD + Histeresis + Auth SHA-256 + NTP ");
    Serial.println("========================================================");

    // Configuración de Pines GPIO
    pinMode(PIN_TRIG, OUTPUT);
    pinMode(PIN_ECHO, INPUT);
    pinMode(PIN_FLOW_HALL, INPUT_PULLUP);
    pinMode(PIN_BUZZER, OUTPUT);
    pinMode(PIN_LED_RED, OUTPUT);
    pinMode(PIN_LED_GREEN, OUTPUT);
    pinMode(PIN_LED_BLUE, OUTPUT);

    digitalWrite(PIN_LED_RED, LOW);
    digitalWrite(PIN_LED_GREEN, LOW);
    digitalWrite(PIN_LED_BLUE, LOW);
    noTone(PIN_BUZZER);

    // Interrupción Externa Hall con Spinlock
    attachInterrupt(digitalPinToInterrupt(PIN_FLOW_HALL), flowPulseISR, FALLING);

    pinMode(PIN_TFT_BL, OUTPUT);
    digitalWrite(PIN_TFT_BL, HIGH);
    pinMode(PIN_TFT_CS, OUTPUT);
    pinMode(PIN_SD_CS, OUTPUT);
    digitalWrite(PIN_TFT_CS, HIGH);
    digitalWrite(PIN_SD_CS, HIGH);

    // Inicialización de buses
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setClock(100000);
    SPI.begin(PIN_TFT_SCK, PIN_TFT_MISO, PIN_TFT_MOSI, PIN_TFT_CS);

    tft.begin();
    tft.setRotation(0);
    tft.fillScreen(ILI9341_BLACK);
    tft.setTextColor(ILI9341_WHITE);
    tft.setTextSize(2);
    tft.setCursor(8, 12);
    tft.println("MONITOR HIDRICO");
    tft.println("INICIANDO...");

    // Detección SHT30
    if (detectAndInitSHT30()) {
        Serial.printf("[OK] Sensor SHT30 detectado en 0x%02X.\n", sht30Address);
    } else {
        Serial.println("[WARN] SHT30 no detectado. Valores de respaldo.");
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

    // Inicializar LittleFS
    if (LittleFS.begin(true)) {
        littleFsMounted = true;
        Serial.println("[OK] LittleFS montado correctamente.");
    } else {
        littleFsMounted = false;
        Serial.println("[WARN] LittleFS no disponible.");
    }

    // Computar hash del password al arrancar
    computePasswordHash(AUTH_SALT, "sabana2026", storedPasswordHash);
    Serial.println("[OK] Hash SHA-256 de credenciales calculado.");

    delay(1000);
    tft.fillScreen(ILI9341_BLACK);

    // Red Resiliente: Default STA con Fallback a SoftAP
    initWiFiNetwork();

    // Configurar NTP para día juliano real
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
    Serial.println("[OK] NTP configurado (UTC-5 Colombia).");

    // Inicializar ventana térmica 24h
    lastTempResetMillis = millis();

    // Mutex FreeRTOS
    telemetryMutex = xSemaphoreCreateMutex();
    spiMutex = xSemaphoreCreateMutex();

    // Registro base de telemetría
    if (telemetryMutex != NULL) {
        latestRecord.uptimeSec = 0;
        latestRecord.levelCm = iot::watermonitor::TANK_HEIGHT_CM;
        latestRecord.levelPercent = 100.0f;
        latestRecord.flowLMin = 0.0f;
        latestRecord.dropRateCmH = 0.0f;
        latestRecord.tempC = 20.0f;
        latestRecord.pressureHpa = PRESSURE_UNAVAILABLE_HPA;
        latestRecord.humidityPct = 65.0f;
        latestRecord.etoMmDay = 3.8f;
        latestRecord.riskState = 0;
    }

    // Lanzamiento de Tareas FreeRTOS en Cores Específicos
    // Core 0: Red lwIP y Servidor Web HTTP
    xTaskCreatePinnedToCore(
        Task_WebServer_HTTP,
        "WebServerTask",
        8192,
        NULL,
        1,
        &xWebServerTaskHandle,
        0 // Core 0
    );

    // Core 1: Tareas de Borde en Tiempo Real y HMI Local
    xTaskCreatePinnedToCore(Task_EdgeAnalytics, "AnalyticsTask", 4096, NULL, 2, &xAnalyticsTaskHandle, 1);
    xTaskCreatePinnedToCore(Task_SensorAcquisition, "SensorTask", 4096, NULL, 2, &xSensorTaskHandle, 1);
    xTaskCreatePinnedToCore(Task_HMI_Local, "HmiTask", 4096, NULL, 1, &xHmiTaskHandle, 1);

    Serial.println("[OK] Tareas FreeRTOS creadas en Core 0 y Core 1.");
    Serial.println("[OK] Sistema listo. Escribe 'help' para comandos.");
    Serial.print("SabanaNode> ");
}

// ============================================================================
// LOOP PRINCIPAL (Consola Serial en Core 1)
// ============================================================================
void loop() {
    handleSerialCLI();
    vTaskDelay(pdMS_TO_TICKS(50));
}
