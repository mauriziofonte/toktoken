/*
 * test_e2e_full_helpers.h -- Shared index lifecycle + macros for full-project E2E tests.
 */

#ifndef TT_TEST_E2E_FULL_HELPERS_H
#define TT_TEST_E2E_FULL_HELPERS_H

#include "test_framework.h"
#include "test_helpers.h"
#include "test_e2e_helpers.h"

#include <stdlib.h>
#include <string.h>

/* Shared state: fixture path + index-creation flag. */
static const char *g_full_fixture;
static int g_full_indexed;

/*
 * full_ensure_index -- Create index on full-project fixture (idempotent).
 *
 * Uses --full to include graphql/hcl/nix files that the smart filter excludes.
 */
static void full_ensure_index(void)
{
    if (g_full_indexed) return;

    g_full_fixture = tt_test_fixture("full-project");
    if (!g_full_fixture) return;

    char cmd[1024];

    /* Clear any stale index */
    snprintf(cmd, sizeof(cmd), "cache:clear --path %s", g_full_fixture);
    tt_e2e_run(cmd, NULL);

    /* Create fresh index with --full */
    snprintf(cmd, sizeof(cmd), "index:create --full --path %s", g_full_fixture);
    cJSON *json = NULL;
    int rc = tt_e2e_run(cmd, &json);
    g_full_indexed = (rc == 0);
    if (json) cJSON_Delete(json);
}

/* ---- Helper: search for a symbol and return a cJSON field ---- */

__attribute__((unused))
static cJSON *full_search_symbol(const char *query, const char *extra_flags)
{
    if (!g_full_indexed) return NULL;
    char cmd[1024];
    if (extra_flags && extra_flags[0]) {
        snprintf(cmd, sizeof(cmd), "search:symbols \"%s\" %s --path %s",
                 query, extra_flags, g_full_fixture);
    } else {
        snprintf(cmd, sizeof(cmd), "search:symbols \"%s\" --path %s",
                 query, g_full_fixture);
    }
    cJSON *json = NULL;
    tt_e2e_run(cmd, &json);
    return json;
}

/* Extract first symbol ID from a search result. Returns static buffer. */
__attribute__((unused))
static const char *full_first_symbol_id(cJSON *search_json)
{
    static char id_buf[512];
    if (!search_json) return NULL;
    cJSON *results = cJSON_GetObjectItemCaseSensitive(search_json, "results");
    if (!cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0) return NULL;
    cJSON *first = cJSON_GetArrayItem(results, 0);
    cJSON *id = cJSON_GetObjectItemCaseSensitive(first, "id");
    if (!cJSON_IsString(id)) return NULL;
    snprintf(id_buf, sizeof(id_buf), "%s", id->valuestring);
    return id_buf;
}

/* ---- Macro: per-language symbol search test ---- */

#define E2E_SEARCH_LANG(test_name, query, expected_kind, file_substr) \
TT_TEST(test_name) \
{ \
    full_ensure_index(); \
    if (!g_full_indexed) return; \
    cJSON *json = full_search_symbol(query, ""); \
    TT_ASSERT_NOT_NULL(json); \
    if (!json) return; \
    cJSON *n = cJSON_GetObjectItemCaseSensitive(json, "n"); \
    TT_ASSERT(cJSON_IsNumber(n) && n->valueint >= 1, \
              "search:symbols " query " should find >= 1 result"); \
    cJSON *results = cJSON_GetObjectItemCaseSensitive(json, "results"); \
    if (cJSON_IsArray(results) && cJSON_GetArraySize(results) > 0) { \
        int found_kind = 0, found_file = 0; \
        cJSON *r; \
        cJSON_ArrayForEach(r, results) { \
            cJSON *k = cJSON_GetObjectItemCaseSensitive(r, "kind"); \
            cJSON *f = cJSON_GetObjectItemCaseSensitive(r, "file"); \
            if (cJSON_IsString(k) && strcmp(k->valuestring, expected_kind) == 0) \
                found_kind = 1; \
            if (cJSON_IsString(f) && strstr(f->valuestring, file_substr)) \
                found_file = 1; \
        } \
        TT_ASSERT(found_kind, "should find kind=" expected_kind " for " query); \
        TT_ASSERT(found_file, "should find file containing " file_substr " for " query); \
    } \
    cJSON_Delete(json); \
}

/* ---- Macro: per-language outline test ---- */

#define E2E_OUTLINE_LANG(test_name, file_rel, min_syms) \
TT_TEST(test_name) \
{ \
    full_ensure_index(); \
    if (!g_full_indexed) return; \
    char cmd[1024]; \
    snprintf(cmd, sizeof(cmd), "inspect:outline \"%s\" --path %s", \
             file_rel, g_full_fixture); \
    cJSON *json = NULL; \
    int rc = tt_e2e_run(cmd, &json); \
    TT_ASSERT_EQ_INT(0, rc); \
    TT_ASSERT_NOT_NULL(json); \
    if (!json) return; \
    cJSON *symbols = cJSON_GetObjectItemCaseSensitive(json, "symbols"); \
    TT_ASSERT(cJSON_IsArray(symbols), "outline should have symbols array"); \
    if (cJSON_IsArray(symbols)) { \
        TT_ASSERT_GE_INT(cJSON_GetArraySize(symbols), min_syms); \
    } \
    cJSON_Delete(json); \
}

/* ---- Macro: per-language importers test ---- */

#define E2E_IMPORTERS_LANG(test_name, file_rel, expected_count, importer_substr) \
TT_TEST(test_name) \
{ \
    full_ensure_index(); \
    if (!g_full_indexed) return; \
    char cmd[1024]; \
    snprintf(cmd, sizeof(cmd), "find:importers \"%s\" --path %s", \
             file_rel, g_full_fixture); \
    cJSON *json = NULL; \
    int rc = tt_e2e_run(cmd, &json); \
    TT_ASSERT_EQ_INT(0, rc); \
    TT_ASSERT_NOT_NULL(json); \
    if (!json) return; \
    cJSON *importers = cJSON_GetObjectItemCaseSensitive(json, "importers"); \
    TT_ASSERT(cJSON_IsArray(importers), "should have importers array"); \
    if (cJSON_IsArray(importers)) { \
        TT_ASSERT_GE_INT(cJSON_GetArraySize(importers), expected_count); \
        int found = 0; \
        cJSON *imp; \
        cJSON_ArrayForEach(imp, importers) { \
            cJSON *f = cJSON_GetObjectItemCaseSensitive(imp, "from_file"); \
            if (cJSON_IsString(f) && strstr(f->valuestring, importer_substr)) \
                found = 1; \
        } \
        TT_ASSERT(found, "should find importer containing " importer_substr); \
    } \
    cJSON_Delete(json); \
}

#endif /* TT_TEST_E2E_FULL_HELPERS_H */
