/*
 * token_savings.h -- Token savings calculation and persistence.
 *
 * Tracks how many tokens TokToken saves by providing structured results
 * instead of raw file content. Savings are persisted per-project in the
 * same SQLite database as the index.
 *
 * Formula: tokens_saved = max(0, (raw_bytes - response_bytes) / 4)
 *   - raw_bytes: total size of files the client would have read without TokToken
 *   - response_bytes: size of the JSON response produced
 *   - 4: industry-standard bytes-per-token approximation
 */

#ifndef TT_TOKEN_SAVINGS_H
#define TT_TOKEN_SAVINGS_H

#include "database.h"

#include <cJSON.h>
#include <stdbool.h>
#include <stdint.h>

/* ---- Record of a single savings calculation ---- */

typedef struct {
    const char *tool_name;      /* [borrows] name of the tool */
    int64_t raw_bytes;          /* bytes the client would have read */
    int64_t response_bytes;     /* bytes of the TokToken response */
    int64_t tokens_saved;       /* (raw - response) / 4, minimum 0 */
} tt_savings_record_t;

/* ---- Cumulative totals for a project ---- */

typedef struct {
    int64_t total_calls;
    int64_t total_raw_bytes;
    int64_t total_response_bytes;
    int64_t total_tokens_saved;
} tt_savings_totals_t;

/* ---- Per-tool breakdown entry ---- */

typedef struct {
    char *tool_name;            /* [owns] tool name */
    int64_t call_count;
    int64_t raw_bytes;
    int64_t response_bytes;
    int64_t tokens_saved;
} tt_savings_per_tool_t;

/* ---- Core functions ---- */

/*
 * tt_savings_calculate -- Calculate token savings for a single call.
 *
 * Pure calculation, no DB access. tokens_saved is clamped to >= 0.
 */
tt_savings_record_t tt_savings_calculate(const char *tool_name,
                                          int64_t raw_bytes,
                                          int64_t response_bytes);

/*
 * tt_savings_ensure_tables -- Create savings tables if they don't exist.
 *
 * Called lazily before first record. Safe to call multiple times.
 * Returns 0 on success, -1 on error.
 */
int tt_savings_ensure_tables(sqlite3 *db);

/*
 * tt_savings_record -- Persist a savings record in the project database.
 *
 * Updates savings_totals. Returns 0 on success, -1 on error.
 */
int tt_savings_record(tt_database_t *db, const tt_savings_record_t *record);

/*
 * tt_savings_get_totals -- Load cumulative totals from the database.
 *
 * If no totals exist, out is zeroed. Returns 0 on success, -1 on error.
 */
int tt_savings_get_totals(tt_database_t *db, tt_savings_totals_t *out);

/*
 * tt_savings_reset -- Clear all savings data for the current project.
 *
 * Clears both savings_totals and savings_per_tool.
 * Returns 0 on success, -1 on error.
 */
int tt_savings_reset(tt_database_t *db);

/*
 * tt_savings_get_per_tool -- Load per-tool breakdown from the database.
 *
 * Returns an array of per-tool entries sorted by tokens_saved DESC.
 * Caller must free via tt_savings_free_per_tool().
 * Returns 0 on success, -1 on error.
 */
int tt_savings_get_per_tool(tt_database_t *db, tt_savings_per_tool_t **out,
                            int *out_count);

/*
 * tt_savings_free_per_tool -- Free array returned by tt_savings_get_per_tool.
 */
void tt_savings_free_per_tool(tt_savings_per_tool_t *items, int count);

/* ---- raw_bytes helpers ---- */

/*
 * tt_savings_raw_from_file_sizes -- Sum file sizes for unique paths.
 *
 * project_root is prepended to each relative path.
 * Duplicate paths are counted only once.
 */
int64_t tt_savings_raw_from_file_sizes(const char *project_root,
                                        const char **file_paths,
                                        int file_count);

/*
 * tt_savings_raw_from_file -- Size of a single file (absolute path).
 */
int64_t tt_savings_raw_from_file(const char *full_path);

/*
 * tt_savings_raw_from_index -- Sum of all indexed file sizes from DB.
 *
 * Returns 0 if query fails.
 */
int64_t tt_savings_raw_from_index(tt_database_t *db);

/* ---- Integration helper ---- */

/*
 * tt_savings_track -- Calculate and record savings in the database.
 *
 * Call this in _exec functions before closing the DB:
 *   1. Serializes result to measure response_bytes
 *   2. Calculates savings
 *   3. Records in DB (creates tables if needed)
 *
 * If db is NULL or raw_bytes is 0, does nothing.
 */
void tt_savings_track(tt_database_t *db, const char *tool_name,
                       int64_t raw_bytes, cJSON *result);

#endif /* TT_TOKEN_SAVINGS_H */
