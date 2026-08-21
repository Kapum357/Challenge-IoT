#include "WaterMonitorCore.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

void expectNear(float actual, float expected, float tolerance, const char* label) {
    const float diff = std::fabs(actual - expected);
    if (diff > tolerance) {
        std::cerr << "[FAIL] " << label << ": esperado " << expected << ", obtenido " << actual
                  << " (delta=" << diff << ", tol=" << tolerance << ")\n";
        std::exit(1);
    }
    std::cout << "[PASS] " << label << " = " << actual << " (esperado " << expected << ")\n";
}

void expectInt(int actual, int expected, const char* label) {
    if (actual != expected) {
        std::cerr << "[FAIL] " << label << ": esperado " << expected << ", obtenido " << actual << "\n";
        std::exit(1);
    }
    std::cout << "[PASS] " << label << " = " << actual << " (" << iot::watermonitor::riskLabel(actual) << ")\n";
}

} // namespace

int main() {
    std::cout << "==================================================\n";
    std::cout << " EJECUTANDO SUITE DE PRUEBAS UNITARIAS: CHALLENGE 1\n";
    std::cout << " Sistema de Monitoreo Hidrologico Sabana Centro   \n";
    std::cout << "==================================================\n\n";

    // 1. Compensación Termo-Acústica
    expectNear(iot::watermonitor::computeSoundSpeed(25.0f), 346.45f, 0.05f,
               "Velocidad del sonido a 25 C");
    expectNear(iot::watermonitor::computeSoundSpeed(0.0f), 331.30f, 0.05f,
               "Velocidad del sonido a 0 C");
    expectNear(iot::watermonitor::measureCorrectedLevel(25.0f, 1000L), 32.6775f, 0.5f,
               "Nivel corregido a 25 C con eco de 1000 us (Escala 50 cm)");

    // 2. Caudal Instantáneo Hall (YF-S201)
    expectNear(iot::watermonitor::computeInstantFlowLMin(15UL, 2.0f, 7.5f), 1.0f, 0.05f,
               "Caudal instantaneo (15 pulsos / 2s)");
    expectNear(iot::watermonitor::computeInstantFlowLMin(75UL, 1.0f, 7.5f), 10.0f, 0.05f,
               "Caudal instantaneo (75 pulsos / 1s)");

    // 3. Evapotranspiración Hargreaves-Samani en Sabana Centro (Lat 4.81 N)
    const float eto = iot::watermonitor::calculateHargreavesETo(180, 18.0f, 28.0f);
    if (!std::isfinite(eto) || eto <= 0.0f) {
        std::cerr << "[FAIL] calculateHargreavesETo retorno un valor no valido: " << eto << "\n";
        return 1;
    }
    std::cout << "[PASS] ETo Hargreaves-Samani (Dia 180, 18-28 C) = " << eto << " mm/dia\n";

    // 4. Matriz de Fusión Multisensorial de Riesgo
    expectInt(iot::watermonitor::evaluateRisk(85.0f, 1.0f, 2.5f, 5.0f), 0,
              "Matriz de Riesgo: Escenario Normal");
    expectInt(iot::watermonitor::evaluateRisk(60.0f, 2.0f, 4.8f, 10.0f), 1,
              "Matriz de Riesgo: Escenario Estres Moderado");
    expectInt(iot::watermonitor::evaluateRisk(35.0f, 6.0f, 5.2f, 15.0f), 2,
              "Matriz de Riesgo: Escenario Alerta Severa");
    expectInt(iot::watermonitor::evaluateRisk(15.0f, 12.0f, 6.0f, 25.0f), 3,
              "Matriz de Riesgo: Escenario Emergencia Critica");

    std::cout << "\n==================================================\n";
    std::cout << " TODAS LAS VERIFICACIONES MATEMATICAS PASARON OK   \n";
    std::cout << "==================================================\n";
    return 0;
}
