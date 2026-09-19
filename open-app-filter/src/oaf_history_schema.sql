-- OpenAppFilter history database schema v1.
-- Traffic tables use OpenAppFilter's integer app ID as app_id.
-- Device IDs are compact surrogate keys for MAC addresses.
CREATE TABLE IF NOT EXISTS device (
    device_id INTEGER PRIMARY KEY,
    mac TEXT NOT NULL UNIQUE,
    hostname TEXT
);

CREATE TABLE IF NOT EXISTS app (
    app_id INTEGER PRIMARY KEY,
    app_name TEXT NOT NULL UNIQUE
);

CREATE TABLE IF NOT EXISTS traffic_minute (
    ts_minute INTEGER NOT NULL,
    device_id INTEGER NOT NULL,
    app_id INTEGER NOT NULL,
    traffic_kb INTEGER NOT NULL CHECK (traffic_kb > 0),
    PRIMARY KEY (ts_minute, device_id, app_id)
);

CREATE TABLE IF NOT EXISTS traffic_day (
    day INTEGER NOT NULL,
    device_id INTEGER NOT NULL,
    app_id INTEGER NOT NULL,
    traffic_kb INTEGER NOT NULL CHECK (traffic_kb > 0),
    PRIMARY KEY (day, device_id, app_id)
);

CREATE TABLE IF NOT EXISTS traffic_month (
    month INTEGER NOT NULL,
    device_id INTEGER NOT NULL,
    app_id INTEGER NOT NULL,
    traffic_kb INTEGER NOT NULL CHECK (traffic_kb > 0),
    PRIMARY KEY (month, device_id, app_id)
);

CREATE INDEX IF NOT EXISTS idx_minute_device_time
    ON traffic_minute(device_id, ts_minute);
CREATE INDEX IF NOT EXISTS idx_day_device_time
    ON traffic_day(device_id, day);
CREATE INDEX IF NOT EXISTS idx_month_device_time
    ON traffic_month(device_id, month);

CREATE INDEX IF NOT EXISTS idx_minute_app_time
    ON traffic_minute(app_id, ts_minute);
CREATE INDEX IF NOT EXISTS idx_day_app_time
    ON traffic_day(app_id, day);
CREATE INDEX IF NOT EXISTS idx_month_app_time
    ON traffic_month(app_id, month);

-- Existing App visit/session history is preserved here so the legacy LuCI
-- "History Records" page can continue working after client.db migration.
CREATE TABLE IF NOT EXISTS visit_history (
    visit_id INTEGER PRIMARY KEY,
    device_id INTEGER NOT NULL,
    app_id INTEGER NOT NULL,
    start_time INTEGER NOT NULL,
    end_time INTEGER NOT NULL,
    duration INTEGER NOT NULL,
    action INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_visit_history_device_time
    ON visit_history(device_id, end_time);
CREATE INDEX IF NOT EXISTS idx_visit_history_app_time
    ON visit_history(app_id, end_time);

CREATE TABLE IF NOT EXISTS history_meta (
    meta_key TEXT PRIMARY KEY,
    meta_value TEXT NOT NULL
);

PRAGMA user_version=1;
