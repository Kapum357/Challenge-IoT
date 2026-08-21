#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BMP280.h>
#include <LiquidCrystal.h>
#include "WaterMonitorCore.h"

// ============================================================================
// SISTEMA IOT DE BORDE: MONITOREO HIDROMETEOROLÓGICO Y ALERTA TEMPRANA
// Sabana Centro, Cundinamarca - Latitud: 4.81° N
// Hardware: ESP32 DevKit v1 + HC-SR04 + BMP280 + LCD1602A (4-bit) + Buzzer + LEDs
// ============================================================================

// Mapeo de Pines de Hardware
#define PIN_TRIG        18  // Salida Trigger Ultrasonido
#define PIN_ECHO        19  // Entrada Echo Ultrasonido (con divisor 5V->3.3V)
#define PIN_FLOW_HALL   14  // Entrada Interrupción Sensor de Caudal Hall
#define PIN_BUZZER      27  // Salida Buzzer Activo / Tono PWM
#define PIN_LED_RED     25  // LED de Alerta Rojo
#define PIN_LED_GREEN   26  // LED de Estado Verde
#define PIN_LED_BLUE    33  // LED Azul

// Pines LCD 1602A - Interfaz Paralelo 4 bits
#define PIN_LCD_RS      23  // LCD Register Select
#define PIN_LCD_EN      17  // LCD Enable
#define PIN_LCD_D4      16  // LCD Dato bit 4
#define PIN_LCD_D5       4  // LCD Dato bit 5
#define PIN_LCD_D6       5  // LCD Dato bit 6
#define PIN_LCD_D7      13  // LCD Dato bit 7
// RW conectado a GND (modo solo escritura)
// V0 conectado a GND (contraste máximo)

// Intervalos de Muestreo
constexpr unsigned long SAMPLE_INTERVAL_MS = 2000; // Periodo de adquisición (2s)

// Instancias de Periféricos
Adafruit_BMP280 bmp;                                          // Sensor BMP280 vía I2C
LiquidCrystal   lcd(PIN_LCD_RS, PIN_LCD_EN,                  // LCD 1602A - 4 bits paralelo
                    PIN_LCD_D4, PIN_LCD_D5, PIN_LCD_D6, PIN_LCD_D7);

// Variables de Estado y Medición
volatile unsigned long pulseCounter = 0;
float currentFlow_Lmin = 0.0f;
float waterLevel_cm = 0.0f;
float previousLevel_cm = iot::watermonitor::TANK_HEIGHT_CM;
float tempMax_24h = 24.0f;
float tempMin_24h = 14.0f;
bool firstTempReading = true;
bool firstLevelReading = true;
float calculatedETo = 0.0f;
unsigned long lastSamplingTime = 0;
int systemRiskState = 0; // 0: Normal, 1: Estrés, 2: Alerta, 3: Emergencia
bool bmpAvailable = false;

// ============================================================================
// Rutina de Servicio de Interrupción (ISR) - Conteo de Flujo
// ============================================================================
void IRAM_ATTR flowPulseISR() {
    pulseCounter++;
}

// ============================================================================
// Medición de Nivel Ultrasónico con Compensación Termo-Acústica
// ============================================================================
float readAcousticLevel(float ambientTempC) {
    digitalWrite(PIN_TRIG, LOW);
    delayMicroseconds(2);
    digitalWrite(PIN_TRIG, HIGH);
    delayMicroseconds(10);
    digitalWrite(PIN_TRIG, LOW);

    // Timeout de 25000 us (~4.3 metros)
    long durationUs = pulseIn(PIN_ECHO, HIGH, 25000L);
    if (durationUs <= 0) {
        return previousLevel_cm; // Retener último valor válido en caso de eco perdido
    }

    return iot::watermonitor::measureCorrectedLevel(ambientTempC, durationUs);
}

