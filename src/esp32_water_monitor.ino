#include "WaterMonitorCore.h"

// This file is a lightweight ESP32 wrapper for the logic described in the engineering design.
// The core calculations are implemented in the portable C++ module so they can be compiled
// and validated outside a hardware-embedded toolchain.

void setup() {
    Serial.begin(115200);
    Serial.println("ESP32 water-monitor: booting");
}

void loop() {
    const float ambientTempC = 24.5f;
    const long echoDurationUs = 12345L;
    const float levelCm = iot::watermonitor::measureCorrectedLevel(ambientTempC, echoDurationUs);
    const float levelPercent = (levelCm / iot::watermonitor::TANK_HEIGHT_CM) * 100.0f;
    const float etoMmDay = iot::watermonitor::calculateHargreavesETo(180, 18.0f, 28.0f);
    const float flowLMin = iot::watermonitor::computeInstantFlowLMin(15UL, 2.0f);
    const int riskState = iot::watermonitor::evaluateRisk(levelPercent, 2.3f, etoMmDay, flowLMin);

    Serial.print("Level: ");
    Serial.print(levelPercent);
    Serial.print("% | Flow: ");
    Serial.print(flowLMin);
    Serial.print(" L/min | ETo: ");
    Serial.print(etoMmDay);
    Serial.print(" mm/day | Risk: ");
    Serial.println(iot::watermonitor::riskLabel(riskState));

    delay(2000);
}
