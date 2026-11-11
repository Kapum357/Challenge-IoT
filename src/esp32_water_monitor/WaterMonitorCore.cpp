#include "WaterMonitorCore.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace iot {
namespace watermonitor {

namespace {
constexpr double PI = 3.14159265358979323846;

float clampUnitInterval(float value) {
    return std::max(-1.0f, std::min(1.0f, value));
}
} // namespace

// ============================================================================
// Implementación de SramRingBuffer
// ============================================================================
SramRingBuffer::SramRingBuffer() : head_(0), count_(0) {}

void SramRingBuffer::push(const TelemetryRecord& record) {
    buffer_[head_] = record;
    head_ = (head_ + 1) % CAPACITY;
    if (count_ < CAPACITY) {
        count_++;
    }
}

size_t SramRingBuffer::size() const {
    return count_;
}

size_t SramRingBuffer::capacity() const {
    return CAPACITY;
}

bool SramRingBuffer::isFull() const {
    return count_ == CAPACITY;
}

void SramRingBuffer::clear() {
    head_ = 0;
    count_ = 0;
}

bool SramRingBuffer::get(size_t index, TelemetryRecord& outRecord) const {
    if (index >= count_) return false;
    outRecord = buffer_[(count_ < CAPACITY) ? index : (head_ + index) % CAPACITY];
    return true;
}

bool SramRingBuffer::getLatest(TelemetryRecord& outRecord) const {
    if (count_ == 0) return false;
    outRecord = buffer_[(head_ + CAPACITY - 1) % CAPACITY];
    return true;
}


// ============================================================================
// Implementación de RiskStateMachine (Histéresis Temporal)
// ============================================================================
RiskStateMachine::RiskStateMachine()
    : stableState_(0), candidateState_(0), consecutiveCount_(0) {}

int RiskStateMachine::update(int rawRisk) {
    if (rawRisk == stableState_) {
        // Riesgo instantáneo coincide con estado estable → reiniciar contador
        candidateState_ = stableState_;
        consecutiveCount_ = 0;
        return stableState_;
    }

    if (rawRisk == candidateState_) {
        // Misma dirección consecutiva
        consecutiveCount_++;
    } else {
        // Cambio de dirección → reiniciar con nuevo candidato
        candidateState_ = rawRisk;
        consecutiveCount_ = 1;
    }

    // Determinar umbral según dirección de cambio
    int threshold = (rawRisk > stableState_)
                        ? ESCALATE_THRESHOLD
                        : DEESCALATE_THRESHOLD;

    if (consecutiveCount_ >= threshold) {
        stableState_ = candidateState_;
        consecutiveCount_ = 0;
    }

    return stableState_;
}

int RiskStateMachine::stableState() const {
    return stableState_;
}

void RiskStateMachine::reset() {
    stableState_ = 0;
    candidateState_ = 0;
    consecutiveCount_ = 0;
}

// ============================================================================
// Implementación de Validación de Subred
// ============================================================================
bool isSubnetAllowed(uint32_t clientIp, uint32_t localIp, uint32_t subnetMask) {
    return (clientIp & subnetMask) == (localIp & subnetMask);
}

// ============================================================================
// Algoritmos de Borde Físicos e Hidrometeorológicos
// ============================================================================
float computeSoundSpeed(float ambientTempC) {
    return 331.3f + 0.606f * ambientTempC;
}

/**
 * CORRECCIÓN Ch2 (fail-safe):
 * Antes: durationUs <= 0 → retornaba TANK_HEIGHT_CM (tanque 100% lleno) — PELIGROSO
 * Ahora: durationUs <= 0 → retorna LEVEL_SENSOR_ERROR (-1.0f) — fail-safe
 * El firmware DEBE verificar retorno < 0 y mantener último valor válido.
 */
float measureCorrectedLevel(float ambientTempC, long durationUs) {
    if (durationUs <= 0) {
        return LEVEL_SENSOR_ERROR;  // Fail-safe: nunca inventar "100% lleno"
    }

    const float speedOfSoundCmUs = (computeSoundSpeed(ambientTempC) * 100.0f) / 1000000.0f;
    const float distanceCm = (durationUs * speedOfSoundCmUs) / 2.0f;

    float levelCm = TANK_HEIGHT_CM - distanceCm;
    if (levelCm < 0.0f) {
        levelCm = 0.0f;
    }
    if (levelCm > TANK_HEIGHT_CM) {
        levelCm = TANK_HEIGHT_CM;
    }

    return levelCm;
}

float calculateHargreavesETo(int dayOfYear, float tMinC, float tMaxC) {
    if (tMaxC <= tMinC) {
        return 0.0f;
    }

    if (dayOfYear < 1) dayOfYear = 1;
    if (dayOfYear > 366) dayOfYear = 366;

    const float tMean = (tMinC + tMaxC) / 2.0f;
    const float dr = 1.0f + 0.033f * std::cos((2.0f * PI / 365.0f) * static_cast<double>(dayOfYear));
    const float delta = 0.409f * std::sin((2.0f * PI / 365.0f) * static_cast<double>(dayOfYear) - 1.39f);
    const float tanLatitude = std::tan(LATITUDE_RAD);
    const float tanDelta = std::tan(delta);
    const float safeArgument = clampUnitInterval(-tanLatitude * tanDelta);
    const float ws = std::acos(safeArgument);

    // Radiación extraterrestre Ra en MJ/(m^2*día)
    const float ra = (24.0f * 60.0f / PI) * 0.0820f * dr *
        (ws * std::sin(LATITUDE_RAD) * std::sin(delta) +
         std::cos(LATITUDE_RAD) * std::cos(delta) * std::sin(ws));

    // ETo = 0.0023 * 0.408 * Ra * (Tmean + 17.8) * sqrt(Tmax - Tmin)
    return 0.0023f * 0.408f * ra * (tMean + 17.8f) * std::sqrt(tMaxC - tMinC);
}

#if ENABLE_FLOW_SENSOR
float computeInstantFlowLMin(unsigned long pulseCount, float elapsedSeconds, float flowFactor) {
    if (elapsedSeconds <= 0.0f || flowFactor <= 0.0f) {
        return 0.0f;
    }

    const float pulsesPerSecond = static_cast<float>(pulseCount) / elapsedSeconds;
    return pulsesPerSecond / flowFactor;
}
#endif

/**
 * Matriz de fusión multisensorial de borde.
 * En Challenge #3 (ENABLE_FLOW_SENSOR == 0), no se evalúa caudal físico,
 * el diagnóstico se basa en nivel acústico compensado, dH/dt y ETo.
 */
int evaluateRisk(float levelPercent, float dropRateCmPerHour, float etoMmDay, float flowLMin) {
#if !ENABLE_FLOW_SENSOR
    (void)flowLMin;
#endif
    // 3: Emergencia Crítica (Vaciado severo o nivel < 25%)
    if (levelPercent < 25.0f || dropRateCmPerHour >= 10.0f) {
        return 3;
    }

#if ENABLE_FLOW_SENSOR
    // Detección de fugas con caudalímetro: bajo caudal de salida + descenso anómalo
    if (flowLMin < 1.0f && dropRateCmPerHour >= 3.0f) {
        return 2; // ALERTA — posible fuga
    }
#endif

    // 2: Alerta Severa (Nivel 25-50% o caída rápida con alta evaporación)
    if ((levelPercent >= 25.0f && levelPercent < 50.0f) ||
        (dropRateCmPerHour >= 5.0f && etoMmDay > 5.0f)) {
        return 2;
    }

    // 1: Estrés Moderado (Nivel 50-70% o alta ETo o alto caudal sostenido)
    if ((levelPercent >= 50.0f && levelPercent < 70.0f) || etoMmDay >= 4.5f
#if ENABLE_FLOW_SENSOR
        || flowLMin > 20.0f
#endif
    ) {
        return 1;
    }

    // Nivel >= 70% (zona normal) PERO con descenso rápido
    if (dropRateCmPerHour >= 5.0f) {
        return 1; // ESTRÉS — descenso significativo aun con nivel alto
    }

    // 0: Operación Normal (nivel >= 70%, sin anomalías)
    return 0;
}

int evaluateRisk(float levelPercent, float dropRateCmPerHour, float etoMmDay) {
    return evaluateRisk(levelPercent, dropRateCmPerHour, etoMmDay, 0.0f);
}

const char* riskLabel(int riskState) {
    switch (riskState) {
        case 0:
            return "NORMAL";
        case 1:
            return "ESTRES MOD";
        case 2:
            return "ALERTA SEV";
        case 3:
            return "EMERGENCIA!";
        default:
            return "DESCONOCIDO";
    }
}

// ============================================================================
// Implementación de Analítica Cuantitativa Compartida Gateway & ESP32
// ============================================================================
float computeVpdKpa(float tempC, float rhPct) {
    if (tempC < -40.0f || rhPct < 0.0f) {
        return -1.0f;
    }
    const float boundedRh = std::max(0.0f, std::min(100.0f, rhPct));
    const float es = 0.61078f * std::exp((17.27f * tempC) / (tempC + 237.3f));
    const float ea = es * (boundedRh / 100.0f);
    return std::round((es - ea) * 1000.0f) / 1000.0f;
}

float computeZScore(float currentDrop, float meanDrop, float stdDrop) {
    const float effectiveStd = (stdDrop > 1e-6f) ? stdDrop : 0.5f;
    return std::round(((currentDrop - meanDrop) / effectiveStd) * 100.0f) / 100.0f;
}

float computeOlsSlope(const float* timeHours, const float* levelsCm, size_t count) {
    if (!timeHours || !levelsCm || count < 2) {
        return 0.0f;
    }
    float xSum = 0.0f;
    float ySum = 0.0f;
    for (size_t i = 0; i < count; ++i) {
        xSum += timeHours[i];
        ySum += levelsCm[i];
    }
    const float xMean = xSum / static_cast<float>(count);
    const float yMean = ySum / static_cast<float>(count);
    float num = 0.0f;
    float den = 0.0f;
    for (size_t i = 0; i < count; ++i) {
        const float xDiff = timeHours[i] - xMean;
        num += xDiff * (levelsCm[i] - yMean);
        den += xDiff * xDiff;
    }
    if (den <= 1e-6f) {
        return 0.0f;
    }
    return std::round((num / den) * 1000.0f) / 1000.0f;
}

float computeHoursToCritical(float currentLevelCm, float effectiveDescentRateCmH, float criticalLevelCm) {
    if (currentLevelCm < 0.0f) {
        return -1.0f;
    }
    if (currentLevelCm <= criticalLevelCm) {
        return 0.0f;
    }
    if (effectiveDescentRateCmH > 0.05f) {
        return std::round(((currentLevelCm - criticalLevelCm) / effectiveDescentRateCmH) * 10.0f) / 10.0f;
    }
    return 999.0f;
}

std::string formatTelemetryJson(const TelemetryRecord& record,
                                const char* nodoId,
                                const char* timestampUtc,
                                bool alarmSilenced) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2);
    ss << "{\n";
    ss << "  \"nodo_id\": \"" << (nodoId ? nodoId : "nodo01") << "\",\n";
    ss << "  \"timestamp_utc\": \"" << (timestampUtc ? timestampUtc : "2026-10-28T12:00:00Z") << "\",\n";
    ss << "  \"level_cm\": " << record.levelCm << ",\n";
    ss << "  \"level_pct\": " << record.levelPercent << ",\n";
#if ENABLE_FLOW_SENSOR
    ss << "  \"flow_l_min\": " << record.flowLMin << ",\n";
#endif
    ss << "  \"drop_rate_cm_h\": " << record.dropRateCmH << ",\n";
    ss << "  \"temp_c\": " << record.tempC << ",\n";
    ss << "  \"humidity_pct\": " << record.humidityPct << ",\n";
    ss << "  \"pressure_hpa\": " << record.pressureHpa << ",\n";
    ss << "  \"eto_mm_day\": " << record.etoMmDay << ",\n";
    ss << "  \"edge_risk_state\": " << static_cast<int>(record.riskState) << ",\n";
    ss << "  \"alarm_silenced_ack\": " << (alarmSilenced ? "true" : "false") << "\n";
    ss << "}";
    return ss.str();
}

} // namespace watermonitor
} // namespace iot
