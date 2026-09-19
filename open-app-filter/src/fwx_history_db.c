// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright(c) 2026 OpenAppFilter contributors
 *
 * SQLite access is intentionally isolated in this module. Runtime packet
 * handling only enqueues minute buckets in memory; SQLite is touched by the
 * service timer, never from the packet path.
 */
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "fwx.h"
#include "fwx_config.h"
#include "fwx_history_db.h"

typedef struct pending_flow {
    time_t ts_minute;
    int app_id;
    uint64_t traffic_bytes;
    char mac[32];
    struct pending_flow *next;
} pending_flow_t;

typedef struct device_cache {
    char mac[32];
    int device_id;
    struct device_cache *next;
} device_cache_t;

typedef struct app_cache {
    int app_id;
    int known;
    struct app_cache *next;
} app_cache_t;

static sqlite3 *g_db = NULL;
static int g_db_ready = 0;
static int g_db_disabled = 0;
static time_t g_db_last_failure = 0;
static pending_flow_t *g_pending = NULL;
static size_t g_pending_count = 0;
static device_cache_t *g_device_cache = NULL;
static app_cache_t *g_app_cache = NULL;
static pthread_mutex_t g_db_lock = PTHREAD_MUTEX_INITIALIZER;
static time_t g_last_maintenance_day = (time_t)-1;

static sqlite3_int64 traffic_bytes_to_kb(uint64_t bytes)
{
    sqlite3_int64 kb = (sqlite3_int64)(bytes / 1024ULL);
    if (bytes % 1024ULL)
        kb++;
    return kb > 0 ? kb : 1;
}

static int mkdir_one(const char *path)
{
    if (mkdir(path, 0755) == 0 || errno == EEXIST)
        return 0;
    LOG_ERROR("history db: mkdir %s failed: %s\n", path, strerror(errno));
    return -1;
}

