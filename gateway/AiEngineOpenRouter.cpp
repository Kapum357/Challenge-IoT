#include "AiEngineOpenRouter.hpp"
#include "../src/WaterMonitorCore.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#if __has_include(<curl/curl.h>)
#define HAVE_LIBCURL 1
#include <curl/curl.h>
#else
#define HAVE_LIBCURL 0
#endif

#if __has_include(<nlohmann/json.hpp>)
#define HAVE_NLOHMANN_JSON 1
#include <nlohmann/json.hpp>
using json = nlohmann::json;
#else
#define HAVE_NLOHMANN_JSON 0
#endif

namespace iot
{
    namespace gateway
    {

        namespace
        {

            const char *OPENROUTER_API_URL = "https://openrouter.ai/api/v1/chat/completions";

            const std::vector<std::string> OPENROUTER_FREE_MODELS = {
                "google/gemini-2.0-flash-exp:free",
                "meta-llama/llama-3.3-70b-instruct:free",
                "qwen/qwen-2.5-72b-instruct:free",
                "openrouter/free"};

            const char *SYSTEM_PROMPT_TEXT = R"RAW([ROL]
Actúas como un Ingeniero Hidrometeorológico Senior y Analista de Sistemas IoT de Misión Crítica adscrito al Comité Municipal de Gestión del Riesgo de Desastres (CMGRD) y a la Unidad Administrativa Especial para la Gestión del Riesgo de Desastres (UAEGRD) de Cundinamarca, especializado en reservorios y tanques veredales de Sabana Centro (altitud: 2,600 m s. n. m.; presión barométrica nominal: ~748-750 hPa; latitud: 4.81° N) durante el Fenómeno de El Niño 2026-2027. Tu postura es técnica, determinista, conservadora frente al riesgo y orientada a respaldar decisiones de racionamiento preventivo y planes de contingencia.

[OBJETIVO / TAREA]
Analizar el resumen estructurado de telemetría hidrometeorológica, las estadísticas de las últimas 24 horas y los indicadores pre-calculados en SQLite (regresión lineal de nivel y Z-score de tasa de descenso) enviados por el Gateway IoT (Raspberry Pi) para ejecutar tres funciones de apoyo a la decisión:
1. Diagnosticar la causa física del comportamiento del reservorio diferenciando entre: (a) operación normal, (b) pérdida acelerada por estrés evaporativo/calor extremo (alto ETo y alto VPD), (c) riesgo de helada radiativa en la Sabana, (d) anomalía hidráulica por descenso abrupto no explicado por evaporación (posible fuga estructural o captación no autorizada), y (e) fallo instrumental de sensores.
2. Validar la predicción de horas remanentes hasta el nivel crítico (25% de capacidad = 12.5 cm en escala de 50 cm) y proyectar el porcentaje de nivel a 6 horas.
3. Emitir una recomendación operativa clara y concisa en lenguaje natural para las autoridades locales (CMGRD / Acueducto Veredal) y para la comunidad (JAC), sin activar ni bloquear directamente las alarmas físicas (las cuales son gobernadas por la lógica determinista del ESP32).
Alcance estricto: Responde ÚNICAMENTE con un objeto JSON válido que será almacenado en SQLite y publicado por MQTT hacia Mosquitto (agua/<nodo_id>/ia/recomendacion) y Ubidots.

[CONTEXTO / DATOS]
Recibirás en el mensaje del usuario un bloque delimitado por <SQLITE_GATEWAY_PAYLOAD> y </SQLITE_GATEWAY_PAYLOAD> con los siguientes datos:
- Parámetros físicos del punto crítico: nodo_id, tank_height_cm = 50.0 cm (umbral crítico = 25.0% = 12.5 cm).
- Variables en tiempo real (lectura_actual, sin sensor de caudal):
  * timestamp_utc, level_cm, level_pct, drop_rate_cm_h, temp_c, humidity_pct, pressure_hpa, eto_mm_day, edge_risk_state, alarm_silenced_ack.
- Analítica pre-calculada en SQLite por la Raspberry Pi (analitica_sqlite_local):
  * muestras_24h, temp_min_24h_c, temp_max_24h_c, vpd_kpa, z_score_drop_rate, ols_slope_cm_h, ols_hours_to_critical_25pct.

[RESTRICCIONES Y PROTOCOLO ANTI-ALUCINACION]
1. Cero invención de sensores o variables: El sistema NO posee sensor de caudal/flujo. Prohibido mencionar caudalímetros o flujo en tubería.
2. Protocolo ante datos faltantes o fallo de sensor (-1.0 o null):
   - Si level_cm == -1.0 o pressure_hpa == -1.0, declara explícitamente "DATO_NO_DISPONIBLE_EN_SQLITE" en justificacion_tecnica, clasifica tipo_evento = "FALLO_INSTRUMENTAL_SENSOR" y recomienda inspección técnica inmediata sin emitir falsas alertas de sequía.
3. Privacidad y Habeas Data: Cero IPs privadas, MACs, tokens ni nombres personales en la salida JSON.
4. Separación de hechos e inferencias: En justificacion_tecnica, inicia siempre con "Hechos en SQLite: [...]" seguido de "Inferencia hidroclimática: [...]".
5. Contrato de formato puro: Prohibido emitir texto fuera del bloque JSON o usar delimitadores Markdown.

[PROCESO / ESTRATEGIA DE RAZONAMIENTO]
Paso 1 (Integridad Sensorial): Comprueba si level_cm == -1.0 o pressure_hpa == -1.0. Si hay centinela, clasifica inmediatamente FALLO_INSTRUMENTAL_SENSOR.
Paso 2 (Evaluación Hidroclimática Cruzada sin Sensor de Caudal):
  - Regla 2.1: eto_mm_day >= 4.5 o vpd_kpa >= 1.6 indican alta demanda evaporativa El Niño.
  - Regla 2.2: Evaporación física pura = 0.02 a 0.06 cm/h. Si drop_rate_cm_h >= 5.0 y z_score_drop_rate >= 2.0:
    * Si eto_mm_day >= 4.5 en horas diurnas: SOBREDEMANDA_Y_ESTRES_TERMICO_EL_NINO.
    * Si horario nocturno o drop_rate_cm_h >= 8.0: DESCENSO_ANOMALO_POSIBLE_FUGA_O_SOBRECONSUMO.
  - Regla 2.3: Si temp_c <= 5.5: ALERTA_HELADA_RADIATIVA.
  - Regla 2.4: ols_hours_to_critical_25pct como estimador primario hacia 12.5 cm.
Paso 3 (Chain-of-Verification - CoVe):
  - V1: Métricas citadas coinciden exactamente con SQLite.
  - V2: Prohibición absoluta de caudalímetro/sensor de flujo respetada.
  - V3: Recomendación orientada a decisiones CMGRD/UAEGRD.
  )RAW";

std::string getIso8601Now() {
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm gm{};
#if defined(_WIN32) || defined(_WIN64)
    gmtime_s(&gm, &tt);
#else
    gmtime_r(&tt, &gm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &gm);
    return std::string(buf);
}

std::string jsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 16);
    for (unsigned char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

#if HAVE_LIBCURL
struct CurlStringResponse {
    std::string data;
};

size_t curlWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t totalSize = size * nmemb;
    auto* mem = static_cast<CurlStringResponse*>(userp);
    mem->data.append(static_cast<char*>(contents), totalSize);
    return totalSize;
}
#endif

} // namespace

