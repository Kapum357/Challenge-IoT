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

float computeSoundSpeed(float ambientTempC) {
    return 331.3f + 0.606f * ambientTempC;
}

float measureCorrectedLevel(float ambientTempC, long durationUs) {
    if (durationUs <= 0) {
        return TANK_HEIGHT_CM;
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

int evaluateRisk(float levelPercent, float dropRateCmPerHour, float etoMmDay, float flowLMin) {
    // 3: Emergencia Crítica (Vaciado severo o nivel < 25%)
    if (levelPercent < 25.0f || dropRateCmPerHour >= 10.0f) {
        return 3;
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

    // 0: Operación Normal
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
