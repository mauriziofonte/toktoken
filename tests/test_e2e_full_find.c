/*
 * test_e2e_full_find.c -- E2E tests for find:importers/references/callers/dead.
 */

#include "test_e2e_full_helpers.h"

/* ---------- find:importers ---------- */

TT_TEST(test_full_find_importers_ts)
{
    full_ensure_index();
    if (!g_full_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "find:importers \"src/ts/types.ts\" --path %s", g_full_fixture);
    cJSON *json = NULL;
    int rc = tt_e2e_run(cmd, &json);

    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_NOT_NULL(json);
    if (json) {
        cJSON *importers = cJSON_GetObjectItemCaseSensitive(json, "importers");
        TT_ASSERT(cJSON_IsArray(importers), "should have importers array");
        if (cJSON_IsArray(importers)) {
            TT_ASSERT_GE_INT(cJSON_GetArraySize(importers), 1);
            int found = 0;
            cJSON *imp;
            cJSON_ArrayForEach(imp, importers) {
                cJSON *f = cJSON_GetObjectItemCaseSensitive(imp, "from_file");
                if (cJSON_IsString(f) && strstr(f->valuestring, "service.ts"))
                    found = 1;
            }
            TT_ASSERT(found, "types.ts should be imported by service.ts");
        }
        cJSON_Delete(json);
    }
}

TT_TEST(test_full_find_importers_js)
{
    full_ensure_index();
    if (!g_full_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "find:importers \"src/js/middleware.js\" --path %s", g_full_fixture);
    cJSON *json = NULL;
    int rc = tt_e2e_run(cmd, &json);

    TT_ASSERT_EQ_INT(0, rc);
    if (json) {
        cJSON *importers = cJSON_GetObjectItemCaseSensitive(json, "importers");
        if (cJSON_IsArray(importers)) {
            TT_ASSERT_GE_INT(cJSON_GetArraySize(importers), 1);
            int found = 0;
            cJSON *imp;
            cJSON_ArrayForEach(imp, importers) {
                cJSON *f = cJSON_GetObjectItemCaseSensitive(imp, "from_file");
                if (cJSON_IsString(f) && strstr(f->valuestring, "router.js"))
                    found = 1;
            }
            TT_ASSERT(found, "middleware.js should be imported by router.js");
        }
        cJSON_Delete(json);
    }
}

TT_TEST(test_full_find_importers_python)
{
    full_ensure_index();
    if (!g_full_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "find:importers \"src/python/models.py\" --path %s", g_full_fixture);
    cJSON *json = NULL;
    int rc = tt_e2e_run(cmd, &json);

    TT_ASSERT_EQ_INT(0, rc);
    if (json) {
        cJSON *importers = cJSON_GetObjectItemCaseSensitive(json, "importers");
        if (cJSON_IsArray(importers)) {
            TT_ASSERT_GE_INT(cJSON_GetArraySize(importers), 1);
            int found = 0;
            cJSON *imp;
            cJSON_ArrayForEach(imp, importers) {
                cJSON *f = cJSON_GetObjectItemCaseSensitive(imp, "from_file");
                if (cJSON_IsString(f) && strstr(f->valuestring, "analyzer.py"))
                    found = 1;
            }
            TT_ASSERT(found, "models.py should be imported by analyzer.py");
        }
        cJSON_Delete(json);
    }
}

/* ---------- find:references ---------- */

TT_TEST(test_full_find_references)
{
    full_ensure_index();
    if (!g_full_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "find:references Analyzer --path %s", g_full_fixture);
    cJSON *json = NULL;
    int rc = tt_e2e_run(cmd, &json);

    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_NOT_NULL(json);
    if (json) {
        cJSON *refs = cJSON_GetObjectItemCaseSensitive(json, "references");
        if (cJSON_IsArray(refs)) {
            TT_ASSERT_GE_INT(cJSON_GetArraySize(refs), 1);
            if (cJSON_GetArraySize(refs) > 0) {
                cJSON *first = cJSON_GetArrayItem(refs, 0);
                TT_ASSERT_NOT_NULL(
                    cJSON_GetObjectItemCaseSensitive(first, "from_file"));
            }
        }
        cJSON_Delete(json);
    }
}