const char* getSystemPrompt() {
    return SYSTEM_PROMPT_TEXT;
}

std::string gatewayPayloadToJson(const GatewayPayload& p) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2);
    ss << "{\n";
    ss << "  \"nodo_id\": \"" << p.nodo_id << "\",\n";
    ss << "  \"tank_height_cm\": " << p.tank_height_cm << ",\n";
    ss << "  \"lectura_actual\": {\n";
    ss << "    \"timestamp_utc\": \"" << p.lectura_actual.timestamp_utc << "\",\n";
    ss << "    \"level_cm\": " << p.lectura_actual.level_cm << ",\n";
    ss << "    \"level_pct\": " << p.lectura_actual.level_pct << ",\n";
    ss << "    \"drop_rate_cm_h\": " << p.lectura_actual.drop_rate_cm_h << ",\n";
    ss << "    \"temp_c\": " << p.lectura_actual.temp_c << ",\n";
    ss << "    \"humidity_pct\": " << p.lectura_actual.humidity_pct << ",\n";
    ss << "    \"pressure_hpa\": " << p.lectura_actual.pressure_hpa << ",\n";
    ss << "    \"eto_mm_day\": " << p.lectura_actual.eto_mm_day << ",\n";
    ss << "    \"edge_risk_state\": " << p.lectura_actual.edge_risk_state << ",\n";
    ss << "    \"alarm_silenced_ack\": " << (p.lectura_actual.alarm_silenced_ack ? "true" : "false") << "\n";
    ss << "  },\n";
    ss << "  \"analitica_sqlite_local\": {\n";
    ss << "    \"muestras_24h\": " << p.analitica_sqlite_local.muestras_24h << ",\n";
    ss << "    \"temp_min_24h_c\": " << p.analitica_sqlite_local.temp_min_24h_c << ",\n";
    ss << "    \"temp_max_24h_c\": " << p.analitica_sqlite_local.temp_max_24h_c << ",\n";
    ss << "    \"vpd_kpa\": " << p.analitica_sqlite_local.vpd_kpa << ",\n";
    ss << "    \"z_score_drop_rate\": " << p.analitica_sqlite_local.z_score_drop_rate << ",\n";
    ss << "    \"ols_slope_cm_h\": " << p.analitica_sqlite_local.ols_slope_cm_h << ",\n";
    ss << "    \"ols_hours_to_critical_25pct\": " << p.analitica_sqlite_local.ols_hours_to_critical_25pct << "\n";
    ss << "  }\n";
    ss << "}";
    return ss.str();
}

