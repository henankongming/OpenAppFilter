// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright(c) 2026 OpenAppFilter contributors
 */
#ifndef __FWX_HISTORY_DB_H__
#define __FWX_HISTORY_DB_H__

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define OAF_HISTORY_DB_DIR "/etc/openappfilter"
#define OAF_HISTORY_DB_PATH OAF_HISTORY_DB_DIR "/oaf.db"
#define OAF_HISTORY_MINUTE_RETENTION_DAYS 31
#define OAF_HISTORY_DAY_RETENTION_DAYS 365
#define OAF_HISTORY_PENDING_MAX 8192

enum oaf_history_granularity {
    OAF_HISTORY_GRANULARITY_MINUTE = 0,
    OAF_HISTORY_GRANULARITY_DAY = 1,
    OAF_HISTORY_GRANULARITY_MONTH = 2
};

typedef struct oaf_history_traffic_row {
    int64_t timestamp;
    int device_id;
    int app_id;
    int64_t traffic_kb;
    char mac[32];
    char app_name[128];
    int granularity;
} oaf_history_traffic_row_t;

typedef int (*oaf_history_traffic_row_cb)(const oaf_history_traffic_row_t *row, void *arg);

typedef struct oaf_history_visit_record {
    int app_id;
    uint32_t start_time;
    uint32_t end_time;
    int duration;
    int action;
} oaf_history_visit_record_t;

typedef struct oaf_history_visit_row {
    int64_t visit_id;
    int device_id;
    int app_id;
    int64_t start_time;
    int64_t end_time;
    int duration;
    int action;
    char mac[32];
} oaf_history_visit_row_t;

typedef int (*oaf_history_visit_row_cb)(const oaf_history_visit_row_t *row, void *arg);

int oaf_history_db_init(void);
void oaf_history_db_close(void);
int oaf_history_db_is_ready(void);

/* Traffic history: enqueue one already-aggregated flow sample in memory. */
int oaf_history_db_record_minute(const char *mac,
                                 int app_id,
                                 time_t timestamp,
                                 uint64_t traffic_kb);

/* Flush completed minute buckets in one SQLite transaction. */
int oaf_history_db_flush_due(time_t now);

/* Flush all pending buckets, including the current minute, for clean shutdown/tests. */
int oaf_history_db_flush_all(void);

/* Run daily retention/rollup. Safe to call repeatedly. */
int oaf_history_db_maintenance(time_t now);

/* Generic cross-granularity traffic query. mac and app_id are optional filters. */
int oaf_history_db_query_traffic(const char *mac,
                                 int app_id,
                                 time_t start_time,
                                 time_t end_time,
                                 int page,
                                 int page_size,
                                 oaf_history_traffic_row_cb cb,
                                 void *arg,
                                 int64_t *total_num);

int oaf_history_db_sum_traffic(const char *mac,
                               int app_id,
                               time_t start_time,
                               time_t end_time,
                               int64_t *total_kb);

/* Visit history is kept separately from traffic history but in the same DB. */
int oaf_history_db_replace_visit_day(const char *mac,
                                     time_t day_start,
                                     const oaf_history_visit_record_t *records,
                                     size_t record_count);

int oaf_history_db_query_visits(const char *mac,
                                int app_id,
                                time_t start_time,
                                time_t end_time,
                                int page,
                                int page_size,
                                oaf_history_visit_row_cb cb,
                                void *arg,
                                int64_t *total_num);

/* Migrate the existing client.db app_visit_record table once, without deleting it. */
int oaf_history_db_migrate_legacy_visit_db(const char *legacy_db_path);

#endif
