#ifndef WATER_MONITOR_CORE_H
#define WATER_MONITOR_CORE_H

#include <cstddef>
#include <cstdint>

namespace iot {
namespace watermonitor {

// ============================================================================
// Constantes físicas y de calibración para Sabana Centro (Escala MVP Laboratorio)
// Challenge #2: Evolución con histéresis, detección de fugas y fail-safe
// ============================================================================
constexpr float TANK_HEIGHT_CM = 50.0f;  // Altura nominal de reservorio/recipiente en maqueta de laboratorio (cm)
constexpr float LATITUDE_RAD = 0.08395f; // Latitud Sabana Centro (4.81° N en radianes)
constexpr float FLOW_FACTOR = 7.5f;      // Factor de calibración YF-S201: F (Hz) = 7.5 * Q (L/min)
constexpr size_t RING_BUFFER_CAPACITY = 360; // Capacidad de telemetría histórica en SRAM

// Sentinel para indicar fallo de medición (eco inválido del HC-SR04)
constexpr float LEVEL_SENSOR_ERROR = -1.0f;

/**
 * @brief Estructura empacada de telemetría periódica para almacenamiento en SRAM y API REST.
 *        Optimizado con alineamiento de 1 byte para minimizar memoria en ESP32.
 */
#pragma pack(push, 1)
struct TelemetryRecord {
    uint32_t uptimeSec;   // Segundos de operación activa del nodo
    float levelCm;        // Nivel absoluto en cm
    float levelPercent;   // Porcentaje de llenado (0 a 100%)
    float flowLMin;       // Caudal de descarga instantáneo (L/min)
    float dropRateCmH;    // Tasa de descenso de nivel (cm/h)
    float tempC;          // Temperatura ambiente BME280 (°C)
    float pressureHpa;    // Presión barométrica BME280 (hPa); -1.0 indica N/D
    float humidityPct;    // Humedad relativa BME280 (% HR)
    float etoMmDay;       // Evapotranspiración ETo Hargreaves-Samani (mm/día)
    uint8_t riskState;    // Estado de riesgo (0: NORMAL, 1: ESTRES, 2: ALERTA, 3: EMERGENCIA)
};
#pragma pack(pop)

/**
 * @brief Buffer circular determinístico en SRAM para series temporales de telemetría.
 *        Capacidad fija de 360 registros con ordenamiento cronológico garantizado O(1).
 */
class SramRingBuffer {
public:
    static constexpr size_t CAPACITY = RING_BUFFER_CAPACITY;

    SramRingBuffer();

    void push(const TelemetryRecord& record);
    size_t size() const;
    size_t capacity() const;
    bool isFull() const;
    void clear();

    /**
     * @brief Obtiene un registro por índice cronológico (0 = más antiguo, size()-1 = más reciente).
     */
    bool get(size_t index, TelemetryRecord& outRecord) const;

    /**
     * @brief Obtiene el registro más recientemente insertado.
     */
    bool getLatest(TelemetryRecord& outRecord) const;

private:
    TelemetryRecord buffer_[CAPACITY];
    size_t head_;
    size_t count_;
};

/**
 * @brief Máquina de estados de riesgo con histéresis temporal.
 *
 * Previene falsos positivos y falsos negativos al requerir persistencia
 * de N muestras consecutivas antes de escalar o des-escalar el estado de riesgo.
 *
 * - Escalamiento (subir riesgo): requiere >= ESCALATE_THRESHOLD muestras consecutivas
 *   en un nivel de riesgo más alto que el estado estable actual.
 * - Des-escalamiento (bajar riesgo): requiere >= DEESCALATE_THRESHOLD muestras consecutivas
 *   en un nivel de riesgo más bajo que el estado estable actual.
 *
 * Esto evita que una lectura aislada cambie el estado del sistema.
 */
class RiskStateMachine {
public:
    static constexpr int ESCALATE_THRESHOLD = 3;   // Muestras para subir
    static constexpr int DEESCALATE_THRESHOLD = 5;  // Muestras para bajar

    RiskStateMachine();

