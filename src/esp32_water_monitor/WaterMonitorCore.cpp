#include "WaterMonitorCore.h"

#include <algorithm>
#include <cmath>

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

float computeInstantFlowLMin(unsigned long pulseCount, float elapsedSeconds, float flowFactor) {
    if (elapsedSeconds <= 0.0f || flowFactor <= 0.0f) {
        return 0.0f;
    }

    const float pulsesPerSecond = static_cast<float>(pulseCount) / elapsedSeconds;
    return pulsesPerSecond / flowFactor;
}

/**
 * CORRECCIÓN Ch2: Matriz de fusión mejorada.
 *
 * Cambios respecto a Ch1:
 * 1. El estado "NORMAL" (nivel >= 70%) AHORA VERIFICA dH/dt.
 *    Si dropRate >= 10 cm/h con nivel alto → ALERTA (2).
 *    Si dropRate >= 5 cm/h con nivel alto → ESTRÉS (1).
 *
 * 2. Detección de fugas: si flowLMin < 1.0 (casi sin salida medida)
 *    pero dropRate >= 3.0 (nivel bajando rápidamente) → ALERTA (2).
 *    Esto indica pérdida de agua no explicada por caudal de salida.
 */
int evaluateRisk(float levelPercent, float dropRateCmPerHour, float etoMmDay, float flowLMin) {
    // 3: Emergencia Crítica (Vaciado severo o nivel < 25%)
    if (levelPercent < 25.0f || dropRateCmPerHour >= 10.0f) {
        return 3;
    }

    // Detección de fugas: bajo caudal de salida + descenso anómalo de nivel
    // Indica pérdida de agua no explicada por el flujo medido (fisura, fuga en tubería, etc.)
    if (flowLMin < 1.0f && dropRateCmPerHour >= 3.0f) {
        return 2; // ALERTA — posible fuga
    }

    // 2: Alerta Severa (Nivel 25-50% o caída rápida con alta evaporación)
    if ((levelPercent >= 25.0f && levelPercent < 50.0f) ||
        (dropRateCmPerHour >= 5.0f && etoMmDay > 5.0f)) {
        return 2;
    }

    // 1: Estrés Moderado (Nivel 50-70% o alta ETo o alto caudal sostenido)
    if ((levelPercent >= 50.0f && levelPercent < 70.0f) || etoMmDay >= 4.5f || flowLMin > 20.0f) {
        return 1;
    }

    // CORRECCIÓN Ch2: Nivel >= 70% (zona "normal") PERO con descenso rápido
    // Bug del Ch1: esta verificación no existía, retornaba 0 directamente
    if (dropRateCmPerHour >= 5.0f) {
        return 1; // ESTRÉS — descenso significativo aun con nivel alto
    }

    // 0: Operación Normal (nivel >= 70%, sin anomalías)
    return 0;
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

} // namespace watermonitor
} // namespace iot