std::string recommendationResultToJson(const AiRecommendationResult& r) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2);
    ss << "{\n";
    ss << "  \"timestamp_analisis\": \"" << r.timestamp_analisis << "\",\n";
    ss << "  \"nodo_id\": \"" << r.nodo_id << "\",\n";
    ss << "  \"diagnostico_ia\": {\n";
    ss << "    \"nivel_riesgo_sugerido\": " << r.diagnostico_ia.nivel_riesgo_sugerido << ",\n";
    ss << "    \"etiqueta_alerta\": \"" << r.diagnostico_ia.etiqueta_alerta << "\",\n";
    ss << "    \"tipo_evento\": \"" << r.diagnostico_ia.tipo_evento << "\",\n";
    ss << "    \"anomalia_estadistica_detectada\": " << (r.diagnostico_ia.anomalia_estadistica_detectada ? "true" : "false") << "\n";
    ss << "  },\n";
    ss << "  \"prediccion_tendencia\": {\n";
    ss << "    \"horas_hasta_nivel_critico_25pct\": " << r.prediccion_tendencia.horas_hasta_nivel_critico_25pct << ",\n";
    ss << "    \"nivel_proyectado_6h_cm\": " << r.prediccion_tendencia.nivel_proyectado_6h_cm << ",\n";
    ss << "    \"nivel_proyectado_6h_pct\": " << r.prediccion_tendencia.nivel_proyectado_6h_pct << ",\n";
    ss << "    \"demanda_evaporativa_estado\": \"" << r.prediccion_tendencia.demanda_evaporativa_estado << "\"\n";
    ss << "  },\n";
    ss << "  \"recomendacion_autoridades\": {\n";
    ss << "    \"decision_sugerida_cmgrd_uaegrd\": \"" << jsonEscape(r.recomendacion_autoridades.decision_sugerida_cmgrd_uaegrd) << "\",\n";
    ss << "    \"mensaje_comunidad_acueducto\": \"" << jsonEscape(r.recomendacion_autoridades.mensaje_comunidad_acueducto) << "\",\n";
    ss << "    \"prioridad_atencion\": \"" << r.recomendacion_autoridades.prioridad_atencion << "\"\n";
    ss << "  },\n";
    ss << "  \"justificacion_tecnica\": \"" << jsonEscape(r.justificacion_tecnica) << "\",\n";
    ss << "  \"auditoria_cove\": {\n";
    ss << "    \"datos_integros\": " << (r.auditoria_cove.datos_integros ? "true" : "false") << ",\n";
    ss << "    \"sin_alucinacion_de_caudal\": " << (r.auditoria_cove.sin_alucinacion_de_caudal ? "true" : "false") << ",\n";
    ss << "    \"metodo_aplicado\": \"" << r.auditoria_cove.metodo_aplicado << "\"\n";
    ss << "  }\n";
    ss << "}";
    return ss.str();
}