// ============================================================================
// Control Óptico y Acústico de Alertas In-Situ
// ============================================================================
void applyAlertOutputs(int riskState) {
    switch (riskState) {
        case 0: // NORMAL: LED Verde encendido, Buzzer apagado
            digitalWrite(PIN_LED_RED, LOW);
            digitalWrite(PIN_LED_GREEN, HIGH);
            digitalWrite(PIN_LED_BLUE, LOW);
            noTone(PIN_BUZZER);
            break;

        case 1: // ESTRÉS MODERADO: LED Azul encendido (demanda climática/caudal), Buzzer apagado
            digitalWrite(PIN_LED_RED, LOW);
            digitalWrite(PIN_LED_GREEN, LOW);
            digitalWrite(PIN_LED_BLUE, HIGH);
            noTone(PIN_BUZZER);
            break;

        case 2: // ALERTA SEVERA: LED Rojo fijo, Beep intermitente
            digitalWrite(PIN_LED_RED, HIGH);
            digitalWrite(PIN_LED_GREEN, LOW);
            digitalWrite(PIN_LED_BLUE, LOW);
            tone(PIN_BUZZER, 1200, 100); // 1200 Hz por 100 ms
            break;

        case 3: // EMERGENCIA CRÍTICA: LEDs Rojo y Azul alternando en estroboscópico, Sirena continua 2800 Hz
            digitalWrite(PIN_LED_RED, (millis() / 150) % 2);
            digitalWrite(PIN_LED_GREEN, LOW);
            digitalWrite(PIN_LED_BLUE, !((millis() / 150) % 2));
            tone(PIN_BUZZER, 2800); // Tono agudo continuo (85 dB)
            break;
    }
}

// ============================================================================
// SETUP
// ============================================================================
void setup() {
    Serial.begin(115200);
    delay(200);

    Serial.println("\n==================================================");
    Serial.println(" INICIANDO NODO DE BORDE IOT - SABANA CENTRO     ");
    Serial.println("==================================================");

    // Configuración de Pines GPIO
    pinMode(PIN_TRIG, OUTPUT);
    pinMode(PIN_ECHO, INPUT);
    pinMode(PIN_FLOW_HALL, INPUT_PULLUP);
    pinMode(PIN_BUZZER, OUTPUT);
    pinMode(PIN_LED_RED, OUTPUT);
    pinMode(PIN_LED_GREEN, OUTPUT);
    pinMode(PIN_LED_BLUE, OUTPUT);

    // Adjuntar interrupción para pulsos del sensor Hall
    attachInterrupt(digitalPinToInterrupt(PIN_FLOW_HALL), flowPulseISR, FALLING);

    // Inicialización de bus I2C (SDA=21, SCL=22)
    Wire.begin(21, 22);

    // Inicializar Display LCD 1602A (Paralelo 4 bits, 16 cols x 2 filas)
    lcd.begin(16, 2);
    lcd.setCursor(0, 0);
    lcd.print("MONITOR HIDRICO");
    lcd.setCursor(0, 1);
    lcd.print("SABANA CENTRO...");

    // Inicializar Sensor BMP280 (Dirección 0x76 o 0x77)
    unsigned status = bmp.begin(0x76);
    if (!status) {
        status = bmp.begin(0x77);
    }
    if (status) {
        bmpAvailable = true;
        bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,
                        Adafruit_BMP280::SAMPLING_X2,
                        Adafruit_BMP280::SAMPLING_X16,
                        Adafruit_BMP280::FILTER_X16,
                        Adafruit_BMP280::STANDBY_MS_500);
        Serial.println("[OK] Sensor BMP280 detectado y configurado exitosamente.");
    } else {
        Serial.println("[WARN] BMP280 no detectado. Usando temperatura de respaldo (20.0 C) y presion de respaldo (1013.25 hPa).");
        bmpAvailable = false;
    }

    delay(1500);
    lcd.clear();
    Serial.println("[OK] Setup completado exitosamente.\n");
}

