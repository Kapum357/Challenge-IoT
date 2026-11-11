-- ============================================================================
-- Esquema de Base de Datos SQLite para Gateway IoT (Raspberry Pi) — Equipo 4
-- Challenge #3: Monitoreo y Alerta Hidroclimática en Sabana Centro (2026-2)
-- ============================================================================

PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;

-- Tabla de Ingesta Periódica de Telemetría desde Nodos ESP32 (Vía MQTT)
-- Nota de diseño: Se elimina por completo el sensor de caudal YF-S201.
CREATE TABLE IF NOT EXISTS telemetry (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    nodo_id TEXT NOT NULL,
    timestamp_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    level_cm REAL NOT NULL,
    level_pct REAL NOT NULL,
    drop_rate_cm_h REAL NOT NULL,
    temp_c REAL NOT NULL,
    humidity_pct REAL NOT NULL,
    pressure_hpa REAL NOT NULL,
    eto_mm_day REAL NOT NULL,
    edge_risk_state INTEGER NOT NULL,
    alarm_silenced_ack INTEGER NOT NULL DEFAULT 0,
    created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))
);

-- Índices de consulta rápida para ventanas analíticas (1 hora y 24 horas)
CREATE INDEX IF NOT EXISTS idx_telemetry_nodo_time
    ON telemetry(nodo_id, timestamp_utc DESC);

CREATE INDEX IF NOT EXISTS idx_telemetry_valid_samples
    ON telemetry(nodo_id, timestamp_utc DESC)
    WHERE level_cm >= 0;

-- Tabla de Registro Histórico de Diagnósticos y Recomendaciones de IA
CREATE TABLE IF NOT EXISTS ia_recommendations (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    nodo_id TEXT NOT NULL,
    timestamp_analisis TEXT NOT NULL,
    nivel_riesgo_sugerido INTEGER NOT NULL,
    etiqueta_alerta TEXT NOT NULL,
    tipo_evento TEXT NOT NULL,
    anomalia_estadistica INTEGER NOT NULL,
    horas_a_critico REAL NOT NULL,
    decision_cmgrd TEXT NOT NULL,
    mensaje_comunidad TEXT NOT NULL,
    prioridad TEXT NOT NULL,
    metodo_aplicado TEXT NOT NULL,
    raw_json TEXT NOT NULL,
    created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_ia_recom_nodo_time
    ON ia_recommendations(nodo_id, timestamp_analisis DESC);

-- Cola store-and-forward hacia Ubidots: se escribe primero en SQLite y se vacia al reconectar.
CREATE TABLE IF NOT EXISTS store_and_forward_queue (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    nodo_id TEXT NOT NULL,
    kind TEXT NOT NULL,          -- telemetria | alerta | estado | confirmacion | ia
    payload TEXT NOT NULL,       -- JSON listo para /v1.6/devices/<nodo_id>
    sent INTEGER NOT NULL DEFAULT 0,
    attempts INTEGER NOT NULL DEFAULT 0,
    created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    sent_at TEXT
);

CREATE INDEX IF NOT EXISTS idx_saf_pending
    ON store_and_forward_queue(sent, id) WHERE sent = 0;