static int db_exec(const char *sql)
{
    char *errmsg = NULL;
    int rc;

    rc = sqlite3_exec(g_db, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        LOG_ERROR("history db SQL failed rc=%d: %s\n", rc, errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
        return -1;
    }
    return 0;
}

static int db_begin(void)
{
    return sqlite3_exec(g_db, "BEGIN IMMEDIATE TRANSACTION;", NULL, NULL, NULL);
}

static int db_commit(void)
{
    return sqlite3_exec(g_db, "COMMIT;", NULL, NULL, NULL);
}

static void db_rollback(void)
{
    if (g_db)
        sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
}

static void requeue_pending_locked(pending_flow_t *list)
{
    pending_flow_t *p = list;

    while (p) {
        pending_flow_t *next = p->next;
        p->next = g_pending;
        g_pending = p;
        g_pending_count++;
        p = next;
    }
}

static int db_is_fatal_error(int rc)
{
    return rc == SQLITE_FULL ||
           rc == SQLITE_IOERR ||
           rc == SQLITE_CORRUPT ||
           rc == SQLITE_NOTADB ||
           rc == SQLITE_CANTOPEN ||
           rc == SQLITE_READONLY;
}

static void free_pending(void)
{
    pending_flow_t *p = g_pending;
    while (p) {
        pending_flow_t *next = p->next;
        free(p);
        p = next;
    }
    g_pending = NULL;
    g_pending_count = 0;
}

static void free_caches(void)
{
    device_cache_t *d = g_device_cache;
    app_cache_t *a = g_app_cache;

    while (d) {
        device_cache_t *next = d->next;
        free(d);
        d = next;
    }
    while (a) {
        app_cache_t *next = a->next;
        free(a);
        a = next;
    }
    g_device_cache = NULL;
    g_app_cache = NULL;
}

static void disable_db_locked(int rc)
{
    g_db_last_failure = time(NULL);
    g_db_disabled = 1;
    g_db_ready = 0;
    LOG_ERROR("history db disabled after SQLite error rc=%d; realtime filtering is unaffected\n", rc);
    free_caches();
}

static int get_user_version(int *version)
{
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (!version)
        return -1;

    rc = sqlite3_prepare_v2(g_db, "PRAGMA user_version;", -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return -1;
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
        *version = sqlite3_column_int(stmt, 0);
    else {
        sqlite3_finalize(stmt);
        return -1;
    }
    sqlite3_finalize(stmt);
    return 0;
}

static int set_user_version(int version)
{
    char sql[64];
    snprintf(sql, sizeof(sql), "PRAGMA user_version=%d;", version);
    return db_exec(sql);
}

static int create_schema_v1(void)
{
    static const char *schema =
        "CREATE TABLE IF NOT EXISTS device ("
        "device_id INTEGER PRIMARY KEY,"
        "mac TEXT NOT NULL UNIQUE,"
        "hostname TEXT"
        ");"
        "CREATE TABLE IF NOT EXISTS app ("
        "app_id INTEGER PRIMARY KEY,"
        "app_name TEXT NOT NULL UNIQUE"
        ");"
        "CREATE TABLE IF NOT EXISTS traffic_minute ("
        "ts_minute INTEGER NOT NULL,"
        "device_id INTEGER NOT NULL,"
        "app_id INTEGER NOT NULL,"
        "traffic_kb INTEGER NOT NULL CHECK (traffic_kb > 0),"
        "PRIMARY KEY (ts_minute, device_id, app_id)"
        ");"
        "CREATE TABLE IF NOT EXISTS traffic_day ("
        "day INTEGER NOT NULL,"
        "device_id INTEGER NOT NULL,"
        "app_id INTEGER NOT NULL,"
        "traffic_kb INTEGER NOT NULL CHECK (traffic_kb > 0),"
        "PRIMARY KEY (day, device_id, app_id)"
        ");"
        "CREATE TABLE IF NOT EXISTS traffic_month ("
        "month INTEGER NOT NULL,"
        "device_id INTEGER NOT NULL,"
        "app_id INTEGER NOT NULL,"
        "traffic_kb INTEGER NOT NULL CHECK (traffic_kb > 0),"
        "PRIMARY KEY (month, device_id, app_id)"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_minute_device_time "
        "ON traffic_minute(device_id, ts_minute);"
        "CREATE INDEX IF NOT EXISTS idx_day_device_time "
        "ON traffic_day(device_id, day);"
        "CREATE INDEX IF NOT EXISTS idx_month_device_time "
        "ON traffic_month(device_id, month);"
        "CREATE INDEX IF NOT EXISTS idx_minute_app_time "
        "ON traffic_minute(app_id, ts_minute);"
        "CREATE INDEX IF NOT EXISTS idx_day_app_time "
        "ON traffic_day(app_id, day);"
        "CREATE INDEX IF NOT EXISTS idx_month_app_time "
        "ON traffic_month(app_id, month);"
        "CREATE TABLE IF NOT EXISTS visit_history ("
        "visit_id INTEGER PRIMARY KEY,"
        "device_id INTEGER NOT NULL,"
        "app_id INTEGER NOT NULL,"
        "start_time INTEGER NOT NULL,"
        "end_time INTEGER NOT NULL,"
        "duration INTEGER NOT NULL,"
        "action INTEGER NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_visit_history_device_time "
        "ON visit_history(device_id, end_time);"
        "CREATE INDEX IF NOT EXISTS idx_visit_history_app_time "
        "ON visit_history(app_id, end_time);"
        "CREATE TABLE IF NOT EXISTS history_meta ("
        "meta_key TEXT PRIMARY KEY,"
        "meta_value TEXT NOT NULL"
        ");";

    return db_exec(schema);
}

static int migrate_schema_locked(void)
{
    int version = 0;

    if (get_user_version(&version) != 0)
        return -1;

    if (version == 0) {
        if (create_schema_v1() != 0)
            return -1;
        if (set_user_version(1) != 0)
            return -1;
        version = 1;
    }

    if (version == 1)
        return 0;

    LOG_ERROR("history db: unsupported schema version %d\n", version);
    return -1;
}

static device_cache_t *find_device_cache(const char *mac)
{
    device_cache_t *p;
    for (p = g_device_cache; p; p = p->next) {
        if (strcmp(p->mac, mac) == 0)
            return p;
    }
    return NULL;
}

static int get_or_create_device_locked(const char *mac, const char *hostname)
{
    device_cache_t *cached;
    sqlite3_stmt *stmt = NULL;
    int rc;
    int device_id = -1;

    cached = find_device_cache(mac);
    if (cached)
        return cached->device_id;

    rc = sqlite3_prepare_v2(
        g_db,
        "INSERT INTO device(mac, hostname) VALUES(?, ?) "
        "ON CONFLICT(mac) DO UPDATE SET "
        "hostname = CASE WHEN excluded.hostname IS NOT NULL AND excluded.hostname <> '' "
        "THEN excluded.hostname ELSE device.hostname END;",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return -1;

    sqlite3_bind_text(stmt, 1, mac, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, hostname ? hostname : "", -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return -1;

    rc = sqlite3_prepare_v2(g_db,
                            "SELECT device_id FROM device WHERE mac = ?;",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, mac, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
        device_id = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    if (device_id < 0)
        return -1;

    cached = calloc(1, sizeof(*cached));
    if (!cached)
        return device_id;
    strncpy(cached->mac, mac, sizeof(cached->mac) - 1);
    cached->device_id = device_id;
    cached->next = g_device_cache;
    g_device_cache = cached;
    return device_id;
}

static int ensure_app_locked(int app_id)
{
    app_cache_t *cached;
    sqlite3_stmt *stmt = NULL;
    int rc;
    const char *name;

    cached = g_app_cache;
    while (cached) {
        if (cached->app_id == app_id)
            return 0;
        cached = cached->next;
    }

    name = get_app_name_by_id(app_id);
    if (!name || !*name) {
        char fallback[64];
        snprintf(fallback, sizeof(fallback), "App%d", app_id);
        name = fallback;

        rc = sqlite3_prepare_v2(
            g_db,
            "INSERT INTO app(app_id, app_name) VALUES(?, ?) "
            "ON CONFLICT(app_id) DO UPDATE SET app_name=excluded.app_name;",
            -1, &stmt, NULL);
        if (rc != SQLITE_OK)
            return -1;
        sqlite3_bind_int(stmt, 1, app_id);
        sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
    } else {
        rc = sqlite3_prepare_v2(
            g_db,
            "INSERT INTO app(app_id, app_name) VALUES(?, ?) "
            "ON CONFLICT(app_id) DO UPDATE SET app_name=excluded.app_name;",
            -1, &stmt, NULL);
        if (rc != SQLITE_OK)
            return -1;
        sqlite3_bind_int(stmt, 1, app_id);
        sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return -1;

    cached = calloc(1, sizeof(*cached));
    if (cached) {
        cached->app_id = app_id;
        cached->known = 1;
        cached->next = g_app_cache;
        g_app_cache = cached;
    }
    return 0;
}

static pending_flow_t *find_pending(const char *mac, int app_id, time_t ts_minute)
{
    pending_flow_t *p;
    for (p = g_pending; p; p = p->next) {
        if (p->ts_minute == ts_minute &&
            p->app_id == app_id &&
            strcmp(p->mac, mac) == 0)
            return p;
    }
    return NULL;
}

int oaf_history_db_init(void)
{
    int rc;
    int version = 0;

    pthread_mutex_lock(&g_db_lock);

    if (g_db_ready) {
        pthread_mutex_unlock(&g_db_lock);
        return 0;
    }

    if (mkdir_one(OAF_HISTORY_DB_DIR) != 0) {
        pthread_mutex_unlock(&g_db_lock);
        return -1;
    }

    if (g_db_disabled && g_db_last_failure != 0 &&
        time(NULL) - g_db_last_failure < 300) {
        pthread_mutex_unlock(&g_db_lock);
        return -1;
    }

    if (g_db)
        sqlite3_close(g_db);
    g_db = NULL;

    rc = sqlite3_open_v2(OAF_HISTORY_DB_PATH, &g_db,
                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                         SQLITE_OPEN_FULLMUTEX, NULL);
    if (rc != SQLITE_OK) {
        LOG_ERROR("history db: open %s failed rc=%d: %s\n",
                  OAF_HISTORY_DB_PATH, rc, g_db ? sqlite3_errmsg(g_db) : "unknown");
        if (g_db)
            sqlite3_close(g_db);
        g_db = NULL;
        g_db_ready = 0;
        g_db_disabled = 1;
        g_db_last_failure = time(NULL);
        pthread_mutex_unlock(&g_db_lock);
        return -1;
    }

    sqlite3_busy_timeout(g_db, 1000);

    /* Phase 1 deliberately keeps rollback journal + FULL synchronous. */
    rc = sqlite3_exec(g_db,
                      "PRAGMA foreign_keys=OFF;"
                      "PRAGMA journal_mode=DELETE;"
                      "PRAGMA synchronous=FULL;",
                      NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        LOG_ERROR("history db: PRAGMA init failed rc=%d\n", rc);
        sqlite3_close(g_db);
        g_db = NULL;
        g_db_ready = 0;
        g_db_disabled = 1;
        g_db_last_failure = time(NULL);
        pthread_mutex_unlock(&g_db_lock);
        return -1;
    }

    if (migrate_schema_locked() != 0) {
        LOG_ERROR("history db: schema migration failed\n");
        sqlite3_close(g_db);
        g_db = NULL;
        g_db_ready = 0;
        g_db_disabled = 1;
        g_db_last_failure = time(NULL);
        pthread_mutex_unlock(&g_db_lock);
        return -1;
    }

    if (get_user_version(&version) != 0 || version != 1) {
        LOG_ERROR("history db: invalid schema version %d\n", version);
        sqlite3_close(g_db);
        g_db = NULL;
        g_db_ready = 0;
        g_db_disabled = 1;
        g_db_last_failure = time(NULL);
        pthread_mutex_unlock(&g_db_lock);
        return -1;
    }

    g_db_ready = 1;
    g_db_disabled = 0;
    g_db_last_failure = 0;
    g_last_maintenance_day = (time_t)-1;
    pthread_mutex_unlock(&g_db_lock);

    LOG_INFO("history db ready: %s\n", OAF_HISTORY_DB_PATH);
    return 0;
}

void oaf_history_db_close(void)
{
    pthread_mutex_lock(&g_db_lock);

    if (g_db_ready && g_pending) {
        sqlite3_stmt *stmt = NULL;
        int tx_rc = db_begin();

        if (tx_rc == SQLITE_OK) {
            tx_rc = sqlite3_prepare_v2(
                g_db,
                "INSERT INTO traffic_minute(ts_minute, device_id, app_id, traffic_kb) "
                "VALUES(?, ?, ?, ?) "
                "ON CONFLICT(ts_minute, device_id, app_id) DO UPDATE SET "
                "traffic_kb = traffic_kb + excluded.traffic_kb;",
                -1, &stmt, NULL);
            if (tx_rc == SQLITE_OK) {
                pending_flow_t *p;
                for (p = g_pending; p; p = p->next) {
                    int device_id = get_or_create_device_locked(p->mac, "");
                    if (device_id < 0 || ensure_app_locked(p->app_id) != 0) {
                        tx_rc = SQLITE_ERROR;
                        break;
                    }
                    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)p->ts_minute);
                    sqlite3_bind_int(stmt, 2, device_id);
                    sqlite3_bind_int(stmt, 3, p->app_id);
                    sqlite3_bind_int64(stmt, 4, traffic_bytes_to_kb(p->traffic_bytes));
                    tx_rc = sqlite3_step(stmt);
                    sqlite3_reset(stmt);
                    sqlite3_clear_bindings(stmt);
                    if (tx_rc != SQLITE_DONE)
                        break;
                }
            }
        }
        if (stmt)
            sqlite3_finalize(stmt);
        if (tx_rc == SQLITE_DONE || tx_rc == SQLITE_OK) {
            if (db_commit() == SQLITE_OK)
                free_pending();
            else
                db_rollback();
        } else {
            db_rollback();
        }
    }

    free_pending();
    free_caches();

    if (g_db) {
        sqlite3_close(g_db);
        g_db = NULL;
    }

    g_db_ready = 0;
    pthread_mutex_unlock(&g_db_lock);
}

int oaf_history_db_is_ready(void)
{
    int ready;
    pthread_mutex_lock(&g_db_lock);
    ready = g_db_ready;
    pthread_mutex_unlock(&g_db_lock);
    return ready;
}

int oaf_history_db_record_minute_bytes(const char *mac,
                                       int app_id,
                                       time_t timestamp,
                                       uint64_t traffic_bytes)
{
    pending_flow_t *p;
    time_t ts_minute;

    if (!mac || !*mac || app_id <= 0 || traffic_bytes == 0)
        return 0;

    pthread_mutex_lock(&g_db_lock);

    if (!g_db_ready) {
        pthread_mutex_unlock(&g_db_lock);
        return -1;
    }

    ts_minute = (timestamp / 60) * 60;
    p = find_pending(mac, app_id, ts_minute);
    if (p) {
        p->traffic_bytes += traffic_bytes;
        pthread_mutex_unlock(&g_db_lock);
        return 0;
    }

    if (g_pending_count >= OAF_HISTORY_PENDING_MAX) {
        LOG_ERROR("history db: pending bucket limit reached; dropping history sample\n");
        pthread_mutex_unlock(&g_db_lock);
        return -1;
    }

    p = calloc(1, sizeof(*p));
    if (!p) {
        pthread_mutex_unlock(&g_db_lock);
        return -1;
    }

    p->ts_minute = ts_minute;
    p->app_id = app_id;
    p->traffic_bytes = traffic_bytes;
    strncpy(p->mac, mac, sizeof(p->mac) - 1);
    p->next = g_pending;
    g_pending = p;
    g_pending_count++;

    pthread_mutex_unlock(&g_db_lock);
    return 0;
}

int oaf_history_db_flush_due(time_t now)
{
    pending_flow_t *p;
    pending_flow_t *prev;
    pending_flow_t *next;
    sqlite3_stmt *stmt = NULL;
    int rc = SQLITE_OK;
    time_t cutoff = (now / 60) * 60;
    int transaction_started = 0;
    pending_flow_t *flush_list = NULL;

    pthread_mutex_lock(&g_db_lock);

    if (!g_db_ready || !g_pending) {
        pthread_mutex_unlock(&g_db_lock);
        return 0;
    }

    prev = NULL;
    p = g_pending;
    while (p) {
        next = p->next;
        if (p->ts_minute < cutoff) {
            p->next = flush_list;
            flush_list = p;
            if (prev)
                prev->next = next;
            else
                g_pending = next;
            g_pending_count--;
        } else {
            prev = p;
        }
        p = next;
    }

    if (!flush_list) {
        pthread_mutex_unlock(&g_db_lock);
        return 0;
    }

    rc = db_begin();
    if (rc != SQLITE_OK) {
        /* Put the records back so a transient failure can be retried. */
        requeue_pending_locked(flush_list);
        if (db_is_fatal_error(rc))
            disable_db_locked(rc);
        pthread_mutex_unlock(&g_db_lock);
        return -1;
    }
    transaction_started = 1;

    rc = sqlite3_prepare_v2(
        g_db,
        "INSERT INTO traffic_minute(ts_minute, device_id, app_id, traffic_kb) "
        "VALUES(?, ?, ?, ?) "
        "ON CONFLICT(ts_minute, device_id, app_id) DO UPDATE SET "
        "traffic_kb = traffic_kb + excluded.traffic_kb;",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        goto FAIL;

    for (p = flush_list; p; p = p->next) {
        int device_id = get_or_create_device_locked(p->mac, "");
        if (device_id < 0) {
            rc = SQLITE_ERROR;
            break;
        }
        if (ensure_app_locked(p->app_id) != 0) {
            rc = SQLITE_ERROR;
            break;
        }

        sqlite3_bind_int64(stmt, 1, (sqlite3_int64)p->ts_minute);
        sqlite3_bind_int(stmt, 2, device_id);
        sqlite3_bind_int(stmt, 3, p->app_id);
        sqlite3_bind_int64(stmt, 4, traffic_bytes_to_kb(p->traffic_bytes));
        rc = sqlite3_step(stmt);
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        if (rc != SQLITE_DONE)
            break;
    }

    if (rc == SQLITE_DONE || rc == SQLITE_OK) {
        rc = db_commit();
        if (rc == SQLITE_OK)
            transaction_started = 0;
    }

FAIL:
    if (stmt)
        sqlite3_finalize(stmt);
    if (rc != SQLITE_OK) {
        if (transaction_started)
            db_rollback();
        free_caches();
        requeue_pending_locked(flush_list);
        if (db_is_fatal_error(rc))
            disable_db_locked(rc);
        pthread_mutex_unlock(&g_db_lock);
        return -1;
    }

    p = flush_list;
    while (p) {
        next = p->next;
        free(p);
        p = next;
    }

    pthread_mutex_unlock(&g_db_lock);
    return 0;
}

int oaf_history_db_flush_all(void)
{
    time_t now = time(NULL);
    pending_flow_t *p;
    pending_flow_t *next;
    sqlite3_stmt *stmt = NULL;
    int rc = SQLITE_OK;
    int transaction_started = 0;

    pthread_mutex_lock(&g_db_lock);

    if (!g_db_ready || !g_pending) {
        pthread_mutex_unlock(&g_db_lock);
        return 0;
    }

    rc = db_begin();
    if (rc != SQLITE_OK) {
        if (db_is_fatal_error(rc))
            disable_db_locked(rc);
        pthread_mutex_unlock(&g_db_lock);
        return -1;
    }
    transaction_started = 1;

    rc = sqlite3_prepare_v2(
        g_db,
        "INSERT INTO traffic_minute(ts_minute, device_id, app_id, traffic_kb) "
        "VALUES(?, ?, ?, ?) "
        "ON CONFLICT(ts_minute, device_id, app_id) DO UPDATE SET "
        "traffic_kb = traffic_kb + excluded.traffic_kb;",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        goto FAIL_ALL;

    for (p = g_pending; p; p = p->next) {
        int device_id = get_or_create_device_locked(p->mac, "");
        if (device_id < 0 || ensure_app_locked(p->app_id) != 0) {
            rc = SQLITE_ERROR;
            break;
        }
        sqlite3_bind_int64(stmt, 1, (sqlite3_int64)p->ts_minute);
        sqlite3_bind_int(stmt, 2, device_id);
        sqlite3_bind_int(stmt, 3, p->app_id);
        sqlite3_bind_int64(stmt, 4, (sqlite3_int64)(p->traffic_bytes / 1024ULL));
        rc = sqlite3_step(stmt);
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        if (rc != SQLITE_DONE)
            break;
    }

    if (rc == SQLITE_DONE || rc == SQLITE_OK) {
        rc = db_commit();
        if (rc == SQLITE_OK)
            transaction_started = 0;
    }

FAIL_ALL:
    if (stmt)
        sqlite3_finalize(stmt);
    if (rc != SQLITE_OK) {
        if (transaction_started)
            db_rollback();
        free_caches();
        if (db_is_fatal_error(rc))
            disable_db_locked(rc);
        pthread_mutex_unlock(&g_db_lock);
        return -1;
    }

    free_pending();
    (void)now;
    pthread_mutex_unlock(&g_db_lock);
    return 0;
}

int oaf_history_db_maintenance(time_t now)
{
    time_t current_day = now / 86400;
    time_t minute_cutoff = now - (time_t)OAF_HISTORY_MINUTE_RETENTION_DAYS * 86400;
    time_t day_cutoff = now - (time_t)OAF_HISTORY_DAY_RETENTION_DAYS * 86400;
    int rc = 0;

    if (oaf_history_db_flush_due(now) != 0)
        rc = -1;

    pthread_mutex_lock(&g_db_lock);
    if (!g_db_ready) {
        pthread_mutex_unlock(&g_db_lock);
        return rc;
    }

    if (g_last_maintenance_day == current_day) {
        pthread_mutex_unlock(&g_db_lock);
        return rc;
    }

    /*
     * Use SQLite strftime in UTC for bucket keys. This matches the Unix
     * timestamp bucketing used by the API and avoids DST-dependent day sizes.
     */

    {
        sqlite3_stmt *ins = NULL;
        sqlite3_stmt *del = NULL;
        const char *insert_sql =
            "INSERT INTO traffic_day(day, device_id, app_id, traffic_kb) "
            "SELECT CAST(strftime('%s', datetime(ts_minute, 'unixepoch', 'localtime', 'start of day'), 'utc') AS INTEGER), "
            "device_id, app_id, SUM(traffic_kb) "
            "FROM traffic_minute WHERE ts_minute < ? "
            "GROUP BY 1, device_id, app_id "
            "ON CONFLICT(day, device_id, app_id) DO UPDATE SET "
            "traffic_kb = traffic_kb + excluded.traffic_kb;";
        const char *delete_sql = "DELETE FROM traffic_minute WHERE ts_minute < ?;";

        if (db_begin() != 0) {
            pthread_mutex_unlock(&g_db_lock);
            return -1;
        }

        if (sqlite3_prepare_v2(g_db, insert_sql, -1, &ins, NULL) != SQLITE_OK ||
            sqlite3_prepare_v2(g_db, delete_sql, -1, &del, NULL) != SQLITE_OK) {
            if (ins) sqlite3_finalize(ins);
            if (del) sqlite3_finalize(del);
            db_rollback();
            pthread_mutex_unlock(&g_db_lock);
            return -1;
        }

        sqlite3_bind_int64(ins, 1, (sqlite3_int64)minute_cutoff);
        rc = sqlite3_step(ins);
        sqlite3_finalize(ins);
        ins = NULL;
        if (rc != SQLITE_DONE) {
            sqlite3_finalize(del);
            db_rollback();
            if (db_is_fatal_error(rc))
                disable_db_locked(rc);
            pthread_mutex_unlock(&g_db_lock);
            return -1;
        }

        sqlite3_bind_int64(del, 1, (sqlite3_int64)minute_cutoff);
        rc = sqlite3_step(del);
        sqlite3_finalize(del);
        del = NULL;
        if (rc != SQLITE_DONE) {
            db_rollback();
            if (db_is_fatal_error(rc))
                disable_db_locked(rc);
            pthread_mutex_unlock(&g_db_lock);
            return -1;
        }

        if (db_commit() != 0) {
            db_rollback();
            pthread_mutex_unlock(&g_db_lock);
            return -1;
        }
    }

    if (db_begin() != 0) {
        pthread_mutex_unlock(&g_db_lock);
        return -1;
    }

    {
        sqlite3_stmt *ins = NULL;
        sqlite3_stmt *del = NULL;
        const char *insert_sql =
            "INSERT INTO traffic_month(month, device_id, app_id, traffic_kb) "
            "SELECT CAST(strftime('%s', datetime(day, 'unixepoch', 'localtime', 'start of month'), 'utc') AS INTEGER), "
            "device_id, app_id, SUM(traffic_kb) "
            "FROM traffic_day WHERE day < ? "
            "GROUP BY 1, device_id, app_id "
            "ON CONFLICT(month, device_id, app_id) DO UPDATE SET "
            "traffic_kb = traffic_kb + excluded.traffic_kb;";
        const char *delete_sql = "DELETE FROM traffic_day WHERE day < ?;";

        if (sqlite3_prepare_v2(g_db, insert_sql, -1, &ins, NULL) != SQLITE_OK ||
            sqlite3_prepare_v2(g_db, delete_sql, -1, &del, NULL) != SQLITE_OK) {
            if (ins) sqlite3_finalize(ins);
            if (del) sqlite3_finalize(del);
            db_rollback();
            pthread_mutex_unlock(&g_db_lock);
            return -1;
        }

        sqlite3_bind_int64(ins, 1, (sqlite3_int64)day_cutoff);
        rc = sqlite3_step(ins);
        sqlite3_finalize(ins);
        if (rc != SQLITE_DONE) {
            sqlite3_finalize(del);
            db_rollback();
            if (db_is_fatal_error(rc))
                disable_db_locked(rc);
            pthread_mutex_unlock(&g_db_lock);
            return -1;
        }

        sqlite3_bind_int64(del, 1, (sqlite3_int64)day_cutoff);
        rc = sqlite3_step(del);
        sqlite3_finalize(del);
        if (rc != SQLITE_DONE) {
            db_rollback();
            if (db_is_fatal_error(rc))
                disable_db_locked(rc);
            pthread_mutex_unlock(&g_db_lock);
            return -1;
        }

        if (db_commit() != 0) {
            db_rollback();
            pthread_mutex_unlock(&g_db_lock);
            return -1;
        }
    }

    g_last_maintenance_day = current_day;
    pthread_mutex_unlock(&g_db_lock);
    return rc;
}

static int resolve_device_id_locked(const char *mac)
{
    sqlite3_stmt *stmt = NULL;
    int rc;
    int id = -1;

    if (!mac || !*mac)
        return -1;

    rc = sqlite3_prepare_v2(g_db,
                            "SELECT device_id FROM device WHERE mac=?;",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, mac, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
        id = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return id;
}

static int bind_common_filter(sqlite3_stmt *stmt,
                              int device_id,
                              int app_id,
                              time_t start_time,
                              time_t end_time,
                              int bind_limit,
                              int page,
                              int page_size)
{
    int rc;
    if (device_id >= 0) {
        rc = sqlite3_bind_int(stmt, sqlite3_bind_parameter_index(stmt, ":device_id"), device_id);
        if (rc != SQLITE_OK) return rc;
    }
    if (app_id > 0) {
        rc = sqlite3_bind_int(stmt, sqlite3_bind_parameter_index(stmt, ":app_id"), app_id);
        if (rc != SQLITE_OK) return rc;
    }
    if (start_time > 0) {
        rc = sqlite3_bind_int64(stmt, sqlite3_bind_parameter_index(stmt, ":start_time"), (sqlite3_int64)start_time);
        if (rc != SQLITE_OK) return rc;
    }
    if (end_time > 0) {
        rc = sqlite3_bind_int64(stmt, sqlite3_bind_parameter_index(stmt, ":end_time"), (sqlite3_int64)end_time);
        if (rc != SQLITE_OK) return rc;
    }
    if (bind_limit) {
        rc = sqlite3_bind_int(stmt, sqlite3_bind_parameter_index(stmt, ":limit"), page_size);
        if (rc != SQLITE_OK) return rc;
        rc = sqlite3_bind_int(stmt, sqlite3_bind_parameter_index(stmt, ":offset"), (page - 1) * page_size);
        if (rc != SQLITE_OK) return rc;
    }
    return SQLITE_OK;
}

int oaf_history_db_query_traffic(const char *mac,
                                 int app_id,
                                 time_t start_time,
                                 time_t end_time,
                                 int page,
                                 int page_size,
                                 oaf_history_traffic_row_cb cb,
                                 void *arg,
                                 int64_t *total_num)
{
    sqlite3_stmt *stmt = NULL;
    int rc;
    int device_id = -1;
    int has_device = 0;
    int64_t count = 0;
    char where_min[256] = " WHERE 1=1";
    char where_day[256] = " WHERE 1=1";
    char where_month[256] = " WHERE 1=1";
    char sql_count[3072];
    char sql_query[4096];

    if (total_num)
        *total_num = 0;
    if (page < 1) page = 1;
    if (page_size < 1) page_size = 15;
    if (page_size > 200) page_size = 200;

    pthread_mutex_lock(&g_db_lock);
    if (!g_db_ready) {
        pthread_mutex_unlock(&g_db_lock);
        return -1;
    }

    if (mac && *mac) {
        device_id = resolve_device_id_locked(mac);
        if (device_id < 0) {
            pthread_mutex_unlock(&g_db_lock);
            return 0;
        }
        has_device = 1;
    }

    if (has_device) {
        strncat(where_min, " AND device_id=:device_id", sizeof(where_min)-strlen(where_min)-1);
        strncat(where_day, " AND device_id=:device_id", sizeof(where_day)-strlen(where_day)-1);
        strncat(where_month, " AND device_id=:device_id", sizeof(where_month)-strlen(where_month)-1);
    }
    if (app_id > 0) {
        strncat(where_min, " AND app_id=:app_id", sizeof(where_min)-strlen(where_min)-1);
        strncat(where_day, " AND app_id=:app_id", sizeof(where_day)-strlen(where_day)-1);
        strncat(where_month, " AND app_id=:app_id", sizeof(where_month)-strlen(where_month)-1);
    }
    if (start_time > 0) {
        strncat(where_min, " AND ts_minute>=:start_time", sizeof(where_min)-strlen(where_min)-1);
        strncat(where_day, " AND day>=:start_time", sizeof(where_day)-strlen(where_day)-1);
        strncat(where_month, " AND month>=:start_time", sizeof(where_month)-strlen(where_month)-1);
    }
    if (end_time > 0) {
        strncat(where_min, " AND ts_minute<:end_time", sizeof(where_min)-strlen(where_min)-1);
        strncat(where_day, " AND day<:end_time", sizeof(where_day)-strlen(where_day)-1);
        strncat(where_month, " AND month<:end_time", sizeof(where_month)-strlen(where_month)-1);
    }

    snprintf(sql_count, sizeof(sql_count),
             "SELECT COUNT(*) FROM ("
             "SELECT ts_minute AS ts FROM traffic_minute%s "
             "UNION ALL SELECT day AS ts FROM traffic_day%s "
             "UNION ALL SELECT month AS ts FROM traffic_month%s"
             ");",
             where_min, where_day, where_month);

    rc = sqlite3_prepare_v2(g_db, sql_count, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        goto QUERY_FAIL;
    rc = bind_common_filter(stmt, has_device ? device_id : -1, app_id, start_time, end_time, 0, page, page_size);
    if (rc != SQLITE_OK || sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        goto QUERY_FAIL;
    }
    count = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    stmt = NULL;

    if (total_num)
        *total_num = count;

    if (!cb || count == 0 || (int64_t)(page - 1) * page_size >= count) {
        pthread_mutex_unlock(&g_db_lock);
        return 0;
    }

    snprintf(sql_query, sizeof(sql_query),
             "SELECT h.ts,h.device_id,h.app_id,h.traffic_kb,h.granularity,"
             "d.mac,a.app_name "
             "FROM ("
             "SELECT ts_minute AS ts,device_id,app_id,traffic_kb,0 AS granularity FROM traffic_minute%s "
             "UNION ALL SELECT day AS ts,device_id,app_id,traffic_kb,1 AS granularity FROM traffic_day%s "
             "UNION ALL SELECT month AS ts,device_id,app_id,traffic_kb,2 AS granularity FROM traffic_month%s"
             ") h "
             "JOIN device d ON d.device_id=h.device_id "
             "JOIN app a ON a.app_id=h.app_id "
             "ORDER BY h.ts ASC, h.device_id ASC, h.app_id ASC "
             "LIMIT :limit OFFSET :offset;",
             where_min, where_day, where_month);

    rc = sqlite3_prepare_v2(g_db, sql_query, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        goto QUERY_FAIL;
    rc = bind_common_filter(stmt, has_device ? device_id : -1, app_id, start_time, end_time, 1, page, page_size);
    if (rc != SQLITE_OK)
        goto QUERY_FAIL;

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        oaf_history_traffic_row_t row;
        memset(&row, 0, sizeof(row));
        row.timestamp = sqlite3_column_int64(stmt, 0);
        row.device_id = sqlite3_column_int(stmt, 1);
        row.app_id = sqlite3_column_int(stmt, 2);
        row.traffic_kb = sqlite3_column_int64(stmt, 3);
        row.granularity = sqlite3_column_int(stmt, 4);
        snprintf(row.mac, sizeof(row.mac), "%s",
                 sqlite3_column_text(stmt, 5) ? (const char *)sqlite3_column_text(stmt, 5) : "");
        snprintf(row.app_name, sizeof(row.app_name), "%s",
                 sqlite3_column_text(stmt, 6) ? (const char *)sqlite3_column_text(stmt, 6) : "");
        if (cb(&row, arg) != 0)
            break;
    }

    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_lock);
    return (rc == SQLITE_DONE || rc == SQLITE_ROW) ? 0 : -1;

QUERY_FAIL:
    if (stmt)
        sqlite3_finalize(stmt);
    if (rc != SQLITE_OK && db_is_fatal_error(rc))
        disable_db_locked(rc);
    pthread_mutex_unlock(&g_db_lock);
    return -1;
}

int oaf_history_db_sum_traffic(const char *mac,
                               int app_id,
                               time_t start_time,
                               time_t end_time,
                               int64_t *total_kb)
{
    int64_t total = 0;
    int64_t part = 0;
    sqlite3_stmt *stmt = NULL;
    int rc;
    const char *tables[] = {"traffic_minute", "traffic_day", "traffic_month"};
    const char *cols[] = {"ts_minute", "day", "month"};
    int i;
    int device_id = -1;

    if (!total_kb)
        return -1;
    *total_kb = 0;

    pthread_mutex_lock(&g_db_lock);
    if (!g_db_ready) {
        pthread_mutex_unlock(&g_db_lock);
        return -1;
    }

    if (mac && *mac) {
        device_id = resolve_device_id_locked(mac);
        if (device_id < 0) {
            pthread_mutex_unlock(&g_db_lock);
            return 0;
        }
    }

    for (i = 0; i < 3; i++) {
        char sql[512];
        char *p;
        int first = 1;

        snprintf(sql, sizeof(sql), "SELECT COALESCE(SUM(traffic_kb),0) FROM %s WHERE ", tables[i]);
        p = sql + strlen(sql);
        if (device_id >= 0) {
            snprintf(p, sizeof(sql) - (size_t)(p-sql), "device_id=%d", device_id);
            p = sql + strlen(sql);
            first = 0;
        }
        if (app_id > 0) {
            snprintf(p, sizeof(sql) - (size_t)(p-sql), "%sapp_id=%d", first ? "" : " AND ", app_id);
            p = sql + strlen(sql);
            first = 0;
        }
        if (start_time > 0) {
            snprintf(p, sizeof(sql) - (size_t)(p-sql), "%s%s>=%lld",
                     first ? "" : " AND ", cols[i], (long long)start_time);
            p = sql + strlen(sql);
            first = 0;
        }
        if (end_time > 0) {
            snprintf(p, sizeof(sql) - (size_t)(p-sql), "%s%s<%lld",
                     first ? "" : " AND ", cols[i], (long long)end_time);
        }

        rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
        if (rc != SQLITE_OK)
            goto SUM_FAIL;
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW)
            part = sqlite3_column_int64(stmt, 0);
        else
            part = 0;
        sqlite3_finalize(stmt);
        stmt = NULL;
        total += part;
    }

    *total_kb = total;
    pthread_mutex_unlock(&g_db_lock);
    return 0;

SUM_FAIL:
    if (stmt)
        sqlite3_finalize(stmt);
    if (db_is_fatal_error(rc))
        disable_db_locked(rc);
    pthread_mutex_unlock(&g_db_lock);
    return -1;
}

int oaf_history_db_replace_visit_day(const char *mac,
                                     time_t day_start,
                                     const oaf_history_visit_record_t *records,
                                     size_t record_count)
{
    sqlite3_stmt *del = NULL;
    sqlite3_stmt *ins = NULL;
    int rc;
    int device_id;

    if (!mac || !*mac)
        return -1;

    pthread_mutex_lock(&g_db_lock);
    if (!g_db_ready) {
        pthread_mutex_unlock(&g_db_lock);
        return -1;
    }

    if (db_begin() != 0) {
        pthread_mutex_unlock(&g_db_lock);
        return -1;
    }

    device_id = get_or_create_device_locked(mac, "");
    if (device_id < 0) {
        db_rollback();
        pthread_mutex_unlock(&g_db_lock);
        return -1;
    }

    rc = sqlite3_prepare_v2(
        g_db,
        "DELETE FROM visit_history WHERE device_id=? AND start_time>=? AND start_time<?;",
        -1, &del, NULL);
    if (rc != SQLITE_OK)
        goto VISIT_FAIL;
    sqlite3_bind_int(del, 1, device_id);
    sqlite3_bind_int64(del, 2, (sqlite3_int64)day_start);
    sqlite3_bind_int64(del, 3, (sqlite3_int64)(day_start + 86400));
    rc = sqlite3_step(del);
    sqlite3_finalize(del);
    del = NULL;
    if (rc != SQLITE_DONE)
        goto VISIT_FAIL;

    if (record_count > 0) {
        rc = sqlite3_prepare_v2(
            g_db,
            "INSERT INTO visit_history(device_id,app_id,start_time,end_time,duration,action) "
            "VALUES(?,?,?,?,?,?);",
            -1, &ins, NULL);
        if (rc != SQLITE_OK)
            goto VISIT_FAIL;

        for (size_t i = 0; i < record_count; i++) {
            if (records[i].app_id <= 0)
                continue;
            if (ensure_app_locked(records[i].app_id) != 0) {
                rc = SQLITE_ERROR;
                goto VISIT_FAIL;
            }
            sqlite3_bind_int(ins, 1, device_id);
            sqlite3_bind_int(ins, 2, records[i].app_id);
            sqlite3_bind_int64(ins, 3, (sqlite3_int64)records[i].start_time);
            sqlite3_bind_int64(ins, 4, (sqlite3_int64)records[i].end_time);
            sqlite3_bind_int(ins, 5, records[i].duration > 0 ? records[i].duration : 1);
            sqlite3_bind_int(ins, 6, records[i].action);
            rc = sqlite3_step(ins);
            sqlite3_reset(ins);
            sqlite3_clear_bindings(ins);
            if (rc != SQLITE_DONE)
                goto VISIT_FAIL;
        }
        sqlite3_finalize(ins);
        ins = NULL;
    }

    if (db_commit() != 0) {
        db_rollback();
        pthread_mutex_unlock(&g_db_lock);
        return -1;
    }

    pthread_mutex_unlock(&g_db_lock);
    return 0;

VISIT_FAIL:
    if (del) sqlite3_finalize(del);
    if (ins) sqlite3_finalize(ins);
    db_rollback();
    if (db_is_fatal_error(rc))
        disable_db_locked(rc);
    pthread_mutex_unlock(&g_db_lock);
    return -1;
}

int oaf_history_db_query_visits(const char *mac,
                                int app_id,
                                time_t start_time,
                                time_t end_time,
                                int page,
                                int page_size,
                                oaf_history_visit_row_cb cb,
                                void *arg,
                                int64_t *total_num)
{
    sqlite3_stmt *stmt = NULL;
    int rc;
    int device_id = -1;
    int64_t count = 0;
    char where_sql[768] = " WHERE 1=1";
    char count_sql[1024];
    char query_sql[1200];

    if (total_num)
        *total_num = 0;
    if (page < 1) page = 1;
    if (page_size < 1) page_size = 15;
    if (page_size > 200) page_size = 200;

    pthread_mutex_lock(&g_db_lock);
    if (!g_db_ready) {
        pthread_mutex_unlock(&g_db_lock);
        return -1;
    }

    if (mac && *mac) {
        device_id = resolve_device_id_locked(mac);
        if (device_id < 0) {
            pthread_mutex_unlock(&g_db_lock);
            return 0;
        }
        strncat(where_sql, " AND device_id=:device_id", sizeof(where_sql)-strlen(where_sql)-1);
    }
    if (app_id > 0)
        strncat(where_sql, " AND app_id=:app_id", sizeof(where_sql)-strlen(where_sql)-1);
    if (start_time > 0)
        strncat(where_sql, " AND end_time>=:start_time", sizeof(where_sql)-strlen(where_sql)-1);
    if (end_time > 0)
        strncat(where_sql, " AND start_time<:end_time", sizeof(where_sql)-strlen(where_sql)-1);

    snprintf(count_sql, sizeof(count_sql),
             "SELECT COUNT(*) FROM visit_history%s;", where_sql);
    rc = sqlite3_prepare_v2(g_db, count_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        goto VQUERY_FAIL;
    if (device_id >= 0)
        sqlite3_bind_int(stmt, sqlite3_bind_parameter_index(stmt, ":device_id"), device_id);
    if (app_id > 0)
        sqlite3_bind_int(stmt, sqlite3_bind_parameter_index(stmt, ":app_id"), app_id);
    if (start_time > 0)
        sqlite3_bind_int64(stmt, sqlite3_bind_parameter_index(stmt, ":start_time"), (sqlite3_int64)start_time);
    if (end_time > 0)
        sqlite3_bind_int64(stmt, sqlite3_bind_parameter_index(stmt, ":end_time"), (sqlite3_int64)end_time);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW)
        goto VQUERY_FAIL;
    count = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    stmt = NULL;

    if (total_num)
        *total_num = count;

    if (!cb || count == 0) {
        pthread_mutex_unlock(&g_db_lock);
        return 0;
    }

    snprintf(query_sql, sizeof(query_sql),
             "SELECT visit_id,device_id,app_id,start_time,end_time,duration,action "
             "FROM visit_history%s "
             "ORDER BY end_time DESC, duration ASC LIMIT :limit OFFSET :offset;",
             where_sql);
    rc = sqlite3_prepare_v2(g_db, query_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        goto VQUERY_FAIL;

    if (device_id >= 0)
        sqlite3_bind_int(stmt, sqlite3_bind_parameter_index(stmt, ":device_id"), device_id);
    if (app_id > 0)
        sqlite3_bind_int(stmt, sqlite3_bind_parameter_index(stmt, ":app_id"), app_id);
    if (start_time > 0)
        sqlite3_bind_int64(stmt, sqlite3_bind_parameter_index(stmt, ":start_time"), (sqlite3_int64)start_time);
    if (end_time > 0)
        sqlite3_bind_int64(stmt, sqlite3_bind_parameter_index(stmt, ":end_time"), (sqlite3_int64)end_time);
    sqlite3_bind_int(stmt, sqlite3_bind_parameter_index(stmt, ":limit"), page_size);
    sqlite3_bind_int(stmt, sqlite3_bind_parameter_index(stmt, ":offset"), (page - 1) * page_size);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        oaf_history_visit_row_t row;
        const char *row_mac;
        memset(&row, 0, sizeof(row));
        row.visit_id = sqlite3_column_int64(stmt, 0);
        row.device_id = sqlite3_column_int(stmt, 1);
        row.app_id = sqlite3_column_int(stmt, 2);
        row.start_time = sqlite3_column_int64(stmt, 3);
        row.end_time = sqlite3_column_int64(stmt, 4);
        row.duration = sqlite3_column_int(stmt, 5);
        row.action = sqlite3_column_int(stmt, 6);

        row_mac = NULL;
        {
            sqlite3_stmt *mac_stmt = NULL;
            if (sqlite3_prepare_v2(g_db,
                                   "SELECT mac FROM device WHERE device_id=?;",
                                   -1, &mac_stmt, NULL) == SQLITE_OK) {
                sqlite3_bind_int(mac_stmt, 1, row.device_id);
                if (sqlite3_step(mac_stmt) == SQLITE_ROW)
                    row_mac = (const char *)sqlite3_column_text(mac_stmt, 0);
                if (row_mac) {
                    snprintf(row.mac, sizeof(row.mac), "%s", row_mac);
                }
                sqlite3_finalize(mac_stmt);
            }
        }

        if (cb(&row, arg) != 0)
            break;
    }

    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_lock);
    return (rc == SQLITE_DONE || rc == SQLITE_ROW) ? 0 : -1;

VQUERY_FAIL:
    if (stmt) sqlite3_finalize(stmt);
    if (db_is_fatal_error(rc))
        disable_db_locked(rc);
    pthread_mutex_unlock(&g_db_lock);
    return -1;
}

static int legacy_migration_done_locked(void)
{
    sqlite3_stmt *stmt = NULL;
    int rc;
    const char *value = NULL;

    rc = sqlite3_prepare_v2(
        g_db,
        "SELECT meta_value FROM history_meta WHERE meta_key='legacy_visit_db_migrated';",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return 0;
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
        value = (const char *)sqlite3_column_text(stmt, 0);
    rc = (value && strcmp(value, "1") == 0) ? 1 : 0;
    sqlite3_finalize(stmt);
    return rc;
}

int oaf_history_db_migrate_legacy_visit_db(const char *legacy_db_path)
{
    sqlite3 *legacy = NULL;
    sqlite3_stmt *legacy_stmt = NULL;
    sqlite3_stmt *ins = NULL;
    int rc;
    int migrated = 0;
    int started = 0;

    if (!legacy_db_path || !*legacy_db_path)
        return 0;

    pthread_mutex_lock(&g_db_lock);
    if (!g_db_ready) {
        pthread_mutex_unlock(&g_db_lock);
        return -1;
    }

    if (legacy_migration_done_locked()) {
        pthread_mutex_unlock(&g_db_lock);
        return 0;
    }

    if (access(legacy_db_path, R_OK) != 0) {
        pthread_mutex_unlock(&g_db_lock);
        return 0;
    }

    rc = sqlite3_open_v2(legacy_db_path, &legacy, SQLITE_OPEN_READONLY, NULL);
    if (rc != SQLITE_OK) {
        if (legacy) sqlite3_close(legacy);
        pthread_mutex_unlock(&g_db_lock);
        return -1;
    }

    rc = sqlite3_prepare_v2(
        legacy,
        "SELECT mac,appid,start_time,end_time,duration,action "
        "FROM app_visit_record ORDER BY rowid;",
        -1, &legacy_stmt, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_close(legacy);
        pthread_mutex_unlock(&g_db_lock);
        return 0; /* Legacy DB may exist without the table. */
    }

    if (db_begin() != 0) {
        sqlite3_finalize(legacy_stmt);
        sqlite3_close(legacy);
        pthread_mutex_unlock(&g_db_lock);
        return -1;
    }
    started = 1;

    rc = sqlite3_prepare_v2(
        g_db,
        "INSERT INTO visit_history(device_id,app_id,start_time,end_time,duration,action) "
        "VALUES(?,?,?,?,?,?);",
        -1, &ins, NULL);
    if (rc != SQLITE_OK)
        goto MIGRATE_FAIL;

    while ((rc = sqlite3_step(legacy_stmt)) == SQLITE_ROW) {
        const char *mac = (const char *)sqlite3_column_text(legacy_stmt, 0);
        int app_id = sqlite3_column_int(legacy_stmt, 1);
        int64_t start_time = sqlite3_column_int64(legacy_stmt, 2);
        int64_t end_time = sqlite3_column_int64(legacy_stmt, 3);
        int duration = sqlite3_column_int(legacy_stmt, 4);
        int action = sqlite3_column_int(legacy_stmt, 5);
        int device_id;

        if (!mac || !*mac || app_id <= 0)
            continue;

        device_id = get_or_create_device_locked(mac, "");
        if (device_id < 0 || ensure_app_locked(app_id) != 0) {
            rc = SQLITE_ERROR;
            goto MIGRATE_FAIL;
        }

        sqlite3_bind_int(ins, 1, device_id);
        sqlite3_bind_int(ins, 2, app_id);
        sqlite3_bind_int64(ins, 3, start_time);
        sqlite3_bind_int64(ins, 4, end_time);
        sqlite3_bind_int(ins, 5, duration > 0 ? duration : 1);
        sqlite3_bind_int(ins, 6, action);
        rc = sqlite3_step(ins);
        sqlite3_reset(ins);
        sqlite3_clear_bindings(ins);
        if (rc != SQLITE_DONE)
            goto MIGRATE_FAIL;

        migrated++;
    }

    if (rc != SQLITE_DONE)
        goto MIGRATE_FAIL;

    sqlite3_finalize(ins);
    ins = NULL;
    sqlite3_finalize(legacy_stmt);
    legacy_stmt = NULL;
    sqlite3_close(legacy);
    legacy = NULL;

    if (db_exec(
            "INSERT INTO history_meta(meta_key,meta_value) "
            "VALUES('legacy_visit_db_migrated','1') "
            "ON CONFLICT(meta_key) DO UPDATE SET meta_value='1';") != 0)
        goto MIGRATE_FAIL;

    if (db_commit() != 0)
        goto MIGRATE_FAIL;
    started = 0;

    pthread_mutex_unlock(&g_db_lock);
    LOG_INFO("history db: migrated %d legacy visit records from %s; source kept intact\n",
             migrated, legacy_db_path);
    return 0;

MIGRATE_FAIL:
    if (ins) sqlite3_finalize(ins);
    if (legacy_stmt) sqlite3_finalize(legacy_stmt);
    if (legacy) sqlite3_close(legacy);
    if (started)
        db_rollback();
    if (db_is_fatal_error(rc))
        disable_db_locked(rc);
    pthread_mutex_unlock(&g_db_lock);
    return -1;
}
