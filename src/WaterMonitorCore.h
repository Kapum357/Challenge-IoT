#ifndef WATER_MONITOR_CORE_H
#define WATER_MONITOR_CORE_H

namespace iot {
namespace watermonitor {

constexpr float TANK_HEIGHT_CM = 300.0f;
constexpr float LATITUDE_RAD = 0.08395f;
constexpr float FLOW_FACTOR = 7.5f;

float computeSoundSpeed(float ambientTempC);
float measureCorrectedLevel(float ambientTempC, long durationUs);
float calculateHargreavesETo(int dayOfYear, float tMinC, float tMaxC);
float computeInstantFlowLMin(unsigned long pulseCount, float elapsedSeconds, float flowFactor = FLOW_FACTOR);
int evaluateRisk(float levelPercent, float dropRateCmPerHour, float etoMmDay, float flowLMin);
const char* riskLabel(int riskState);

} // namespace watermonitor
} // namespace iot

#endif // WATER_MONITOR_CORE_H
