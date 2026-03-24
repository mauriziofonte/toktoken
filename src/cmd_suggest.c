/*
 * cmd_suggest.c -- suggest command: onboarding discovery tool.
 *
 * Aggregates existing index data to produce actionable starting points
 * for exploring an unfamiliar codebase. All queries hit existing tables;
 * no new schema is required.
 */

#include "cmd_suggest.h"
#include "json_output.h"
#include "error.h"
#include "platform.h"
#include "database.h"
#include "index_store.h"
#include "token_savings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>

static cJSON *make_error(const char *code, const char *message, const char *hint)
{
    cJSON *err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "error", code);
    cJSON_AddStringToObject(err, "message", message);
    if (hint)
        cJSON_AddStringToObject(err, "hint", hint);
    return err;
}

/*
 * Top keywords: most frequent symbol name tokens from the FTS5 index.
 * Splits symbol names into tokens (camelCase, snake_case) via SQL.
 */
static cJSON *query_top_keywords(sqlite3 *db, int limit)
{
    cJSON *arr = cJSON_CreateArray();

    /* Use symbol names directly, sorted by frequency.
     * This is a pragmatic approach: group by lowercased name. */
    const char *sql =
        "SELECT LOWER(name), COUNT(*) AS freq FROM symbols "
        "WHERE LENGTH(name) > 2 "
        "GROUP BY LOWER(name) ORDER BY freq DESC LIMIT ?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return arr;
    sqlite3_bind_int(stmt, 1, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const char *kw = (const char *)sqlite3_column_text(stmt, 0);
        int freq = sqlite3_column_int(stmt, 1);
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "keyword", kw ? kw : "");
        cJSON_AddNumberToObject(entry, "count", freq);
        cJSON_AddItemToArray(arr, entry);
    }
    sqlite3_finalize(stmt);
    return arr;
}

/* Kind distribution: count of symbols per kind. */
static cJSON *query_kind_distribution(sqlite3 *db)
{
    cJSON *obj = cJSON_CreateObject();
    const char *sql =
        "SELECT kind, COUNT(*) FROM symbols GROUP BY kind ORDER BY COUNT(*) DESC";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return obj;
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const char *kind = (const char *)sqlite3_column_text(stmt, 0);
        int cnt = sqlite3_column_int(stmt, 1);
        if (kind)
            cJSON_AddNumberToObject(obj, kind, cnt);
    }
    sqlite3_finalize(stmt);
    return obj;
}

/* Language distribution: count of files per language. */
static cJSON *query_language_distribution(sqlite3 *db)
{
    cJSON *obj = cJSON_CreateObject();
    const char *sql =
        "SELECT language, COUNT(DISTINCT file) FROM symbols "
        "GROUP BY language ORDER BY COUNT(DISTINCT file) DESC";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return obj;
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const char *lang = (const char *)sqlite3_column_text(stmt, 0);
        int cnt = sqlite3_column_int(stmt, 1);
        if (lang)
            cJSON_AddNumberToObject(obj, lang, cnt);
    }
    sqlite3_finalize(stmt);
    return obj;
}

/* Most imported files: top 10 by import count. */
static cJSON *query_most_imported(sqlite3 *db, int limit)
{
    cJSON *arr = cJSON_CreateArray();
    const char *sql =
        "SELECT resolved_file, COUNT(DISTINCT source_file) AS import_count "
        "FROM imports WHERE resolved_file != '' "
        "GROUP BY resolved_file ORDER BY import_count DESC LIMIT ?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return arr;
    sqlite3_bind_int(stmt, 1, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const char *f = (const char *)sqlite3_column_text(stmt, 0);
        int cnt = sqlite3_column_int(stmt, 1);
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "file", f ? f : "");
        cJSON_AddNumberToObject(entry, "import_count", cnt);
        cJSON_AddItemToArray(arr, entry);
    }
    sqlite3_finalize(stmt);
    return arr;
}

