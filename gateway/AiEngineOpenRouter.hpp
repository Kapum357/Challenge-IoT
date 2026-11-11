#ifndef AI_ENGINE_OPENROUTER_HPP
#define AI_ENGINE_OPENROUTER_HPP

#include <string>
#include <vector>
#include <cstdint>
#include <sqlite3.h>

namespace iot {
namespace gateway {

// ============================================================================
// Estructuras de Datos de Telemetría y Analítica Cuantitativa en Gateway
// ============================================================================

struct LecturaActual {
    std::string timestamp_utc;
    float level_cm{0.0f};
    float level_pct{0.0f};
    float drop_rate_cm_h{0.0f};
    float temp_c{0.0f};
    float humidity_pct{0.0f};
    float pressure_hpa{0.0f};
    float eto_mm_day{0.0f};
    int edge_risk_state{0};
    bool alarm_silenced_ack{false};
};

struct AnaliticaSqliteLocal {
    int muestras_24h{0};
    float temp_min_24h_c{0.0f};
    float temp_max_24h_c{0.0f};
    float vpd_kpa{0.0f};
    float z_score_drop_rate{0.0f};
    float ols_slope_cm_h{0.0f};
    float ols_hours_to_critical_25pct{999.0f};
};

struct GatewayPayload {
    std::string nodo_id{"nodo01"};
    float tank_height_cm{50.0f};
    LecturaActual lectura_actual;
    AnaliticaSqliteLocal analitica_sqlite_local;
};

// ============================================================================
// Estructuras de Respuesta y Contrato JSON de la Recomendación de IA
// ============================================================================

struct DiagnosticoIA {
    int nivel_riesgo_sugerido{0};
    std::string etiqueta_alerta{"NORMAL"};
    std::string tipo_evento{"OPERACION_NORMAL"};
    bool anomalia_estadistica_detectada{false};
};

struct PrediccionTendencia {
    float horas_hasta_nivel_critico_25pct{999.0f};
    float nivel_proyectado_6h_cm{0.0f};
    float nivel_proyectado_6h_pct{0.0f};
    std::string demanda_evaporativa_estado{"MODERADA"};
};

struct RecomendacionAutoridades {
    std::string decision_sugerida_cmgrd_uaegrd;
    std::string mensaje_comunidad_acueducto;
    std::string prioridad_atencion{"RUTINARIA"};
};

struct AuditoriaCoVe {
    bool datos_integros{true};
    bool sin_alucinacion_de_caudal{true};
    std::string metodo_aplicado{"OPENROUTER_LLM_MAS_OLS_ZSCORE"};
};

struct AiRecommendationResult {
    std::string timestamp_analisis;
    std::string nodo_id;
    DiagnosticoIA diagnostico_ia;
    PrediccionTendencia prediccion_tendencia;
    RecomendacionAutoridades recomendacion_autoridades;
    std::string justificacion_tecnica;
    AuditoriaCoVe auditoria_cove;
    std::string raw_json;
};

// ============================================================================
// Funciones del Motor de IA
// ============================================================================

/**
 * @brief Extrae de SQLite la última lectura del nodo y calcula OLS (1 h) y Z-score (24 h).
 * @param db Puntero a la base de datos SQLite abierta.
 * @param nodoId Identificador del nodo en campo (ej. "nodo01").
 * @return Estructura GatewayPayload lista para inferencia.
 */
GatewayPayload buildSqliteGatewayPayload(sqlite3* db, const std::string& nodoId = "nodo01");

/**
 * @brief Genera un veredicto determinista local en la Raspberry Pi (< 1 ms de latencia)
 *        cuando se cae la conectividad WAN o falla la API de OpenRouter.
 * @param payload Datos extraídos y analizados de SQLite.
 * @param fallbackReason Motivo de activación del fallback.
 * @return Estructura AiRecommendationResult conforme al contrato JSON estricto.
 */
AiRecommendationResult localDeterministicFallback(const GatewayPayload& payload,
                                                   const std::string& fallbackReason);

/**
 * @brief Invoca a OpenRouter Free con rotación de modelos gratuitos y fallback determinista.
 * @param payload Datos extraídos y analizados de SQLite.
 * @param apiKey Llave de API de OpenRouter (si está vacía, activa fallback automáticamente).
 * @return Estructura AiRecommendationResult con la respuesta validada.
 */
AiRecommendationResult generateAiRecommendation(const GatewayPayload& payload,
                                                 const std::string& apiKey = "");

/**
 * @brief Serializa GatewayPayload a JSON string.
 */
std::string gatewayPayloadToJson(const GatewayPayload& payload);

/**
 * @brief Serializa AiRecommendationResult a JSON string estricto.
 */
std::string recommendationResultToJson(const AiRecommendationResult& result);

/**
 * @brief Retorna el System Prompt de 6 bloques parametrizado para Sabana Centro.
 */
const char* getSystemPrompt();

} // namespace gateway
} // namespace iot

#endif // AI_ENGINE_OPENROUTER_HPP