// ============================================================================
// LOOP PRINCIPAL (No Bloqueante)
// ============================================================================
void loop() {
    unsigned long currentMillis = millis();

    // Mantener la señal estroboscópica en caso de Emergencia Crítica (Rojo/Azul alternando)
    if (systemRiskState == 3) {
        bool strobe = (currentMillis / 150) % 2;
        digitalWrite(PIN_LED_RED, strobe);
        digitalWrite(PIN_LED_BLUE, !strobe);
    }

    // Ejecutar ciclo de muestreo periódico
    if (currentMillis - lastSamplingTime >= SAMPLE_INTERVAL_MS) {
        float elapsedTimeSec = (currentMillis - lastSamplingTime) / 1000.0f;
        lastSamplingTime = currentMillis;

        // 1. Cuantificación de Caudal Instantáneo desde ISR
        noInterrupts();
        unsigned long pulses = pulseCounter;
        pulseCounter = 0;
        interrupts();

        currentFlow_Lmin = iot::watermonitor::computeInstantFlowLMin(
            pulses, elapsedTimeSec, iot::watermonitor::FLOW_FACTOR
        );

        // 2. Adquisición Meteorológica (BMP280 - Temperatura y Presión)
        float tempC       = 20.0f;
        float pressureHpa = 1013.25f;

        if (bmpAvailable) {
            tempC       = bmp.readTemperature();
            pressureHpa = bmp.readPressure() / 100.0f;

            if (firstTempReading) {
                tempMax_24h = tempC + 4.0f;
                tempMin_24h = tempC - 4.0f;
                firstTempReading = false;
            } else {
                if (tempC > tempMax_24h) tempMax_24h = tempC;
                if (tempC < tempMin_24h) tempMin_24h = tempC;
            }
        }

        // 3. Medición de Nivel Corregido Termo-Acústicamente
        waterLevel_cm = readAcousticLevel(tempC);
        float levelPercent = (waterLevel_cm / iot::watermonitor::TANK_HEIGHT_CM) * 100.0f;

        // Auto-inicialización en el primer ciclo para evitar picos espurios de arranque
        if (firstLevelReading) {
            previousLevel_cm = waterLevel_cm;
            firstLevelReading = false;
        }

        // Tasa de descenso de nivel (cm/h)
        float dropRate_cm_h = (previousLevel_cm - waterLevel_cm) * (3600.0f / elapsedTimeSec);
        if (dropRate_cm_h < 0.0f) dropRate_cm_h = 0.0f;
        previousLevel_cm = waterLevel_cm;

        // 4. Estimación de Evapotranspiración ETo (Hargreaves-Samani)
        int dayOfYear = 180; // Día representativo de temporada seca
        calculatedETo = iot::watermonitor::calculateHargreavesETo(dayOfYear, tempMin_24h, tempMax_24h);

        // 5. Evaluación de la Matriz de Fusión Multisensorial
        systemRiskState = iot::watermonitor::evaluateRisk(
            levelPercent, dropRate_cm_h, calculatedETo, currentFlow_Lmin
        );

        // 6. Actuación Óptica y Acústica In-Situ
        applyAlertOutputs(systemRiskState);

        // 7. Renderizado en Display LCD 1602A
        char line0[17];
        char line1[17];
        snprintf(line0, sizeof(line0), "Niv:%3.0f%% Q:%3.1fL", levelPercent, currentFlow_Lmin);
        snprintf(line1, sizeof(line1), "EST: %-11s", iot::watermonitor::riskLabel(systemRiskState));

        lcd.setCursor(0, 0);
        lcd.print(line0);
        lcd.setCursor(0, 1);
        lcd.print(line1);

        // 8. Telemetría Serial Formateada
        Serial.printf(
            "| Nivel: %5.1f cm (%3.0f%%) | Caudal: %4.1f L/min | Temp: %4.1f C | Pres: %7.2f hPa | ETo: %3.1f mm/d | Estado: %s |\n",
            waterLevel_cm, levelPercent, currentFlow_Lmin, tempC, pressureHpa, calculatedETo, iot::watermonitor::riskLabel(systemRiskState)
        );
    }
}
