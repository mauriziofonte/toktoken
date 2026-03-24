/*
 * cmd_find.c -- find:importers and find:references commands.
 *
 * find:importers <file>       -> who imports this file?
 * find:references <identifier> -> who imports this symbol name?
 */

#include "cmd_find.h"
#include "json_output.h"
#include "error.h"
#include "platform.h"
#include "database.h"
#include "index_store.h"
#include "import_extractor.h"
#include "str_util.h"
#include "output_fmt.h"

#include "hashmap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sqlite3.h>

static const char *resolve_query(tt_cli_opts_t *opts)
{
    if (opts->search && opts->search[0]) return opts->search;
    if (opts->positional_count > 0) return opts->positional[0];
    return "";
}

static cJSON *make_error(const char *code, const char *message, const char *hint)
{
    cJSON *json = cJSON_CreateObject();
    if (!json) return NULL;
    cJSON_AddStringToObject(json, "error", code);
    cJSON_AddStringToObject(json, "message", message);
    if (hint && hint[0])
        cJSON_AddStringToObject(json, "hint", hint);
    return json;
}

static cJSON *format_imports(const tt_import_t *imps, int count,
                             tt_hashmap_t *importers_map)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "from_file", imps[i].from_file ? imps[i].from_file : "");
        cJSON_AddStringToObject(obj, "to_specifier", imps[i].to_specifier ? imps[i].to_specifier : "");
        if (imps[i].to_file)
            cJSON_AddStringToObject(obj, "to_file", imps[i].to_file);
        if (imps[i].symbol_name)
            cJSON_AddStringToObject(obj, "symbol_name", imps[i].symbol_name);
        cJSON_AddNumberToObject(obj, "line", imps[i].line);
        cJSON_AddStringToObject(obj, "import_type", imps[i].import_type ? imps[i].import_type : "");
        if (importers_map && imps[i].from_file) {
            bool val = tt_hashmap_has(importers_map, imps[i].from_file);
            cJSON_AddBoolToObject(obj, "has_importers", val);
        }
        cJSON_AddItemToArray(arr, obj);
    }
    return arr;
}

cJSON *tt_cmd_find_importers_exec(tt_cli_opts_t *opts)
{
    const char *query = resolve_query(opts);
    if (!query || !query[0]) {
        return make_error("missing_argument",
                           "Usage: find:importers <file>",
                           "Specify a file path to find its importers");
    }

    char *project_path = tt_resolve_project_path(opts->path);
    if (!project_path) {
        tt_error_set("Failed to resolve project path");
        return NULL;
    }

    tt_database_t db;
    memset(&db, 0, sizeof(db));
    if (tt_database_open(&db, project_path) < 0) {
        free(project_path);
        return make_error("storage_error", "Failed to open database", NULL);
    }

    tt_index_store_t store;
    if (tt_store_init(&store, &db) < 0) {
        tt_database_close(&db);
        free(project_path);
        return make_error("storage_error", "Failed to prepare statements", NULL);
    }

    tt_import_t *imps = NULL;
    int count = 0;
    tt_store_get_importers(&store, query, &imps, &count);

    /* Build has_importers map if requested.
     * Keys = unique from_file values. Value = (void*)1 if file has importers,
     * absent from map if not (we use a second "checked" map to avoid re-querying). */
    tt_hashmap_t *importers_map = NULL;
    if (opts->has_importers && count > 0) {
        importers_map = tt_hashmap_new(count > 4 ? (size_t)count * 2 : 8);
        tt_hashmap_t *checked = tt_hashmap_new(count > 4 ? (size_t)count * 2 : 8);
        for (int i = 0; i < count; i++) {
            if (!imps[i].from_file || tt_hashmap_has(checked, imps[i].from_file))
                continue;
            tt_hashmap_set(checked, imps[i].from_file, (void *)1);
            int c = tt_store_count_importers(&store, imps[i].from_file);
            if (c > 0)
                tt_hashmap_set(importers_map, imps[i].from_file, (void *)1);
        }
        tt_hashmap_free(checked);
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "file", query);
    cJSON_AddNumberToObject(result, "count", count);
    cJSON_AddItemToObject(result, "importers", format_imports(imps, count, importers_map));

    if (importers_map)
        tt_hashmap_free(importers_map);
    tt_import_array_free(imps, count);
    tt_store_close(&store);
    tt_database_close(&db);
    free(project_path);
    return result;
}