GatewayPayload buildSqliteGatewayPayload(sqlite3* db, const std::string& nodoId) {
    if (!db) {
        throw std::runtime_error("Puntero a base de datos SQLite nulo");
    }

    GatewayPayload payload;
    payload.nodo_id = nodoId;
    payload.tank_height_cm = watermonitor::TANK_HEIGHT_CM;

    // 1. Extraer última lectura
    const char* sqlLatest =
        "SELECT timestamp_utc, level_cm, level_pct, drop_rate_cm_h, "
        "       temp_c, humidity_pct, pressure_hpa, eto_mm_day, "
        "       edge_risk_state, alarm_silenced_ack "
        "FROM telemetry WHERE nodo_id = ? ORDER BY id DESC LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sqlLatest, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("Error preparando consulta de última lectura");
    }
    sqlite3_bind_text(stmt, 1, nodoId.c_str(), -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        throw std::runtime_error("Sin datos en SQLite para el nodo: " + nodoId);
    }

    payload.lectura_actual.timestamp_utc = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    payload.lectura_actual.level_cm = static_cast<float>(sqlite3_column_double(stmt, 1));
    payload.lectura_actual.level_pct = static_cast<float>(sqlite3_column_double(stmt, 2));
    payload.lectura_actual.drop_rate_cm_h = static_cast<float>(sqlite3_column_double(stmt, 3));
    payload.lectura_actual.temp_c = static_cast<float>(sqlite3_column_double(stmt, 4));
    payload.lectura_actual.humidity_pct = static_cast<float>(sqlite3_column_double(stmt, 5));
    payload.lectura_actual.pressure_hpa = static_cast<float>(sqlite3_column_double(stmt, 6));
    payload.lectura_actual.eto_mm_day = static_cast<float>(sqlite3_column_double(stmt, 7));
    payload.lectura_actual.edge_risk_state = sqlite3_column_int(stmt, 8);
    payload.lectura_actual.alarm_silenced_ack = (sqlite3_column_int(stmt, 9) != 0);
    sqlite3_finalize(stmt);

    // 2. Extraer estadísticas de las últimas 24 horas
    const char* sqlStats =
        "SELECT COUNT(*), "
        "       MIN(CASE WHEN temp_c > -40 THEN temp_c END), "
        "       MAX(CASE WHEN temp_c > -40 THEN temp_c END), "
        "       AVG(drop_rate_cm_h), "
        "       AVG(drop_rate_cm_h * drop_rate_cm_h) "
        "FROM telemetry "
        "WHERE nodo_id = ? AND timestamp_utc >= strftime('%Y-%m-%dT%H:%M:%SZ', 'now', '-24 hours') AND level_cm >= 0;";
    if (sqlite3_prepare_v2(db, sqlStats, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, nodoId.c_str(), -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            payload.analitica_sqlite_local.muestras_24h = sqlite3_column_int(stmt, 0);
            payload.analitica_sqlite_local.temp_min_24h_c = (sqlite3_column_type(stmt, 1) != SQLITE_NULL)
                ? static_cast<float>(sqlite3_column_double(stmt, 1)) : payload.lectura_actual.temp_c;
            payload.analitica_sqlite_local.temp_max_24h_c = (sqlite3_column_type(stmt, 2) != SQLITE_NULL)
                ? static_cast<float>(sqlite3_column_double(stmt, 2)) : payload.lectura_actual.temp_c;

            float meanDrop = (sqlite3_column_type(stmt, 3) != SQLITE_NULL)
                ? static_cast<float>(sqlite3_column_double(stmt, 3)) : 0.0f;
            float meanSq = (sqlite3_column_type(stmt, 4) != SQLITE_NULL)
                ? static_cast<float>(sqlite3_column_double(stmt, 4)) : 0.0f;
            float variance = std::max(0.0f, meanSq - meanDrop * meanDrop);
            float stdDrop = (variance > 1e-6f) ? std::sqrt(variance) : 0.5f;

            payload.analitica_sqlite_local.z_score_drop_rate =
                watermonitor::computeZScore(payload.lectura_actual.drop_rate_cm_h, meanDrop, stdDrop);
        }
        sqlite3_finalize(stmt);
    }

    // 3. Regresión Lineal OLS sobre la última hora
    const char* sqlOls =
        "SELECT strftime('%s', timestamp_utc), level_cm "
        "FROM telemetry "
        "WHERE nodo_id = ? AND timestamp_utc >= strftime('%Y-%m-%dT%H:%M:%SZ', 'now', '-1 hour') AND level_cm >= 0 "
        "ORDER BY id ASC;";
    std::vector<float> timeHours;
    std::vector<float> levelsCm;
    if (sqlite3_prepare_v2(db, sqlOls, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, nodoId.c_str(), -1, SQLITE_STATIC);
        double t0 = -1.0;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            double epochSec = sqlite3_column_double(stmt, 0);
            float lvl = static_cast<float>(sqlite3_column_double(stmt, 1));
            if (t0 < 0.0) t0 = epochSec;
            timeHours.push_back(static_cast<float>((epochSec - t0) / 3600.0));
            levelsCm.push_back(lvl);
        }
        sqlite3_finalize(stmt);
    }

    if (timeHours.size() >= 3) {
        payload.analitica_sqlite_local.ols_slope_cm_h =
            watermonitor::computeOlsSlope(timeHours.data(), levelsCm.data(), timeHours.size());
    } else {
        payload.analitica_sqlite_local.ols_slope_cm_h = -payload.lectura_actual.drop_rate_cm_h;
    }

    // VPD y Proyección a umbral crítico
    payload.analitica_sqlite_local.vpd_kpa =
        watermonitor::computeVpdKpa(payload.lectura_actual.temp_c, payload.lectura_actual.humidity_pct);

    float descentRateEffective = std::max(payload.lectura_actual.drop_rate_cm_h,
                                         -payload.analitica_sqlite_local.ols_slope_cm_h);
    payload.analitica_sqlite_local.ols_hours_to_critical_25pct =
        watermonitor::computeHoursToCritical(payload.lectura_actual.level_cm,
                                             descentRateEffective,
                                             watermonitor::CRITICAL_LEVEL_CM);

    return payload;
}

