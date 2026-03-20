/*
 * token_savings.c -- Token savings calculation and persistence.
 */

#include "token_savings.h"
#include "error.h"
#include "platform.h"
#include "hashmap.h"
#include "str_util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- DDL for savings tables ---- */

static const char *DDL_SAVINGS =
    "DROP TABLE IF EXISTS savings_log;"
    "CREATE TABLE IF NOT EXISTS savings_totals ("
    "    id INTEGER PRIMARY KEY CHECK (id = 1),"
    "    total_calls INTEGER NOT NULL DEFAULT 0,"
    "    total_raw_bytes INTEGER NOT NULL DEFAULT 0,"
    "    total_response_bytes INTEGER NOT NULL DEFAULT 0,"
    "    total_tokens_saved INTEGER NOT NULL DEFAULT 0,"
    "    last_updated TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))"
    ");"
    "CREATE TABLE IF NOT EXISTS savings_per_tool ("
    "    tool_name TEXT PRIMARY KEY,"
    "    call_count INTEGER NOT NULL DEFAULT 0,"
    "    raw_bytes INTEGER NOT NULL DEFAULT 0,"
    "    response_bytes INTEGER NOT NULL DEFAULT 0,"
    "    tokens_saved INTEGER NOT NULL DEFAULT 0,"
    "    last_updated TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))"
    ");";

/* ---- Core functions ---- */

tt_savings_record_t tt_savings_calculate(const char *tool_name,
                                          int64_t raw_bytes,
                                          int64_t response_bytes)
{
    tt_savings_record_t rec;
    rec.tool_name = tool_name;
    rec.raw_bytes = raw_bytes;
    rec.response_bytes = response_bytes;

    int64_t delta = raw_bytes - response_bytes;
    rec.tokens_saved = delta > 0 ? delta / 4 : 0;

    return rec;
}

int tt_savings_ensure_tables(sqlite3 *db)
{
    char *errmsg = NULL;
    int rc = sqlite3_exec(db, DDL_SAVINGS, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        tt_error_set("savings: failed to create tables: %s",
                     errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
        return -1;
    }
    return 0;
}

int tt_savings_record(tt_database_t *db, const tt_savings_record_t *record)
{
    if (!db || !db->db || !record) return -1;

    if (tt_savings_ensure_tables(db->db) < 0) return -1;

    /* Upsert totals */
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->db,
        "INSERT INTO savings_totals (id, total_calls, total_raw_bytes, "
        "total_response_bytes, total_tokens_saved) "
        "VALUES (1, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET "
        "total_calls = total_calls + excluded.total_calls, "
        "total_raw_bytes = total_raw_bytes + excluded.total_raw_bytes, "
        "total_response_bytes = total_response_bytes + excluded.total_response_bytes, "
        "total_tokens_saved = total_tokens_saved + excluded.total_tokens_saved, "
        "last_updated = strftime('%Y-%m-%dT%H:%M:%SZ', 'now')",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_int64(stmt, 1, 1);  /* 1 call */
    sqlite3_bind_int64(stmt, 2, record->raw_bytes);
    sqlite3_bind_int64(stmt, 3, record->response_bytes);
    sqlite3_bind_int64(stmt, 4, record->tokens_saved);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;

    /* Upsert per-tool breakdown */
    if (record->tool_name && record->tool_name[0]) {
        stmt = NULL;
        rc = sqlite3_prepare_v2(db->db,
            "INSERT INTO savings_per_tool (tool_name, call_count, raw_bytes, "
            "response_bytes, tokens_saved) "
            "VALUES (?, 1, ?, ?, ?) "
            "ON CONFLICT(tool_name) DO UPDATE SET "
            "call_count = call_count + 1, "
            "raw_bytes = raw_bytes + excluded.raw_bytes, "
            "response_bytes = response_bytes + excluded.response_bytes, "
            "tokens_saved = tokens_saved + excluded.tokens_saved, "
            "last_updated = strftime('%Y-%m-%dT%H:%M:%SZ', 'now')",
            -1, &stmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, record->tool_name, -1, SQLITE_STATIC);
            sqlite3_bind_int64(stmt, 2, record->raw_bytes);
            sqlite3_bind_int64(stmt, 3, record->response_bytes);
            sqlite3_bind_int64(stmt, 4, record->tokens_saved);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    return 0;
}

int tt_savings_get_totals(tt_database_t *db, tt_savings_totals_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!db || !db->db) return -1;

    if (tt_savings_ensure_tables(db->db) < 0) return -1;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->db,
        "SELECT total_calls, total_raw_bytes, total_response_bytes, "
        "total_tokens_saved FROM savings_totals WHERE id = 1",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out->total_calls = sqlite3_column_int64(stmt, 0);
        out->total_raw_bytes = sqlite3_column_int64(stmt, 1);
        out->total_response_bytes = sqlite3_column_int64(stmt, 2);
        out->total_tokens_saved = sqlite3_column_int64(stmt, 3);
    }
    sqlite3_finalize(stmt);
    return 0;
}