cJSON *tt_cmd_find_references_exec(tt_cli_opts_t *opts)
{
    const char *query = resolve_query(opts);
    if (!query || !query[0]) {
        return make_error("missing_argument",
                           "Usage: find:references <identifier>",
                           "Specify an identifier to find its references");
    }

    char *project_path = tt_resolve_project_path(opts->path);
    if (!project_path) {
        tt_error_set("Failed to resolve project path");
        return NULL;
    }

    tt_database_t db;
    memset(&db, 0, sizeof(db));
    if (tt_database_open(&db, project_path) < 0) {
        free(project_path);
        return make_error("storage_error", "Failed to open database", NULL);
    }

    tt_index_store_t store;
    if (tt_store_init(&store, &db) < 0) {
        tt_database_close(&db);
        free(project_path);
        return make_error("storage_error", "Failed to prepare statements", NULL);
    }

    tt_import_t *imps = NULL;
    int count = 0;
    tt_store_find_references(&store, query, &imps, &count);

    /* --check: compact boolean reference check */
    if (opts->check) {
        int import_count = count;

        /* Content references: count FTS5 hits for the identifier in symbol text */
        int content_count = 0;
        const char *cnt_sql =
            "SELECT COUNT(*) FROM symbols_fts WHERE symbols_fts MATCH ?";
        sqlite3_stmt *cnt_stmt = NULL;
        if (sqlite3_prepare_v2(store.db->db, cnt_sql, -1, &cnt_stmt, NULL)
            == SQLITE_OK) {
            sqlite3_bind_text(cnt_stmt, 1, query, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(cnt_stmt) == SQLITE_ROW)
                content_count = sqlite3_column_int(cnt_stmt, 0);
            sqlite3_finalize(cnt_stmt);
        }

        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "identifier", query);
        cJSON_AddBoolToObject(result, "is_referenced",
                              import_count > 0 || content_count > 0);
        cJSON_AddNumberToObject(result, "import_count", import_count);
        cJSON_AddNumberToObject(result, "content_count", content_count);

        tt_import_array_free(imps, count);
        tt_store_close(&store);
        tt_database_close(&db);
        free(project_path);
        return result;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "identifier", query);
    cJSON_AddNumberToObject(result, "count", count);
    cJSON_AddItemToObject(result, "references", format_imports(imps, count, NULL));

    tt_import_array_free(imps, count);
    tt_store_close(&store);
    tt_database_close(&db);
    free(project_path);
    return result;
}

int tt_cmd_find_importers(tt_cli_opts_t *opts)
{
    cJSON *result = tt_cmd_find_importers_exec(opts);
    if (!result) {
        const char *err = tt_error_get();
        if (err && err[0])
            return tt_output_error("internal_error", err, NULL);
        return 0;
    }
    if (cJSON_GetObjectItem(result, "error")) {
        tt_json_print(result);
        cJSON_Delete(result);
        return 1;
    }
    tt_json_print(result);
    cJSON_Delete(result);
    return 0;
}

int tt_cmd_find_references(tt_cli_opts_t *opts)
{
    cJSON *result = tt_cmd_find_references_exec(opts);
    if (!result) {
        const char *err = tt_error_get();
        if (err && err[0])
            return tt_output_error("internal_error", err, NULL);
        return 0;
    }
    if (cJSON_GetObjectItem(result, "error")) {
        tt_json_print(result);
        cJSON_Delete(result);
        return 1;
    }
    tt_json_print(result);
    cJSON_Delete(result);
    return 0;
}

cJSON *tt_cmd_find_callers_exec(tt_cli_opts_t *opts)
{
    const char *query = resolve_query(opts);
    if (!query || !query[0]) {
        return make_error("missing_argument",
                           "Usage: find:callers <symbol-id>",
                           "Specify a symbol ID to find its callers");
    }

    char *project_path = tt_resolve_project_path(opts->path);
    if (!project_path) {
        tt_error_set("Failed to resolve project path");
        return NULL;
    }

    tt_database_t db;
    memset(&db, 0, sizeof(db));
    if (tt_database_open(&db, project_path) < 0) {
        free(project_path);
        return make_error("storage_error", "Failed to open database", NULL);
    }

    tt_index_store_t store;
    if (tt_store_init(&store, &db) < 0) {
        tt_database_close(&db);
        free(project_path);
        return make_error("storage_error", "Failed to prepare statements", NULL);
    }

    int limit = opts->limit > 0 ? opts->limit : 50;
    tt_caller_t *callers = NULL;
    int count = 0;
    tt_store_find_callers(&store, query, limit, &callers, &count);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "symbol", query);
    cJSON_AddNumberToObject(result, "n", count);
    cJSON *arr = cJSON_AddArrayToObject(result, "callers");
    for (int i = 0; i < count; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "id", callers[i].id);
        cJSON_AddStringToObject(obj, "file", callers[i].file);
        cJSON_AddStringToObject(obj, "name", callers[i].name);
        cJSON_AddStringToObject(obj, "kind", callers[i].kind);
        cJSON_AddNumberToObject(obj, "line", callers[i].line);
        if (callers[i].signature && callers[i].signature[0])
            cJSON_AddStringToObject(obj, "sig", callers[i].signature);
        cJSON_AddItemToArray(arr, obj);
    }

    tt_caller_free(callers, count);
    tt_store_close(&store);
    tt_database_close(&db);
    free(project_path);
    return result;
}