/* ---------- find:callers ---------- */

TT_TEST(test_full_find_callers)
{
    full_ensure_index();
    if (!g_full_indexed) return;

    /* Use Config interface from types.ts — it's referenced in service.ts
     * via "private config: Config" and "Config, DataItem" import. */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "find:callers \"src/ts/types.ts::Config#interface\" --path %s",
             g_full_fixture);

    cJSON *json = NULL;
    int rc = tt_e2e_run(cmd, &json);

    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_NOT_NULL(json);
    if (json) {
        cJSON *n = cJSON_GetObjectItemCaseSensitive(json, "n");
        TT_ASSERT(cJSON_IsNumber(n) && n->valueint >= 1,
                   "Config should have >= 1 caller");

        cJSON *callers = cJSON_GetObjectItemCaseSensitive(json, "callers");
        TT_ASSERT(cJSON_IsArray(callers), "should have callers array");
        if (cJSON_IsArray(callers)) {
            TT_ASSERT_GE_INT(cJSON_GetArraySize(callers), 1);
            int found_service = 0;
            cJSON *c;
            cJSON_ArrayForEach(c, callers) {
                cJSON *f = cJSON_GetObjectItemCaseSensitive(c, "file");
                if (cJSON_IsString(f) && strstr(f->valuestring, "service.ts"))
                    found_service = 1;
            }
            TT_ASSERT(found_service,
                       "Config callers should include service.ts");
        }
        cJSON_Delete(json);
    }
}

/* ---------- find:dead ---------- */

TT_TEST(test_full_find_dead)
{
    full_ensure_index();
    if (!g_full_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "find:dead --path %s", g_full_fixture);
    cJSON *json = NULL;
    int rc = tt_e2e_run(cmd, &json);

    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_NOT_NULL(json);
    if (json) {
        cJSON *summary = cJSON_GetObjectItemCaseSensitive(json, "summary");
        TT_ASSERT_NOT_NULL(summary);
        if (summary) {
            cJSON *dead = cJSON_GetObjectItemCaseSensitive(summary, "dead");
            if (cJSON_IsNumber(dead))
                TT_ASSERT_GE_INT(dead->valueint, 1);
        }

        /* Verify orphan.py symbols are in results */
        cJSON *results = cJSON_GetObjectItemCaseSensitive(json, "results");
        if (cJSON_IsArray(results)) {
            int found_orphan = 0;
            cJSON *r;
            cJSON_ArrayForEach(r, results) {
                cJSON *file = cJSON_GetObjectItemCaseSensitive(r, "file");
                if (cJSON_IsString(file) &&
                    strstr(file->valuestring, "orphan.py"))
                    found_orphan = 1;
            }
            TT_ASSERT(found_orphan, "orphan.py should be flagged as dead");
        }
        cJSON_Delete(json);
    }
}

TT_TEST(test_full_find_dead_symbols)
{
    full_ensure_index();
    if (!g_full_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "find:dead --path %s", g_full_fixture);
    cJSON *json = NULL;
    int rc = tt_e2e_run(cmd, &json);

    TT_ASSERT_EQ_INT(0, rc);
    if (json) {
        /* Look for orphan_function in dead results */
        cJSON *results = cJSON_GetObjectItemCaseSensitive(json, "results");
        if (cJSON_IsArray(results)) {
            int found = 0;
            cJSON *r;
            cJSON_ArrayForEach(r, results) {
                cJSON *name = cJSON_GetObjectItemCaseSensitive(r, "name");
                if (cJSON_IsString(name) &&
                    strcmp(name->valuestring, "orphan_function") == 0)
                    found = 1;
            }
            TT_ASSERT(found, "orphan_function should be classified as dead");
        }
        cJSON_Delete(json);
    }
}

void run_e2e_full_find_tests(void)
{
    TT_RUN(test_full_find_importers_ts);
    TT_RUN(test_full_find_importers_js);
    TT_RUN(test_full_find_importers_python);
    TT_RUN(test_full_find_references);
    TT_RUN(test_full_find_callers);
    TT_RUN(test_full_find_dead);
    TT_RUN(test_full_find_dead_symbols);
}