    /**
     * @brief Procesa una nueva evaluación instantánea de riesgo y retorna el estado estable.
     * @param rawRisk Evaluación de riesgo instantánea (0-3) de evaluateRisk().
     * @return Estado de riesgo estable con histéresis aplicada (0-3).
     */
    int update(int rawRisk);

    /**
     * @brief Estado estable actual.
     */
    int stableState() const;

    /**
     * @brief Reinicia la máquina al estado NORMAL (0).
     */
    void reset();

private:
    int stableState_;
    int candidateState_;
    int consecutiveCount_;
};

/**
 * @brief Valida si una dirección IP de cliente pertenece a la subred autorizada del nodo.
 *        Aplica el filtrado canónico a nivel de bit: (clientIp & subnetMask) == (localIp & subnetMask).
 */
bool isSubnetAllowed(uint32_t clientIp, uint32_t localIp, uint32_t subnetMask);

/**
 * @brief Calcula la velocidad del sonido en aire en función de la temperatura ambiente.
 * @param ambientTempC Temperatura ambiente en grados Celsius.
 * @return Velocidad del sonido en m/s.
 */
float computeSoundSpeed(float ambientTempC);

/**
 * @brief Medición de nivel con compensación termo-acústica del tiempo de vuelo.
 *
 * CORRECCIÓN Ch2: Modo fail-safe. Si durationUs <= 0 (eco inválido), retorna
 * LEVEL_SENSOR_ERROR (-1.0f) en lugar de TANK_HEIGHT_CM (100% lleno).
 * El llamador DEBE verificar el valor de retorno antes de usarlo.
 *
 * @param ambientTempC Temperatura ambiente (°C).
 * @param durationUs Tiempo de vuelo del pulso de eco en microsegundos.
 * @return Nivel de agua medido en centímetros (0 a TANK_HEIGHT_CM), o LEVEL_SENSOR_ERROR si eco inválido.
 */
float measureCorrectedLevel(float ambientTempC, long durationUs);

/**
 * @brief Modelo astronómico de Hargreaves-Samani para Evapotranspiración de Referencia (ETo).
 * @param dayOfYear Día del año (1 a 365/366).
 * @param tMinC Temperatura mínima registrada en 24h (°C).
 * @param tMaxC Temperatura máxima registrada en 24h (°C).
 * @return ETo estimada en mm/día.
 */
float calculateHargreavesETo(int dayOfYear, float tMinC, float tMaxC);

/**
 * @brief Cuantificación del caudal volumétrico instantáneo a partir de pulsos Hall.
 * @param pulseCount Conteo de pulsos registrados en el intervalo.
 * @param elapsedSeconds Intervalo de tiempo transcurrido en segundos.
 * @param flowFactor Constante del sensor (nominal 7.5 Hz por L/min).
 * @return Caudal en Litros por minuto (L/min).
 */
float computeInstantFlowLMin(unsigned long pulseCount, float elapsedSeconds, float flowFactor = FLOW_FACTOR);

/**
 * @brief Matriz de decisión y fusión multisensorial de borde.
 *
 * CORRECCIÓN Ch2:
 * 1. El estado "NORMAL" ahora SÍ verifica dH/dt (dropRate). Si el nivel es alto (>=70%)
 *    pero hay descenso rápido (>=5 cm/h), escala a ESTRÉS o ALERTA.
 * 2. Detección de fugas: si flow < 1.0 L/min pero dropRate >= 3.0 cm/h,
 *    señal de anomalía (posible fuga) → escala a ALERTA.
 *
 * @param levelPercent Porcentaje de nivel de agua (0% a 100%).
 * @param dropRateCmPerHour Tasa de descenso de nivel (cm/h).
 * @param etoMmDay Evapotranspiración calculada (mm/día).
 * @param flowLMin Caudal de descarga medido (L/min).
 * @return Estado de riesgo: 0 (NORMAL), 1 (ESTRES), 2 (ALERTA), 3 (EMERGENCIA).
 */
int evaluateRisk(float levelPercent, float dropRateCmPerHour, float etoMmDay, float flowLMin);

/**
 * @brief Retorna la etiqueta textual del estado de riesgo.
 */
const char* riskLabel(int riskState);

} // namespace watermonitor
} // namespace iot

#endif // WATER_MONITOR_CORE_H