int tt_cmd_find_callers(tt_cli_opts_t *opts)
{
    cJSON *result = tt_cmd_find_callers_exec(opts);
    if (!result) {
        const char *err = tt_error_get();
        if (err && err[0])
            return tt_output_error("internal_error", err, NULL);
        return 0;
    }
    if (cJSON_GetObjectItem(result, "error")) {
        tt_json_print(result);
        cJSON_Delete(result);
        return 1;
    }
    tt_json_print(result);
    cJSON_Delete(result);
    return 0;
}

/* ---- find:dead -- unreferenced symbol detection ---- */

static bool is_entry_point(const char *name)
{
    static const char *entry_points[] = {
        "main", "__init__", "__main__", "setUp", "tearDown",
        "setup", "configure", "register", "boot", "init",
        "handle", "run", "execute", "__construct", "__destruct",
        NULL
    };
    for (int i = 0; entry_points[i]; i++) {
        if (strcmp(name, entry_points[i]) == 0)
            return true;
    }
    /* test_* or Test* */
    if (strncmp(name, "test_", 5) == 0 || strncmp(name, "Test", 4) == 0)
        return true;
    return false;
}

static bool is_test_file(const char *path)
{
    /* Extract basename */
    const char *base = path;
    const char *p = path;
    while (*p) {
        if (*p == '/' || *p == '\\')
            base = p + 1;
        p++;
    }
    /* Check patterns: test*, spec*, *_test.*, *_spec.*, *.test.*, *.spec.* */
    if (strncasecmp(base, "test", 4) == 0 || strncasecmp(base, "spec", 4) == 0)
        return true;
    /* Check for _test. or _spec. or .test. or .spec. anywhere in basename */
    const char *s = base;
    while (*s) {
        if ((*s == '_' || *s == '.') && s[1]) {
            if (strncasecmp(s + 1, "test", 4) == 0 &&
                (s[5] == '.' || s[5] == '_' || s[5] == '\0'))
                return true;
            if (strncasecmp(s + 1, "spec", 4) == 0 &&
                (s[5] == '.' || s[5] == '_' || s[5] == '\0'))
                return true;
        }
        s++;
    }
    return false;
}

static const char *confidence_for_language(const char *lang)
{
    if (!lang || !lang[0]) return "medium";

    if (strcasecmp(lang, "c") == 0 || strcasecmp(lang, "c++") == 0 ||
        strcasecmp(lang, "go") == 0 || strcasecmp(lang, "java") == 0 ||
        strcasecmp(lang, "kotlin") == 0 || strcasecmp(lang, "rust") == 0)
        return "high";

    if (strcasecmp(lang, "javascript") == 0 || strcasecmp(lang, "typescript") == 0 ||
        strcasecmp(lang, "python") == 0)
        return "medium";

    return "low";
}

static bool kind_matches_filter(const char *kind, const char *filter)
{
    if (!filter || !filter[0]) return true;
    if (!kind) return false;

    /* Comma-separated filter */
    const char *p = filter;
    while (*p) {
        const char *end = p;
        while (*end && *end != ',') end++;
        size_t len = (size_t)(end - p);
        if (len == strlen(kind) && strncasecmp(p, kind, len) == 0)
            return true;
        p = *end ? end + 1 : end;
    }
    return false;
}