/* Build example queries based on the indexed data. */
static cJSON *build_example_queries(cJSON *top_keywords, cJSON *kind_dist,
                                     cJSON *most_imported)
{
    cJSON *arr = cJSON_CreateArray();

    /* Example 1: search for the most common keyword */
    if (cJSON_GetArraySize(top_keywords) > 0)
    {
        cJSON *first = cJSON_GetArrayItem(top_keywords, 0);
        const char *kw = cJSON_GetStringValue(
            cJSON_GetObjectItem(first, "keyword"));
        if (kw)
        {
            char cmd[256];
            snprintf(cmd, sizeof(cmd), "search:symbols %s", kw);
            cJSON_AddItemToArray(arr, cJSON_CreateString(cmd));
        }
    }

    /* Example 2: search for classes if they exist */
    cJSON *class_cnt = cJSON_GetObjectItem(kind_dist, "class");
    if (cJSON_IsNumber(class_cnt) && class_cnt->valueint > 0)
    {
        cJSON_AddItemToArray(arr,
            cJSON_CreateString("search:symbols --kind class --limit 10"));
    }

    /* Example 3: search for functions */
    cJSON *func_cnt = cJSON_GetObjectItem(kind_dist, "function");
    if (cJSON_IsNumber(func_cnt) && func_cnt->valueint > 0)
    {
        cJSON_AddItemToArray(arr,
            cJSON_CreateString("search:symbols --kind function --limit 10"));
    }

    /* Example 4: inspect most-imported file */
    if (cJSON_GetArraySize(most_imported) > 0)
    {
        cJSON *first = cJSON_GetArrayItem(most_imported, 0);
        const char *f = cJSON_GetStringValue(
            cJSON_GetObjectItem(first, "file"));
        if (f)
        {
            char cmd[512];
            snprintf(cmd, sizeof(cmd), "inspect:outline %s", f);
            cJSON_AddItemToArray(arr, cJSON_CreateString(cmd));
        }
    }

    /* Example 5: text search for common patterns */
    cJSON_AddItemToArray(arr,
        cJSON_CreateString("search:text TODO --group-by file"));

    return arr;
}

cJSON *tt_cmd_suggest_exec(tt_cli_opts_t *opts)
{
    tt_timer_start();

    char *project_path = tt_resolve_project_path(opts->path);
    if (!project_path)
    {
        tt_error_set("Failed to resolve project path");
        return NULL;
    }

    if (!tt_database_exists(project_path))
    {
        cJSON *err = make_error("no_index", "No index found.",
                                "Run \"toktoken index:create\" first.");
        free(project_path);
        return err;
    }

    tt_database_t db;
    if (tt_database_open(&db, project_path) < 0)
    {
        cJSON *err = make_error("db_open_failed", tt_error_get(), NULL);
        free(project_path);
        return err;
    }

    cJSON *result = cJSON_CreateObject();

    cJSON *keywords = query_top_keywords(db.db, 20);
    cJSON_AddItemToObject(result, "top_keywords", keywords);

    cJSON *kinds = query_kind_distribution(db.db);
    cJSON_AddItemToObject(result, "kind_distribution", kinds);

    cJSON *langs = query_language_distribution(db.db);
    cJSON_AddItemToObject(result, "language_distribution", langs);

    cJSON *imported = query_most_imported(db.db, 10);
    cJSON_AddItemToObject(result, "most_imported_files", imported);

    cJSON *examples = build_example_queries(keywords, kinds, imported);
    cJSON_AddItemToObject(result, "example_queries", examples);

    tt_database_close(&db);
    free(project_path);
    return result;
}

int tt_cmd_suggest(tt_cli_opts_t *opts)
{
    cJSON *result = tt_cmd_suggest_exec(opts);
    if (!result)
    {
        const char *err = tt_error_get();
        if (err && err[0])
            return tt_output_error("internal_error", err, NULL);
        return 0;
    }
    if (cJSON_GetObjectItem(result, "error"))
    {
        tt_json_print(result);
        cJSON_Delete(result);
        return 1;
    }
    tt_json_print(result);
    cJSON_Delete(result);
    return 0;
}