AiRecommendationResult localDeterministicFallback(const GatewayPayload& payload,
                                                   const std::string& fallbackReason) {
    const auto& curr = payload.lectura_actual;
    const auto& loc = payload.analitica_sqlite_local;
    std::string nowIso = getIso8601Now();

    AiRecommendationResult res;
    res.timestamp_analisis = nowIso;
    res.nodo_id = payload.nodo_id;

    // Caso de Falla de Sensor (Sentinel -1.0f)
    if (curr.level_cm < 0.0f || curr.pressure_hpa < 0.0f) {
        res.diagnostico_ia.nivel_riesgo_sugerido = 2;
        res.diagnostico_ia.etiqueta_alerta = "FALLO_SENSOR";
        res.diagnostico_ia.tipo_evento = "FALLO_INSTRUMENTAL_SENSOR";
        res.diagnostico_ia.anomalia_estadistica_detectada = true;

        res.prediccion_tendencia.horas_hasta_nivel_critico_25pct = -1.0f;
        res.prediccion_tendencia.nivel_proyectado_6h_cm = -1.0f;
        res.prediccion_tendencia.nivel_proyectado_6h_pct = -1.0f;
        res.prediccion_tendencia.demanda_evaporativa_estado = "DATO_NO_DISPONIBLE_EN_SQLITE";

        res.recomendacion_autoridades.decision_sugerida_cmgrd_uaegrd =
            "Despachar técnico al tanque para revisar sensor ultrasónico HC-SR04 o bus I2C del BME280.";
        res.recomendacion_autoridades.mensaje_comunidad_acueducto =
            "Nodo en revisión técnica preventiva; mantener uso prudente del agua.";
        res.recomendacion_autoridades.prioridad_atencion = "URGENTE_6H";

        std::ostringstream just;
        just << std::fixed << std::setprecision(1);
        just << "Hechos en SQLite: level_cm=" << curr.level_cm
             << ", pressure_hpa=" << curr.pressure_hpa
             << ". Inferencia hidroclimática: DATO_NO_DISPONIBLE_EN_SQLITE (" << fallbackReason << ").";
        res.justificacion_tecnica = just.str();

        res.auditoria_cove.datos_integros = false;
        res.auditoria_cove.sin_alucinacion_de_caudal = true;
        res.auditoria_cove.metodo_aplicado = "FALLBACK_LOCAL_DETERMINISTA (" + fallbackReason + ")";
        res.raw_json = recommendationResultToJson(res);
        return res;
    }

    // Evaluación Determinista sin Sensor de Flujo
    float dropRate = curr.drop_rate_cm_h;
    float zScore = loc.z_score_drop_rate;
    float eto = curr.eto_mm_day;
    float projCm = std::max(0.0f, std::min(payload.tank_height_cm, curr.level_cm - dropRate * 6.0f));
    float projPct = (projCm / payload.tank_height_cm) * 100.0f;
    bool anomalia = (zScore >= 2.0f || dropRate >= 5.0f);

    std::string evento;
    std::string accion;

    if (anomalia && eto < 4.5f) {
        evento = "DESCENSO_ANOMALO_POSIBLE_FUGA_O_SOBRECONSUMO";
        accion = "Inspeccionar tubería de salida y activar racionamiento preventivo sectorizado.";
    } else if (curr.edge_risk_state >= 2 || eto >= 4.5f) {
        evento = "SOBREDEMANDA_Y_ESTRES_TERMICO_EL_NINO";
        accion = "Activar protocolo UAEGRD de racionamiento escalonado por alta evaporación y descenso.";
    } else if (curr.temp_c <= 5.5f) {
        evento = "ALERTA_HELADA_RADIATIVA";
        accion = "Vigilar tuberías expuestas por riesgo de helada en madrugada de Sabana Centro.";
    } else {
        evento = "OPERACION_NORMAL";
        accion = "Mantener operación normal del acueducto y monitoreo continuo en tablero.";
    }

    const char* etiquetas[] = {"NORMAL", "ESTRES_MODERADO", "ALERTA_SEVERA", "EMERGENCIA_CRITICA"};
    const char* prioridades[] = {"RUTINARIA", "PREVENTIVA_24H", "URGENTE_6H", "INMEDIATA_CONTINGENCIA"};
    int idx = std::max(0, std::min(3, curr.edge_risk_state));

    res.diagnostico_ia.nivel_riesgo_sugerido = idx;
    res.diagnostico_ia.etiqueta_alerta = etiquetas[idx];
    res.diagnostico_ia.tipo_evento = evento;
    res.diagnostico_ia.anomalia_estadistica_detectada = anomalia;

    res.prediccion_tendencia.horas_hasta_nivel_critico_25pct = loc.ols_hours_to_critical_25pct;
    res.prediccion_tendencia.nivel_proyectado_6h_cm = std::round(projCm * 100.0f) / 100.0f;
    res.prediccion_tendencia.nivel_proyectado_6h_pct = std::round(projPct * 10.0f) / 10.0f;
    res.prediccion_tendencia.demanda_evaporativa_estado = (eto >= 4.5f) ? "ALTA_EL_NINO" : "MODERADA";

    res.recomendacion_autoridades.decision_sugerida_cmgrd_uaegrd = accion;
    res.recomendacion_autoridades.mensaje_comunidad_acueducto =
        (idx >= 2) ? "Reducir consumo no esencial ante alerta hídrica." : "Consumo normal responsable.";
    res.recomendacion_autoridades.prioridad_atencion = prioridades[idx];

    std::ostringstream just;
    just << std::fixed << std::setprecision(1);
    just << "Hechos en SQLite: nivel=" << curr.level_pct << "% (" << curr.level_cm << " cm), "
         << "dH/dt=" << dropRate << " cm/h, ETo=" << eto << " mm/d, Z=" << zScore
         << ". Inferencia hidroclimática: " << evento << " (" << fallbackReason << ").";
    res.justificacion_tecnica = just.str();

    res.auditoria_cove.datos_integros = true;
    res.auditoria_cove.sin_alucinacion_de_caudal = true;
    res.auditoria_cove.metodo_aplicado = "FALLBACK_LOCAL_DETERMINISTA (" + fallbackReason + ")";
    res.raw_json = recommendationResultToJson(res);

    return res;
}

