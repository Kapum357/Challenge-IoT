#include "AiEngineOpenRouter.hpp"
#include "../src/WaterMonitorCore.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <mosquitto.h>
#include <sqlite3.h>

namespace {

using iot::gateway::LecturaActual;

constexpr int UBIDOTS_MIN_PERIOD_SEC = 30;  // Limite de tasa hacia Ubidots (o antes, por evento)
constexpr int FORWARD_BATCH = 50;

std::atomic<bool> g_running{true};
std::atomic<bool> g_ubiConnected{false};
bool g_ubiEnabled = false;

std::mutex g_dbMutex;  // Serializa todo acceso a SQLite entre hilos
sqlite3* g_db = nullptr;
std::map<std::string, std::pair<std::chrono::steady_clock::time_point, int>> g_rate;  // protegido por g_dbMutex

struct mosquitto* g_local = nullptr;
struct mosquitto* g_ubidots = nullptr;
std::vector<std::string> g_nodes;

std::mutex g_wakeMutex;
std::condition_variable g_wakeCv;
std::set<std::string> g_aiPending;
int g_aiPeriodSec = 60;

std::mutex g_ackMutex;
std::condition_variable g_ackCv;
std::set<int> g_ackedMids;

void signalHandler(int) {
    g_running = false;
}

std::string envOr(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : std::string(fallback);
}

std::string nowIso() {
    std::time_t tt = std::time(nullptr);
    std::tm gm{};
#if defined(_WIN32)
    gmtime_s(&gm, &tt);
#else
    gmtime_r(&tt, &gm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &gm);
    return buf;
}

// nodo_id llega por topico MQTT: solo [A-Za-z0-9_-] para impedir inyeccion hacia SQL, topicos o el LLM.
bool validNodeId(const std::string& s) {
    if (s.empty() || s.size() > 32) return false;
    for (unsigned char c : s) {
        if (!(std::isalnum(c) || c == '_' || c == '-')) return false;
    }
    return true;
}

bool parseTopic(const std::string& topic, std::string& nodo, std::string& kind) {
    if (topic.rfind("agua/", 0) != 0) return false;
    const size_t slash = topic.find('/', 5);
    if (slash == std::string::npos) return false;
    nodo = topic.substr(5, slash - 5);
    kind = topic.substr(slash + 1);
    return validNodeId(nodo);
}

bool hasKey(const std::string& s, const std::string& key) {
    return s.find("\"" + key + "\"") != std::string::npos;
}

float jsonFloat(const std::string& s, const std::string& key, float fallback) {
    size_t pos = s.find("\"" + key + "\"");
    if (pos == std::string::npos) return fallback;
    pos = s.find(':', pos);
    if (pos == std::string::npos) return fallback;
    ++pos;
    while (pos < s.size() && s[pos] == ' ') ++pos;
    char* end = nullptr;
    const float v = std::strtof(s.c_str() + pos, &end);
    return (end != s.c_str() + pos) ? v : fallback;
}

bool jsonBool(const std::string& s, const std::string& key) {
    size_t pos = s.find("\"" + key + "\"");
    if (pos == std::string::npos) return false;
    pos = s.find(':', pos);
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < s.size() && s[pos] == ' ') ++pos;
    return s.compare(pos, 4, "true") == 0;
}

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

bool execSql(sqlite3* db, const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[SQLITE] " << (err ? err : "error") << "\n";
        sqlite3_free(err);
        return false;
    }
    return true;
}