cJSON *tt_cmd_find_dead_exec(tt_cli_opts_t *opts)
{
    char *project_path = tt_resolve_project_path(opts->path);
    if (!project_path) {
        tt_error_set("Failed to resolve project path");
        return NULL;
    }

    if (!tt_database_exists(project_path)) {
        free(project_path);
        return make_error("no_index", "No index found.",
                          "Run \"toktoken index:create\" first.");
    }

    tt_database_t db;
    memset(&db, 0, sizeof(db));
    if (tt_database_open(&db, project_path) < 0) {
        free(project_path);
        return make_error("storage_error", "Failed to open database", NULL);
    }

    tt_index_store_t store;
    if (tt_store_init(&store, &db) < 0) {
        tt_database_close(&db);
        free(project_path);
        return make_error("storage_error", "Failed to prepare statements", NULL);
    }

    int limit = opts->limit > 0 ? opts->limit : 100;
    sqlite3 *sdb = db.db;

    /* Query 1: "dead" symbols — file has NO importers */
    const char *sql_dead =
        "SELECT s.id, s.file, s.name, s.kind, s.line, s.language "
        "FROM symbols s "
        "WHERE s.file NOT IN (SELECT DISTINCT resolved_file FROM imports WHERE resolved_file != '') "
        "ORDER BY s.file, s.line";

    /* Query 2: "unreferenced" symbols — file IS imported but symbol
     * name not found in any import's kind column (imported identifier)
     * targeting the SAME file. Must be correlated (resolved_file = s.file)
     * to avoid false negatives from identically-named symbols in other files. */
    const char *sql_unreferenced =
        "SELECT s.id, s.file, s.name, s.kind, s.line, s.language "
        "FROM symbols s "
        "WHERE s.file IN (SELECT DISTINCT resolved_file FROM imports WHERE resolved_file != '') "
        "AND s.name NOT IN ("
        "  SELECT DISTINCT kind FROM imports "
        "  WHERE kind != 'module' AND resolved_file = s.file"
        ") "
        "ORDER BY s.file, s.line";

    cJSON *results = cJSON_CreateArray();
    int dead_count = 0, unreferenced_count = 0;
    int total = 0;

    /* Run both queries */
    const char *queries[] = {sql_dead, sql_unreferenced};
    const char *classifications[] = {"dead", "unreferenced"};

    for (int q = 0; q < 2 && total < limit; q++) {
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(sdb, queries[q], -1, &stmt, NULL) != SQLITE_OK)
            continue;

        while (sqlite3_step(stmt) == SQLITE_ROW && total < limit) {
            const char *id   = (const char *)sqlite3_column_text(stmt, 0);
            const char *file = (const char *)sqlite3_column_text(stmt, 1);
            const char *name = (const char *)sqlite3_column_text(stmt, 2);
            const char *kind = (const char *)sqlite3_column_text(stmt, 3);
            int line         = sqlite3_column_int(stmt, 4);
            const char *lang = (const char *)sqlite3_column_text(stmt, 5);

            if (!id || !name) continue;

            /* Kind filter */
            if (!kind_matches_filter(kind, opts->kind))
                continue;

            /* Language filter */
            if (opts->language && opts->language[0] &&
                (!lang || strcasecmp(lang, opts->language) != 0))
                continue;

            /* Filter/exclude */
            if (file && !tt_matches_path_filters(file, opts->filter, opts->exclude))
                continue;

            /* Exclude tests */
            if (opts->exclude_tests && file && is_test_file(file))
                continue;

            /* Entry point exclusion */
            if (is_entry_point(name))
                continue;

            cJSON *obj = cJSON_CreateObject();
            cJSON_AddStringToObject(obj, "id", id);
            if (file) cJSON_AddStringToObject(obj, "file", file);
            cJSON_AddStringToObject(obj, "name", name);
            if (kind) cJSON_AddStringToObject(obj, "kind", kind);
            cJSON_AddNumberToObject(obj, "line", line);
            cJSON_AddStringToObject(obj, "classification", classifications[q]);
            cJSON_AddStringToObject(obj, "confidence", confidence_for_language(lang));
            cJSON_AddItemToArray(results, obj);

            if (q == 0) dead_count++;
            else unreferenced_count++;
            total++;
        }

        sqlite3_finalize(stmt);
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "results", results);

    cJSON *summary = cJSON_CreateObject();
    cJSON_AddNumberToObject(summary, "dead", dead_count);
    cJSON_AddNumberToObject(summary, "unreferenced", unreferenced_count);
    cJSON_AddNumberToObject(summary, "total", total);
    cJSON_AddItemToObject(result, "summary", summary);

    tt_store_close(&store);
    tt_database_close(&db);
    free(project_path);
    return result;
}

int tt_cmd_find_dead(tt_cli_opts_t *opts)
{
    cJSON *result = tt_cmd_find_dead_exec(opts);
    if (!result) {
        const char *err = tt_error_get();
        if (err && err[0])
            return tt_output_error("internal_error", err, NULL);
        return 0;
    }
    if (cJSON_GetObjectItem(result, "error")) {
        tt_json_print(result);
        cJSON_Delete(result);
        return 1;
    }
    if (opts->count)
    {
        /* Count-only mode: print just the summary */
        cJSON *summary = cJSON_GetObjectItem(result, "summary");
        if (summary)
        {
            if (opts->format && strcmp(opts->format, "table") == 0)
            {
                cJSON *total = cJSON_GetObjectItem(summary, "total");
                printf("%d\n", total ? total->valueint : 0);
                fflush(stdout);
            }
            else
            {
                tt_json_print(summary);
            }
        }
        cJSON_Delete(result);
        return 0;
    }
    tt_json_print(result);
    cJSON_Delete(result);
    return 0;
}