int tt_savings_reset(tt_database_t *db)
{
    if (!db || !db->db) return -1;

    if (tt_savings_ensure_tables(db->db) < 0) return -1;

    char *errmsg = NULL;
    int rc = sqlite3_exec(db->db,
        "DELETE FROM savings_totals; DELETE FROM savings_per_tool;",
        NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        tt_error_set("savings: reset failed: %s", errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
        return -1;
    }
    return 0;
}

/* ---- Per-tool breakdown ---- */

int tt_savings_get_per_tool(tt_database_t *db, tt_savings_per_tool_t **out,
                            int *out_count)
{
    *out = NULL;
    *out_count = 0;
    if (!db || !db->db) return -1;

    if (tt_savings_ensure_tables(db->db) < 0) return -1;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->db,
        "SELECT tool_name, call_count, raw_bytes, response_bytes, tokens_saved "
        "FROM savings_per_tool ORDER BY tokens_saved DESC",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    tt_savings_per_tool_t *items = NULL;
    int count = 0, cap = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            cap = cap ? cap * 2 : 8;
            tt_savings_per_tool_t *tmp = realloc(items,
                (size_t)cap * sizeof(tt_savings_per_tool_t));
            if (!tmp) {
                tt_savings_free_per_tool(items, count);
                sqlite3_finalize(stmt);
                return -1;
            }
            items = tmp;
        }
        items[count].tool_name = tt_strdup(
            (const char *)sqlite3_column_text(stmt, 0));
        items[count].call_count = sqlite3_column_int64(stmt, 1);
        items[count].raw_bytes = sqlite3_column_int64(stmt, 2);
        items[count].response_bytes = sqlite3_column_int64(stmt, 3);
        items[count].tokens_saved = sqlite3_column_int64(stmt, 4);
        count++;
    }
    sqlite3_finalize(stmt);

    *out = items;
    *out_count = count;
    return 0;
}

void tt_savings_free_per_tool(tt_savings_per_tool_t *items, int count)
{
    if (!items) return;
    for (int i = 0; i < count; i++)
        free(items[i].tool_name);
    free(items);
}

/* ---- raw_bytes helpers ---- */

int64_t tt_savings_raw_from_file_sizes(const char *project_root,
                                        const char **file_paths,
                                        int file_count)
{
    if (!project_root || !file_paths || file_count <= 0) return 0;

    /* Deduplicate file paths */
    tt_hashmap_t *seen = tt_hashmap_new(file_count > 4 ? (size_t)file_count * 2 : 8);
    int64_t total = 0;

    for (int i = 0; i < file_count; i++) {
        if (!file_paths[i] || !file_paths[i][0]) continue;
        if (tt_hashmap_has(seen, file_paths[i])) continue;
        tt_hashmap_set(seen, file_paths[i], (void *)1);

        char *full = tt_path_join(project_root, file_paths[i]);
        if (full) {
            int64_t sz = tt_file_size(full);
            if (sz > 0) total += sz;
            free(full);
        }
    }

    tt_hashmap_free(seen);
    return total;
}

int64_t tt_savings_raw_from_file(const char *full_path)
{
    if (!full_path) return 0;
    int64_t sz = tt_file_size(full_path);
    return sz > 0 ? sz : 0;
}

int64_t tt_savings_raw_from_index(tt_database_t *db)
{
    if (!db || !db->db) return 0;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->db,
        "SELECT COALESCE(SUM(size_bytes), 0) FROM files",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return 0;

    int64_t total = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        total = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return total;
}

/* ---- Integration helper ---- */

void tt_savings_track(tt_database_t *db, const char *tool_name,
                       int64_t raw_bytes, cJSON *result)
{
    if (!db || !db->db || raw_bytes <= 0 || !result) return;

    /* Calculate response_bytes from the serialized result */
    char *json_str = cJSON_PrintUnformatted(result);
    if (!json_str) return;
    int64_t response_bytes = (int64_t)strlen(json_str);
    free(json_str);

    /* Calculate savings */
    tt_savings_record_t rec = tt_savings_calculate(tool_name, raw_bytes, response_bytes);

    if (rec.tokens_saved <= 0) return;

    /* Record in DB (silently ignore errors) */
    tt_savings_record(db, &rec);
}
