#ifndef WATER_MONITOR_CORE_H
#define WATER_MONITOR_CORE_H

namespace iot {
namespace watermonitor {

// Constantes físicas y de calibración para Sabana Centro (Escala MVP Laboratorio)
constexpr float TANK_HEIGHT_CM = 50.0f;  // Altura nominal de reservorio/recipiente en maqueta de laboratorio (cm)
constexpr float LATITUDE_RAD = 0.08395f; // Latitud Sabana Centro (4.81° N en radianes)
constexpr float FLOW_FACTOR = 7.5f;     // Factor de calibración YF-S201: F (Hz) = 7.5 * Q (L/min)

/**
 * @brief Calcula la velocidad del sonido en aire en función de la temperatura ambiente.
 * @param ambientTempC Temperatura ambiente en grados Celsius.
 * @return Velocidad del sonido en m/s.
 */
float computeSoundSpeed(float ambientTempC);

/**
 * @brief Medición de nivel con compensación termo-acústica del tiempo de vuelo.
 * @param ambientTempC Temperatura ambiente (°C).
 * @param durationUs Tiempo de vuelo del pulso de eco en microsegundos.
 * @return Nivel de agua medido en centímetros (0 a TANK_HEIGHT_CM).
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
