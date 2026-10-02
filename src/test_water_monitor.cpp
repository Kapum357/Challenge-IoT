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

void expectTrue(bool condition, const char* label) {
    if (!condition) {
        std::cerr << "[FAIL] " << label << ": se esperaba true pero fue false\n";
        std::exit(1);
    }
    std::cout << "[PASS] " << label << " = TRUE\n";
}

// Conversor auxiliar para pruebas de IP en notacion dot-decimal a uint32_t
uint32_t ipToUint(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return (static_cast<uint32_t>(a) << 24) |
           (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(c) << 8)  |
           static_cast<uint32_t>(d);
}

} // namespace

int main() {
    std::cout << "========================================================\n";
    std::cout << " EJECUTANDO SUITE DE PRUEBAS UNITARIAS: CHALLENGE 2     \n";
    std::cout << " Sistema de Monitoreo Hidrologico Sabana Centro (BME280)\n";
    std::cout << " Incluye correcciones de bugs del Ch1 + nuevas funciones\n";
    std::cout << "========================================================\n\n";

    // ========================================================================
    // 1. Compensación Termo-Acústica
    // ========================================================================
    std::cout << "--- 1. Compensacion Termo-Acustica ---\n";
    expectNear(iot::watermonitor::computeSoundSpeed(25.0f), 346.45f, 0.05f,
               "Velocidad del sonido a 25 C");
    expectNear(iot::watermonitor::computeSoundSpeed(0.0f), 331.30f, 0.05f,
               "Velocidad del sonido a 0 C");
    expectNear(iot::watermonitor::computeSoundSpeed(-20.0f), 319.18f, 0.05f,
               "Velocidad sonido a -20 C");
    expectNear(iot::watermonitor::computeSoundSpeed(50.0f), 361.60f, 0.05f,
               "Velocidad sonido a +50 C");

    expectNear(iot::watermonitor::measureCorrectedLevel(25.0f, 1000L), 32.6775f, 0.5f,
               "Nivel corregido a 25 C con eco de 1000 us (Escala 50 cm)");

    // ========================================================================
    // 2. CORRECCIÓN CH2: Fail-safe en measureCorrectedLevel
    //    Bug Ch1: durationUs <= 0 retornaba TANK_HEIGHT_CM (100% lleno) → PELIGROSO
    //    Ch2: retorna LEVEL_SENSOR_ERROR (-1.0f) → fail-safe
    // ========================================================================
    std::cout << "\n--- 2. CORRECCION CH2: Fail-safe measureCorrectedLevel ---\n";
    expectNear(iot::watermonitor::measureCorrectedLevel(20.0f, 0L),
               iot::watermonitor::LEVEL_SENSOR_ERROR, 0.01f,
               "Eco invalido (duration=0) retorna LEVEL_SENSOR_ERROR (-1.0)");
    expectNear(iot::watermonitor::measureCorrectedLevel(20.0f, -5L),
               iot::watermonitor::LEVEL_SENSOR_ERROR, 0.01f,
               "Eco invalido (duration=-5) retorna LEVEL_SENSOR_ERROR (-1.0)");
    expectNear(iot::watermonitor::measureCorrectedLevel(20.0f, -100L),
               iot::watermonitor::LEVEL_SENSOR_ERROR, 0.01f,
               "Eco invalido (duration=-100) retorna LEVEL_SENSOR_ERROR (-1.0)");

    // Verificar que eco válido sigue funcionando normalmente
    float validLevel = iot::watermonitor::measureCorrectedLevel(20.0f, 500L);
    expectTrue(validLevel >= 0.0f && validLevel <= iot::watermonitor::TANK_HEIGHT_CM,
               "Eco valido (duration=500) retorna nivel en rango [0, TANK_HEIGHT]");

    // ========================================================================
    // 3. Caudal Instantáneo Hall (YF-S201)
    // ========================================================================
    std::cout << "\n--- 3. Caudal Instantaneo Hall (YF-S201) ---\n";
    expectNear(iot::watermonitor::computeInstantFlowLMin(15UL, 2.0f, 7.5f), 1.0f, 0.05f,
               "Caudal instantaneo (15 pulsos / 2s)");
    expectNear(iot::watermonitor::computeInstantFlowLMin(75UL, 1.0f, 7.5f), 10.0f, 0.05f,
               "Caudal instantaneo (75 pulsos / 1s)");
    expectNear(iot::watermonitor::computeInstantFlowLMin(0UL, 2.0f, 7.5f), 0.0f, 0.01f,
               "Caudal 0 pulsos = 0 L/min");
    expectNear(iot::watermonitor::computeInstantFlowLMin(10UL, 0.0f, 7.5f), 0.0f, 0.01f,
               "Caudal con elapsedSec=0 retorna 0 (guard)");

    // ========================================================================
    // 4. Evapotranspiración Hargreaves-Samani en Sabana Centro (Lat 4.81 N)
    // ========================================================================
    std::cout << "\n--- 4. Evapotranspiracion Hargreaves-Samani ---\n";
    const float eto = iot::watermonitor::calculateHargreavesETo(180, 18.0f, 28.0f);
    if (!std::isfinite(eto) || eto <= 0.0f) {
        std::cerr << "[FAIL] calculateHargreavesETo retorno un valor no valido: " << eto << "\n";
        return 1;
    }
    std::cout << "[PASS] ETo Hargreaves-Samani (Dia 180, 18-28 C) = " << eto << " mm/dia\n";

    expectNear(iot::watermonitor::calculateHargreavesETo(180, 25.0f, 20.0f), 0.0f, 0.01f,
               "ETo con Tmax <= Tmin retorna 0.0");
    float etoDay1 = iot::watermonitor::calculateHargreavesETo(1, 15.0f, 25.0f);
    float etoDay365 = iot::watermonitor::calculateHargreavesETo(365, 15.0f, 25.0f);
    expectTrue(std::isfinite(etoDay1) && etoDay1 > 0.0f, "ETo en dia 1 es finito y positivo");
    expectTrue(std::isfinite(etoDay365) && etoDay365 > 0.0f, "ETo en dia 365 es finito y positivo");

    // ========================================================================
    // 5. Matriz de Fusión Multisensorial de Riesgo (Escenarios Canónicos)
    // ========================================================================
    std::cout << "\n--- 5. Matriz de Fusion Multisensorial de Riesgo ---\n";
    expectInt(iot::watermonitor::evaluateRisk(85.0f, 1.0f, 2.5f, 5.0f), 0,
              "Matriz de Riesgo: Escenario Normal (85%, bajo descenso, bajo ETo)");
    expectInt(iot::watermonitor::evaluateRisk(60.0f, 2.0f, 4.8f, 10.0f), 1,
              "Matriz de Riesgo: Escenario Estres Moderado (60%, ETo alta)");
    expectInt(iot::watermonitor::evaluateRisk(35.0f, 6.0f, 5.2f, 15.0f), 2,
              "Matriz de Riesgo: Escenario Alerta Severa (35%)");
    expectInt(iot::watermonitor::evaluateRisk(15.0f, 12.0f, 6.0f, 25.0f), 3,
              "Matriz de Riesgo: Escenario Emergencia Critica (15%)");

    // ========================================================================
    // 6. CORRECCIÓN CH2: evaluateRisk — nivel alto + descenso rápido
    //    Bug Ch1: levelPercent=80%, dropRate=7 cm/h → retornaba 0 (NORMAL)
    //    Ch2: retorna >= 1 porque dH/dt >= 5 cm/h es significativo
    // ========================================================================
    std::cout << "\n--- 6. CORRECCION CH2: evaluateRisk con dH/dt en nivel alto ---\n";
    expectTrue(iot::watermonitor::evaluateRisk(80.0f, 7.0f, 3.0f, 5.0f) >= 1,
               "Bug Ch1 corregido: nivel 80% + dropRate 7 cm/h ya NO retorna NORMAL");
    expectTrue(iot::watermonitor::evaluateRisk(80.0f, 5.0f, 2.0f, 5.0f) >= 1,
               "Nivel 80% + dropRate exacto 5 cm/h escala a ESTRES");
    expectInt(iot::watermonitor::evaluateRisk(80.0f, 1.0f, 2.0f, 5.0f), 0,
              "Nivel 80% + dropRate bajo 1 cm/h sigue siendo NORMAL");
    
    // dropRate >= 10 siempre es emergencia independiente del nivel
    expectInt(iot::watermonitor::evaluateRisk(90.0f, 10.0f, 2.0f, 5.0f), 3,
              "Riesgo descenso critico 10 cm/h = 3 (EMERGENCIA) sin importar nivel");

    // ========================================================================
    // 7. NUEVO CH2: Detección de fugas (correlación Q ↔ dH/dt)
    //    Si flow < 1.0 L/min pero dropRate >= 3.0 cm/h → ALERTA (posible fuga)
    // ========================================================================
    std::cout << "\n--- 7. NUEVO CH2: Deteccion de fugas (Q vs dH/dt) ---\n";
    expectInt(iot::watermonitor::evaluateRisk(80.0f, 4.0f, 2.0f, 0.2f), 2,
              "Fuga detectada: flow=0.2, dropRate=4.0, nivel=80% -> ALERTA");
    expectInt(iot::watermonitor::evaluateRisk(75.0f, 3.5f, 1.5f, 0.5f), 2,
              "Fuga detectada: flow=0.5, dropRate=3.5, nivel=75% -> ALERTA");
    expectInt(iot::watermonitor::evaluateRisk(80.0f, 3.0f, 2.0f, 0.0f), 2,
              "Fuga detectada: flow=0.0, dropRate=3.0 exacto -> ALERTA");
    // Caso donde hay caudal significativo → no es fuga, descenso explicado por uso
    // dropRate=4.0 < 5.0 y flow=5.0 > 1.0, así que no aplica fuga ni dropRate alto → NORMAL
    expectInt(iot::watermonitor::evaluateRisk(80.0f, 4.0f, 2.0f, 5.0f), 0,
              "Con flow=5.0 y dropRate=4.0 -> descenso explicado por uso, NORMAL");
    // Pero con dropRate=5.0 y flow > 1.0 → ESTRÉS por dropRate alto (no fuga)
    expectInt(iot::watermonitor::evaluateRisk(80.0f, 5.0f, 2.0f, 5.0f), 1,
              "Con flow=5.0 y dropRate=5.0 -> ESTRES por dropRate alto, no fuga");

    // ========================================================================
    // 8. Transiciones exactas de frontera en la matriz de riesgo
    // ========================================================================
    std::cout << "\n--- 8. Transiciones de frontera en matriz de riesgo ---\n";
    expectInt(iot::watermonitor::evaluateRisk(25.0f, 1.0f, 2.0f, 5.0f), 2,
              "Riesgo frontera nivel 25.0% = 2 (ALERTA SEV)");
    expectInt(iot::watermonitor::evaluateRisk(24.9f, 1.0f, 2.0f, 5.0f), 3,
              "Riesgo frontera nivel 24.9% = 3 (EMERGENCIA)");
    expectInt(iot::watermonitor::evaluateRisk(50.0f, 1.0f, 2.0f, 5.0f), 1,
              "Riesgo frontera nivel 50.0% = 1 (ESTRES MOD)");
    expectInt(iot::watermonitor::evaluateRisk(49.9f, 1.0f, 2.0f, 5.0f), 2,
              "Riesgo frontera nivel 49.9% = 2 (ALERTA SEV)");
    expectInt(iot::watermonitor::evaluateRisk(70.0f, 1.0f, 2.0f, 5.0f), 0,
              "Riesgo frontera nivel 70.0% = 0 (NORMAL)");
    expectInt(iot::watermonitor::evaluateRisk(69.9f, 1.0f, 2.0f, 5.0f), 1,
              "Riesgo frontera nivel 69.9% = 1 (ESTRES MOD)");
    expectInt(iot::watermonitor::evaluateRisk(85.0f, 1.0f, 4.5f, 5.0f), 1,
              "Riesgo ETo alta 4.5 mm/d = 1 (ESTRES MOD)");
    expectInt(iot::watermonitor::evaluateRisk(85.0f, 1.0f, 2.0f, 20.1f), 1,
              "Riesgo caudal sostenido > 20 L/min = 1 (ESTRES MOD)");

    // ========================================================================
    // 9. NUEVO CH2: RiskStateMachine — Histéresis Temporal
    // ========================================================================
    std::cout << "\n--- 9. NUEVO CH2: RiskStateMachine (Histeresis Temporal) ---\n";
    iot::watermonitor::RiskStateMachine rsm;

    // Estado inicial
    expectInt(rsm.stableState(), 0, "RSM estado inicial es NORMAL (0)");

    // Una lectura aislada de riesgo alto NO debe cambiar el estado
    expectInt(rsm.update(2), 0, "RSM: 1 muestra ALERTA aislada NO cambia estado (sigue 0)");
    expectInt(rsm.update(0), 0, "RSM: vuelve a NORMAL, estado sigue 0");

    // 2 muestras consecutivas de EMERGENCIA tampoco alcanzan (threshold = 3)
    rsm.reset();
    rsm.update(3);
    expectInt(rsm.update(3), 0, "RSM: 2 muestras EMERGENCIA aun no escala (threshold=3)");

    // 3 muestras consecutivas SÍ escalan
    expectInt(rsm.update(3), 3, "RSM: 3 muestras EMERGENCIA -> escala a 3");

    // Ahora para des-escalar de 3 a 0 se necesitan 5 muestras
    rsm.update(0); // 1
    rsm.update(0); // 2
    rsm.update(0); // 3
    expectInt(rsm.update(0), 3, "RSM: 4 muestras NORMAL no des-escala (threshold=5)");
    expectInt(rsm.update(0), 0, "RSM: 5 muestras NORMAL des-escala a 0");

    // Interrupción de secuencia reinicia el contador
    rsm.reset();
    rsm.update(2); // 1 ALERTA
    rsm.update(2); // 2 ALERTA
    rsm.update(1); // Interrupción con ESTRÉS → reinicio
    expectInt(rsm.update(2), 0, "RSM: Interrupcion reinicia contador (1 nueva ALERTA)");
    rsm.update(2);  // 2
    expectInt(rsm.update(2), 2, "RSM: 3 consecutivas ALERTA tras interrupcion -> escala a 2");

    // Escalación parcial: escalar de 0 a 1 (intermedio)
    rsm.reset();
    rsm.update(1); rsm.update(1);
    expectInt(rsm.update(1), 1, "RSM: 3 muestras ESTRES -> escala de 0 a 1");

    // Des-escalar de 1 a 0 necesita 5
    rsm.update(0); rsm.update(0); rsm.update(0); rsm.update(0);
    expectInt(rsm.stableState(), 1, "RSM: 4 muestras NORMAL no des-escala de 1");
    rsm.update(0);
    expectInt(rsm.stableState(), 0, "RSM: 5 muestras NORMAL des-escala de 1 a 0");

    // ========================================================================
    // 10. Verificación de TelemetryRecord y Campo de Humedad (SHT30)
    // ========================================================================
    std::cout << "\n--- 10. Estructura de Telemetria (BME280) ---\n";
    iot::watermonitor::TelemetryRecord sampleRecord{};
    sampleRecord.uptimeSec = 1200;
    sampleRecord.levelCm = 35.5f;
    sampleRecord.levelPercent = 71.0f;
    sampleRecord.flowLMin = 3.8f;
    sampleRecord.dropRateCmH = 0.4f;
    sampleRecord.tempC = 19.8f;
    sampleRecord.pressureHpa = 746.5f;
    sampleRecord.humidityPct = 68.5f;
    sampleRecord.etoMmDay = 3.9f;
    sampleRecord.riskState = 0;

    expectNear(sampleRecord.humidityPct, 68.5f, 0.01f, "TelemetryRecord.humidityPct almacena valor BME280");
    expectTrue(sizeof(iot::watermonitor::TelemetryRecord) == 37, "TelemetryRecord empaquetado (#pragma pack) = 37 bytes");

    // ========================================================================
    // 11. Verificación de SramRingBuffer (Capacidad 360 muestras)
    // ========================================================================
    std::cout << "\n--- 11. SramRingBuffer (360 Muestras en SRAM) ---\n";
    iot::watermonitor::SramRingBuffer ringBuffer;
    expectTrue(ringBuffer.size() == 0, "Buffer inicialmente vacio (size == 0)");
    expectTrue(!ringBuffer.isFull(), "Buffer inicialmente no lleno");

    iot::watermonitor::TelemetryRecord tempRec{};
    expectTrue(!ringBuffer.get(0, tempRec), "get(0) en buffer vacio retorna false");
    expectTrue(!ringBuffer.getLatest(tempRec), "getLatest en buffer vacio retorna false");

    // Inserción de 1 elemento
    ringBuffer.push(sampleRecord);
    expectTrue(ringBuffer.size() == 1, "Buffer size == 1 tras primer push");
    expectTrue(ringBuffer.get(0, tempRec) && tempRec.uptimeSec == 1200, "get(0) recupera registro insertado");
    expectTrue(ringBuffer.getLatest(tempRec) && tempRec.humidityPct == 68.5f, "getLatest recupera ultimo registro");

    // Llenar buffer hasta 360 elementos
    for (uint32_t i = 1; i < 360; i++) {
        iot::watermonitor::TelemetryRecord r{};
        r.uptimeSec = 1200 + i * 2;
        r.levelCm = 35.0f - (i * 0.05f);
        r.humidityPct = 60.0f + (i % 30);
        ringBuffer.push(r);
    }
    expectTrue(ringBuffer.size() == 360, "Buffer lleno a capacidad maxima (size == 360)");
    expectTrue(ringBuffer.isFull(), "isFull() reporta true al alcanzar 360");

    // Verificar orden cronológico
    ringBuffer.get(0, tempRec);
    expectTrue(tempRec.uptimeSec == 1200, "Orden cronologico: index 0 es el elemento mas antiguo");
    ringBuffer.get(359, tempRec);
    expectTrue(tempRec.uptimeSec == (1200 + 359 * 2), "Orden cronologico: index 359 es el mas reciente");

    // Sobreescritura (Wrap-around): insertar 10 elementos adicionales
    for (uint32_t i = 0; i < 10; i++) {
        iot::watermonitor::TelemetryRecord r{};
        r.uptimeSec = 2000 + i * 2;
        r.humidityPct = 75.0f;
        ringBuffer.push(r);
    }
    expectTrue(ringBuffer.size() == 360, "Buffer mantiene size == 360 tras wrap-around");
    ringBuffer.get(0, tempRec);
    expectTrue(tempRec.uptimeSec == (1200 + 10 * 2), "Wrap-around correcto: el elemento mas antiguo fue desplazado");
    ringBuffer.get(359, tempRec);
    expectTrue(tempRec.uptimeSec == (2000 + 9 * 2), "Wrap-around correcto: el elemento 359 es el mas reciente");
    ringBuffer.getLatest(tempRec);
    expectTrue(tempRec.uptimeSec == (2000 + 9 * 2), "getLatest retorna registro mas reciente tras wrap-around");

    // Limpieza
    ringBuffer.clear();
    expectTrue(ringBuffer.size() == 0, "clear() vacia el buffer (size == 0)");
    expectTrue(!ringBuffer.isFull(), "isFull() es false tras clear()");

    // Doble wrap-around
    iot::watermonitor::SramRingBuffer edgeBuf;
    for (uint32_t i = 0; i < 720; i++) {
        iot::watermonitor::TelemetryRecord r{};
        r.uptimeSec = 5000 + i;
        edgeBuf.push(r);
    }
    expectTrue(edgeBuf.size() == 360, "Buffer mantiene exactamente 360 tras 720 pushes");
    expectTrue(!edgeBuf.get(360, tempRec), "get(360) fuera de rango retorna false");
    expectTrue(!edgeBuf.get(999, tempRec), "get(999) fuera de rango retorna false");
    edgeBuf.get(0, tempRec);
    expectTrue(tempRec.uptimeSec == (5000 + 360), "Doble wrap-around: index 0 tiene uptime 5360");
    edgeBuf.getLatest(tempRec);
    expectTrue(tempRec.uptimeSec == (5000 + 719), "Doble wrap-around: getLatest tiene uptime 5719");

    // ========================================================================
    // 12. Verificación del Filtro de Seguridad por Subred
    // ========================================================================
    std::cout << "\n--- 12. Filtro de Seguridad por Subred ---\n";
    uint32_t mask24 = ipToUint(255, 255, 255, 0);
    uint32_t localApIp = ipToUint(192, 168, 4, 1);
    uint32_t authorizedClient = ipToUint(192, 168, 4, 25);
    uint32_t foreignClient = ipToUint(10, 0, 0, 15);
    uint32_t anotherSubnet = ipToUint(192, 168, 5, 25);

    expectTrue(iot::watermonitor::isSubnetAllowed(authorizedClient, localApIp, mask24),
               "Filtro Subred: Cliente en 192.168.4.25 AUTORIZADO en 192.168.4.1/24");
    expectTrue(!iot::watermonitor::isSubnetAllowed(foreignClient, localApIp, mask24),
               "Filtro Subred: Cliente foraneo 10.0.0.15 DENEGADO (403)");
    expectTrue(!iot::watermonitor::isSubnetAllowed(anotherSubnet, localApIp, mask24),
               "Filtro Subred: Cliente en 192.168.5.25 DENEGADO (403)");

    // Máscaras de frontera
    uint32_t mask32 = ipToUint(255, 255, 255, 255);
    uint32_t mask16 = ipToUint(255, 255, 0, 0);
    uint32_t mask0 = 0;
    expectTrue(iot::watermonitor::isSubnetAllowed(localApIp, localApIp, mask32),
               "Filtro Subred: Mascara /32 autoriza solo IP identica");
    expectTrue(!iot::watermonitor::isSubnetAllowed(authorizedClient, localApIp, mask32),
               "Filtro Subred: Mascara /32 deniega host distinto");
    expectTrue(iot::watermonitor::isSubnetAllowed(ipToUint(192, 168, 10, 50), localApIp, mask16),
               "Filtro Subred: Mascara /16 autoriza 192.168.10.50");
    expectTrue(iot::watermonitor::isSubnetAllowed(foreignClient, localApIp, mask0),
               "Filtro Subred: Mascara /0 wildcard autoriza cualquier IP");

    // ========================================================================
    // RESUMEN FINAL
    // ========================================================================
    std::cout << "\n========================================================\n";
    std::cout << " TODAS LAS VERIFICACIONES DEL CHALLENGE 2 PASARON OK    \n";
    std::cout << " Correcciones Ch1: fail-safe, dH/dt en nivel alto, fugas\n";
    std::cout << " Nuevas funciones: RiskStateMachine con histeresis       \n";
    std::cout << "========================================================\n";
    return 0;
}