bool initDatabase(const std::string& dbPath, const std::string& schemaPath) {
    if (sqlite3_open_v2(dbPath.c_str(), &g_db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        std::cerr << "[ERROR] No se pudo abrir SQLite: " << sqlite3_errmsg(g_db) << "\n";
        return false;
    }
    sqlite3_busy_timeout(g_db, 5000);
    std::ifstream f(schemaPath);
    if (!f.is_open()) {
        std::cerr << "[ERROR] No se encontro el esquema: " << schemaPath << "\n";
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return execSql(g_db, ss.str());
}

// Requiere g_dbMutex tomado.
bool insertTelemetry(const LecturaActual& lec, const std::string& nodo) {
    const char* sql =
        "INSERT INTO telemetry (nodo_id, timestamp_utc, level_cm, level_pct, drop_rate_cm_h, temp_c, "
        "humidity_pct, pressure_hpa, eto_mm_day, edge_risk_state, alarm_silenced_ack) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?);";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, nodo.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, lec.timestamp_utc.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(st, 3, lec.level_cm);
    sqlite3_bind_double(st, 4, lec.level_pct);
    sqlite3_bind_double(st, 5, lec.drop_rate_cm_h);
    sqlite3_bind_double(st, 6, lec.temp_c);
    sqlite3_bind_double(st, 7, lec.humidity_pct);
    sqlite3_bind_double(st, 8, lec.pressure_hpa);
    sqlite3_bind_double(st, 9, lec.eto_mm_day);
    sqlite3_bind_int(st, 10, lec.edge_risk_state);
    sqlite3_bind_int(st, 11, lec.alarm_silenced_ack ? 1 : 0);
    const bool ok = (sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
    return ok;
}

// Requiere g_dbMutex tomado.
bool insertRecommendation(const iot::gateway::AiRecommendationResult& r) {
    const char* sql =
        "INSERT INTO ia_recommendations (nodo_id, timestamp_analisis, nivel_riesgo_sugerido, etiqueta_alerta, "
        "tipo_evento, anomalia_estadistica, horas_a_critico, decision_cmgrd, mensaje_comunidad, prioridad, "
        "metodo_aplicado, raw_json) VALUES (?,?,?,?,?,?,?,?,?,?,?,?);";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, r.nodo_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, r.timestamp_analisis.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, r.diagnostico_ia.nivel_riesgo_sugerido);
    sqlite3_bind_text(st, 4, r.diagnostico_ia.etiqueta_alerta.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, r.diagnostico_ia.tipo_evento.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 6, r.diagnostico_ia.anomalia_estadistica_detectada ? 1 : 0);
    sqlite3_bind_double(st, 7, r.prediccion_tendencia.horas_hasta_nivel_critico_25pct);
    sqlite3_bind_text(st, 8, r.recomendacion_autoridades.decision_sugerida_cmgrd_uaegrd.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 9, r.recomendacion_autoridades.mensaje_comunidad_acueducto.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 10, r.recomendacion_autoridades.prioridad_atencion.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 11, r.auditoria_cove.metodo_aplicado.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 12, r.raw_json.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = (sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
    return ok;
}

// Store: toda salida hacia Ubidots se guarda primero en SQLite. Requiere g_dbMutex tomado.
void enqueueUbidots(const std::string& nodo, const char* kind, const std::string& payload) {
    if (!g_ubiEnabled) return;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(g_db, "INSERT INTO store_and_forward_queue (nodo_id, kind, payload) VALUES (?,?,?);",
                           -1, &st, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, nodo.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, payload.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

void requestAi(const std::string& nodo) {
    {
        std::lock_guard<std::mutex> lk(g_wakeMutex);
        g_aiPending.insert(nodo);
    }
    g_wakeCv.notify_all();
}

// ---------------------------------------------------------------------------
// Broker local Mosquitto: ingesta agua/<nodo_id>/{telemetria,alerta,estado,confirmacion}
// ---------------------------------------------------------------------------
void onLocalConnect(struct mosquitto* m, void*, int rc) {
    if (rc != 0) {
        std::cerr << "[MQTT-LOCAL] Conexion rechazada rc=" << rc << "\n";
        return;
    }
    for (const char* sub : {"agua/+/telemetria", "agua/+/alerta", "agua/+/estado", "agua/+/confirmacion"}) {
        mosquitto_subscribe(m, nullptr, sub, 1);
    }
    std::cout << "[MQTT-LOCAL] Conectado y suscrito al arbol agua/+/...\n";
}

void handleTelemetry(const std::string& nodo, const std::string& body) {
    if (!hasKey(body, "level_cm")) return;

    LecturaActual lec;
    lec.timestamp_utc = nowIso();  // El reloj del Gateway es la referencia: el ESP32 puede no tener NTP
    lec.level_cm = jsonFloat(body, "level_cm", -1.0f);
    lec.level_pct = jsonFloat(body, "level_pct", -1.0f);
    lec.drop_rate_cm_h = jsonFloat(body, "drop_rate_cm_h", 0.0f);
    lec.temp_c = jsonFloat(body, "temp_c", 20.0f);
    lec.humidity_pct = jsonFloat(body, "humidity_pct", 65.0f);
    lec.pressure_hpa = jsonFloat(body, "pressure_hpa", -1.0f);
    lec.eto_mm_day = jsonFloat(body, "eto_mm_day", 0.0f);
    lec.edge_risk_state = static_cast<int>(jsonFloat(body, "risk_state", 0.0f));
    lec.alarm_silenced_ack = jsonBool(body, "alarm_silenced");

    bool riskChanged = false;
    {
        std::lock_guard<std::mutex> lk(g_dbMutex);
        if (!insertTelemetry(lec, nodo)) return;

        const auto now = std::chrono::steady_clock::now();
        const auto it = g_rate.find(nodo);
        const bool first = (it == g_rate.end());
        riskChanged = !first && it->second.second != lec.edge_risk_state;
        const bool due = first || riskChanged ||
            std::chrono::duration_cast<std::chrono::seconds>(now - it->second.first).count() >= UBIDOTS_MIN_PERIOD_SEC;
        if (due) {
            g_rate[nodo] = {now, lec.edge_risk_state};
            enqueueUbidots(nodo, "telemetria", fmt(
                "{\"level_cm\":%.2f,\"level_pct\":%.1f,\"drop_rate_cm_h\":%.2f,\"temp_c\":%.2f,"
                "\"humidity_pct\":%.1f,\"pressure_hpa\":%.1f,\"eto_mm_day\":%.2f,\"risk_state\":%d,\"alarm_silenced\":%d}",
                lec.level_cm, lec.level_pct, lec.drop_rate_cm_h, lec.temp_c, lec.humidity_pct,
                lec.pressure_hpa, lec.eto_mm_day, lec.edge_risk_state, lec.alarm_silenced_ack ? 1 : 0));
        }
    }
    if (riskChanged) requestAi(nodo);
}

void onLocalMessage(struct mosquitto*, void*, const struct mosquitto_message* msg) {
    if (!msg || !msg->payload || msg->payloadlen <= 0 || msg->payloadlen > 2048) return;
    std::string nodo, kind;
    if (!parseTopic(msg->topic, nodo, kind)) return;
    const std::string body(static_cast<const char*>(msg->payload), static_cast<size_t>(msg->payloadlen));

    if (kind == "telemetria") {
        handleTelemetry(nodo, body);
    } else if (kind == "alerta") {
        std::lock_guard<std::mutex> lk(g_dbMutex);
        if (hasKey(body, "risk_state")) {
            enqueueUbidots(nodo, "alerta", fmt("{\"risk_state\":%d}", static_cast<int>(jsonFloat(body, "risk_state", 0.0f))));
        } else if (body.find("LEVEL_SENSOR_ERROR") != std::string::npos) {
            enqueueUbidots(nodo, "alerta", "{\"sensor_fault\":1}");
        } else if (body.find("LEVEL_SENSOR_OK") != std::string::npos) {
            enqueueUbidots(nodo, "alerta", "{\"sensor_fault\":0}");
        }
    } else if (kind == "estado") {
        std::lock_guard<std::mutex> lk(g_dbMutex);
        enqueueUbidots(nodo, "estado", fmt("{\"online\":%d}", body.find("online") != std::string::npos ? 1 : 0));
    } else if (kind == "confirmacion") {
        std::lock_guard<std::mutex> lk(g_dbMutex);
        enqueueUbidots(nodo, "confirmacion", fmt("{\"buzzer_muted\":%d}", jsonBool(body, "buzzer_muted") ? 1 : 0));
    }
}

// ---------------------------------------------------------------------------
// Ubidots: reenvio (forward) de la cola y comandos entrantes (silenciar alarma)
// ---------------------------------------------------------------------------
void onUbidotsConnect(struct mosquitto* m, void*, int rc) {
    if (rc != 0) {
        std::cerr << "[UBIDOTS] Conexion rechazada rc=" << rc << "\n";
        return;
    }
    g_ubiConnected = true;
    for (const auto& n : g_nodes) {
        const std::string t = "/v1.6/devices/" + n + "/silence_alarm/lv";
        mosquitto_subscribe(m, nullptr, t.c_str(), 1);
    }
    std::cout << "[UBIDOTS] Conectado; vaciando cola store-and-forward.\n";
}

void onUbidotsDisconnect(struct mosquitto*, void*, int) {
    g_ubiConnected = false;
    std::cerr << "[UBIDOTS] Desconectado; los registros se acumulan en SQLite.\n";
}

void onUbidotsPublish(struct mosquitto*, void*, int mid) {
    {
        std::lock_guard<std::mutex> lk(g_ackMutex);
        g_ackedMids.insert(mid);
    }
    g_ackCv.notify_all();
}

void onUbidotsMessage(struct mosquitto*, void*, const struct mosquitto_message* msg) {
    if (!msg || !msg->payload || msg->payloadlen <= 0 || msg->payloadlen > 32) return;
    const std::string prefix = "/v1.6/devices/";
    const std::string topic = msg->topic;
    if (topic.rfind(prefix, 0) != 0) return;
    const size_t end = topic.find('/', prefix.size());
    if (end == std::string::npos) return;
    const std::string nodo = topic.substr(prefix.size(), end - prefix.size());
    if (!validNodeId(nodo)) return;
    const std::string val(static_cast<const char*>(msg->payload), static_cast<size_t>(msg->payloadlen));
    if (std::atof(val.c_str()) <= 0.0) return;

    const std::string cmd = fmt("{\"cmd\":\"SILENCE_ALARM\",\"duration_sec\":300,\"source\":\"ubidots\",\"req_id\":\"ubi-%ld\"}",
                                static_cast<long>(std::time(nullptr)));
    mosquitto_publish(g_local, nullptr, ("agua/" + nodo + "/comando").c_str(),
                      static_cast<int>(cmd.size()), cmd.c_str(), 1, false);
}

void forwarderWorker() {
    auto lastPrune = std::chrono::steady_clock::now();
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (!g_ubiEnabled || !g_ubiConnected) continue;

        struct Row { long long id; std::string nodo; std::string payload; };
        std::vector<Row> rows;
        {
            std::lock_guard<std::mutex> lk(g_dbMutex);
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(g_db,
                    "SELECT id, nodo_id, payload FROM store_and_forward_queue WHERE sent = 0 ORDER BY id LIMIT ?;",
                    -1, &st, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(st, 1, FORWARD_BATCH);
                while (sqlite3_step(st) == SQLITE_ROW) {
                    rows.push_back({sqlite3_column_int64(st, 0),
                                    reinterpret_cast<const char*>(sqlite3_column_text(st, 1)),
                                    reinterpret_cast<const char*>(sqlite3_column_text(st, 2))});
                }
                sqlite3_finalize(st);
            }
        }

        for (const auto& r : rows) {
            if (!g_running || !g_ubiConnected) break;
            int mid = 0;
            const std::string topic = "/v1.6/devices/" + r.nodo;
            if (mosquitto_publish(g_ubidots, &mid, topic.c_str(), static_cast<int>(r.payload.size()),
                                  r.payload.c_str(), 1, false) != MOSQ_ERR_SUCCESS) break;

            bool acked = false;
            {
                std::unique_lock<std::mutex> lk(g_ackMutex);
                acked = g_ackCv.wait_for(lk, std::chrono::seconds(5), [mid] { return g_ackedMids.count(mid) > 0; });
                g_ackedMids.erase(mid);
            }
            if (!acked) break;  // Sin PUBACK el registro sigue pendiente y se reintenta

            std::lock_guard<std::mutex> lk(g_dbMutex);
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(g_db,
                    "UPDATE store_and_forward_queue SET sent = 1, sent_at = strftime('%Y-%m-%dT%H:%M:%SZ','now') WHERE id = ?;",
                    -1, &st, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(st, 1, r.id);
                sqlite3_step(st);
                sqlite3_finalize(st);
            }
        }

        if (std::chrono::steady_clock::now() - lastPrune > std::chrono::hours(1)) {
            lastPrune = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lk(g_dbMutex);
            execSql(g_db, "DELETE FROM store_and_forward_queue WHERE sent = 1 AND "
                          "sent_at < strftime('%Y-%m-%dT%H:%M:%SZ','now','-7 days');");
        }
    }
}

// ---------------------------------------------------------------------------
// Motor de IA: OLS + Z-score (SQLite) + OpenRouter Free con fallback local
// ---------------------------------------------------------------------------
void aiWorker(std::string apiKey) {
    while (g_running) {
        std::vector<std::string> targets;
        {
            std::unique_lock<std::mutex> lk(g_wakeMutex);
            g_wakeCv.wait_for(lk, std::chrono::seconds(g_aiPeriodSec),
                              [] { return !g_aiPending.empty() || !g_running; });
            if (!g_running) break;
            if (g_aiPending.empty()) {
                targets = g_nodes;
            } else {
                targets.assign(g_aiPending.begin(), g_aiPending.end());
                g_aiPending.clear();
            }
        }

        for (const auto& nodo : targets) {
            iot::gateway::GatewayPayload payload;
            bool ok = true;
            {
                std::lock_guard<std::mutex> lk(g_dbMutex);
                try {
                    payload = iot::gateway::buildSqliteGatewayPayload(g_db, nodo);
                } catch (const std::exception&) {
                    ok = false;  // Aun no hay telemetria de este nodo
                }
            }
            if (!ok) continue;

            const auto rec = iot::gateway::generateAiRecommendation(payload, apiKey);  // Red fuera del lock

            {
                std::lock_guard<std::mutex> lk(g_dbMutex);
                insertRecommendation(rec);
                enqueueUbidots(nodo, "ia", fmt(
                    "{\"ia_risk_level\":%d,\"ia_hours_to_critical\":%.1f,\"ia_anomaly\":%d}",
                    rec.diagnostico_ia.nivel_riesgo_sugerido,
                    rec.prediccion_tendencia.horas_hasta_nivel_critico_25pct,
                    rec.diagnostico_ia.anomalia_estadistica_detectada ? 1 : 0));
            }
            mosquitto_publish(g_local, nullptr, ("agua/" + nodo + "/ia/recomendacion").c_str(),
                              static_cast<int>(rec.raw_json.size()), rec.raw_json.c_str(), 1, true);

            std::cout << "[IA] " << nodo << ": " << rec.diagnostico_ia.tipo_evento << " ("
                      << rec.auditoria_cove.metodo_aplicado << ")\n";
        }
    }
}

struct mosquitto* makeClient(const char* id, const std::string& user, const std::string& pass) {
    struct mosquitto* m = mosquitto_new(id, true, nullptr);
    if (m && !user.empty()) mosquitto_username_pw_set(m, user.c_str(), pass.c_str());
    if (m) mosquitto_reconnect_delay_set(m, 2, 30, true);
    return m;
}

} // namespace

int main() {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // Secretos solo por entorno (.env fuera de VCS)
    const std::string dbPath = envOr("SQLITE_DB_PATH", "sabana_iot_ch3.db");
    const std::string schemaPath = envOr("SCHEMA_PATH", "schema.sql");
    const std::string mqttHost = envOr("MQTT_BROKER_HOST", "127.0.0.1");
    const int mqttPort = std::atoi(envOr("MQTT_BROKER_PORT", "1883").c_str());
    const std::string mqttUser = envOr("MQTT_USERNAME", "");
    const std::string mqttPass = envOr("MQTT_PASSWORD", "");
    const std::string ubiToken = envOr("UBIDOTS_TOKEN", "");
    const std::string ubiHost = envOr("UBIDOTS_HOST", "industrial.api.ubidots.com");
    const int ubiPort = std::atoi(envOr("UBIDOTS_PORT", "8883").c_str());
    const std::string apiKey = envOr("OPENROUTER_API_KEY", "");
    g_aiPeriodSec = std::max(10, std::atoi(envOr("AI_PERIOD_SEC", "60").c_str()));

    std::stringstream nodeList(envOr("NODE_IDS", "nodo01"));
    for (std::string n; std::getline(nodeList, n, ',');) {
        if (validNodeId(n)) g_nodes.push_back(n);
    }
    if (g_nodes.empty()) {
        std::cerr << "[ERROR] NODE_IDS no contiene identificadores validos.\n";
        return 1;
    }

    g_ubiEnabled = !ubiToken.empty();
    if (!initDatabase(dbPath, schemaPath)) return 1;
    std::cout << "[OK] SQLite (WAL) en " << dbPath << " | Ubidots: " << (g_ubiEnabled ? "activo" : "desactivado")
              << " | OpenRouter: " << (apiKey.empty() ? "fallback local" : "activo") << "\n";

    mosquitto_lib_init();

    g_local = makeClient("sabana-gateway-local", mqttUser, mqttPass);
    if (!g_local) return 1;
    mosquitto_connect_callback_set(g_local, onLocalConnect);
    mosquitto_message_callback_set(g_local, onLocalMessage);
    mosquitto_connect_async(g_local, mqttHost.c_str(), mqttPort, 30);
    mosquitto_loop_start(g_local);

    if (g_ubiEnabled) {
        g_ubidots = makeClient("sabana-gateway-ubidots", ubiToken, "");
        if (g_ubidots) {
            if (ubiPort == 8883) mosquitto_tls_set(g_ubidots, nullptr, "/etc/ssl/certs", nullptr, nullptr, nullptr);
            mosquitto_connect_callback_set(g_ubidots, onUbidotsConnect);
            mosquitto_disconnect_callback_set(g_ubidots, onUbidotsDisconnect);
            mosquitto_publish_callback_set(g_ubidots, onUbidotsPublish);
            mosquitto_message_callback_set(g_ubidots, onUbidotsMessage);
            mosquitto_connect_async(g_ubidots, ubiHost.c_str(), ubiPort, 30);
            mosquitto_loop_start(g_ubidots);
        }
    }

    std::thread forwarder(forwarderWorker);
    std::thread ai(aiWorker, apiKey);

    while (g_running) std::this_thread::sleep_for(std::chrono::milliseconds(300));

    g_wakeCv.notify_all();
    g_ackCv.notify_all();
    ai.join();
    forwarder.join();

    mosquitto_disconnect(g_local);
    mosquitto_loop_stop(g_local, true);
    mosquitto_destroy(g_local);
    if (g_ubidots) {
        mosquitto_disconnect(g_ubidots);
        mosquitto_loop_stop(g_ubidots, true);
        mosquitto_destroy(g_ubidots);
    }
    mosquitto_lib_cleanup();
    sqlite3_close(g_db);
    std::cout << "[OK] Gateway cerrado.\n";
    return 0;
}