AiRecommendationResult generateAiRecommendation(const GatewayPayload& payload,
                                                 const std::string& apiKey) {
    if (apiKey.empty()) {
        return localDeterministicFallback(payload, "OPENROUTER_API_KEY_NO_DEFINIDA");
    }

#if !HAVE_LIBCURL
    return localDeterministicFallback(payload, "LIBCURL_NO_COMPILADO");
#else
    std::string payloadJson = gatewayPayloadToJson(payload);
    std::string userPrompt = "<SQLITE_GATEWAY_PAYLOAD>\n" + payloadJson + "\n</SQLITE_GATEWAY_PAYLOAD>";

    CURL* curl = curl_easy_init();
    if (!curl) {
        return localDeterministicFallback(payload, "ERROR_INICIALIZACION_CURL");
    }

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    std::string authHeader = "Authorization: Bearer " + apiKey;
    headers = curl_slist_append(headers, authHeader.c_str());
    headers = curl_slist_append(headers, "HTTP-Referer: https://github.com/Kapum357/Challenge");
    headers = curl_slist_append(headers, "X-Title: Equipo4-SabanaCentro-IoT-Ch3");

    for (const auto& modelId : OPENROUTER_FREE_MODELS) {
        std::ostringstream body;
        body << "{\n";
        body << "  \"model\": \"" << modelId << "\",\n";
        body << "  \"temperature\": 0.1,\n";
        body << "  \"response_format\": {\"type\": \"json_object\"},\n";
        body << "  \"messages\": [\n";
        body << "    {\"role\": \"system\", \"content\": \"" << jsonEscape(SYSTEM_PROMPT_TEXT) << "\"},\n";
        body << "    {\"role\": \"user\", \"content\": \"" << jsonEscape(userPrompt) << "\"}\n";
        body << "  ]\n";
        body << "}";

        std::string requestBody = body.str();
        CurlStringResponse resp;

        curl_easy_setopt(curl, CURLOPT_URL, OPENROUTER_API_URL);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestBody.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 12L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OK) {
            long httpCode = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
            if (httpCode >= 200 && httpCode < 300 && !resp.data.empty()) {
#if HAVE_NLOHMANN_JSON
                try {
                    auto parsed = json::parse(resp.data);
                    std::string content = parsed["choices"][0]["message"]["content"];
                    // Limpieza de delimitadores markdown si existiesen
                    if (content.rfind("```", 0) == 0) {
                        size_t firstNl = content.find('\n');
                        size_t lastBackticks = content.rfind("```");
                        if (firstNl != std::string::npos && lastBackticks != std::string::npos && lastBackticks > firstNl) {
                            content = content.substr(firstNl + 1, lastBackticks - firstNl - 1);
                        }
                    }
                    auto inner = json::parse(content);
                    AiRecommendationResult result;
                    result.timestamp_analisis = inner.value("timestamp_analisis", getIso8601Now());
                    result.nodo_id = inner.value("nodo_id", payload.nodo_id);
                    result.diagnostico_ia.nivel_riesgo_sugerido = inner["diagnostico_ia"].value("nivel_riesgo_sugerido", 0);
                    result.diagnostico_ia.etiqueta_alerta = inner["diagnostico_ia"].value("etiqueta_alerta", "NORMAL");
                    result.diagnostico_ia.tipo_evento = inner["diagnostico_ia"].value("tipo_evento", "OPERACION_NORMAL");
                    result.diagnostico_ia.anomalia_estadistica_detectada = inner["diagnostico_ia"].value("anomalia_estadistica_detectada", false);

                    result.prediccion_tendencia.horas_hasta_nivel_critico_25pct = inner["prediccion_tendencia"].value("horas_hasta_nivel_critico_25pct", 999.0f);
                    result.prediccion_tendencia.nivel_proyectado_6h_cm = inner["prediccion_tendencia"].value("nivel_proyectado_6h_cm", 0.0f);
                    result.prediccion_tendencia.nivel_proyectado_6h_pct = inner["prediccion_tendencia"].value("nivel_proyectado_6h_pct", 0.0f);
                    result.prediccion_tendencia.demanda_evaporativa_estado = inner["prediccion_tendencia"].value("demanda_evaporativa_estado", "MODERADA");

                    result.recomendacion_autoridades.decision_sugerida_cmgrd_uaegrd = inner["recomendacion_autoridades"].value("decision_sugerida_cmgrd_uaegrd", "");
                    result.recomendacion_autoridades.mensaje_comunidad_acueducto = inner["recomendacion_autoridades"].value("mensaje_comunidad_acueducto", "");
                    result.recomendacion_autoridades.prioridad_atencion = inner["recomendacion_autoridades"].value("prioridad_atencion", "RUTINARIA");

                    result.justificacion_tecnica = inner.value("justificacion_tecnica", "");
                    result.auditoria_cove.datos_integros = inner["auditoria_cove"].value("datos_integros", true);
                    result.auditoria_cove.sin_alucinacion_de_caudal = inner["auditoria_cove"].value("sin_alucinacion_de_caudal", true);
                    result.auditoria_cove.metodo_aplicado = "OPENROUTER_LLM (" + modelId + ")_MAS_OLS_ZSCORE";
                    result.raw_json = recommendationResultToJson(result);

                    curl_slist_free_all(headers);
                    curl_easy_cleanup(curl);
                    return result;
                } catch (const std::exception& e) {
                    std::cerr << "[WARN] Parseo de JSON fallido para modelo " << modelId << ": " << e.what() << "\n";
                }
#endif
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return localDeterministicFallback(payload, "SIN_INTERNET_O_RATE_LIMIT_OPENROUTER");
#endif
}

} // namespace gateway
} // namespace iot
