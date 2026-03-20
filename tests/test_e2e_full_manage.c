/*
 * test_e2e_full_manage.c -- E2E tests for stats, codebase:detect, cache:clear.
 *
 * NOTE: cache:clear runs LAST because it destroys the index.
 */

#include "test_e2e_full_helpers.h"

/* ---------- stats ---------- */

TT_TEST(test_full_stats)
{
    full_ensure_index();
    if (!g_full_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "stats --path %s", g_full_fixture);
    cJSON *json = NULL;
    int rc = tt_e2e_run(cmd, &json);

    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_NOT_NULL(json);
    if (json) {
        cJSON *files = cJSON_GetObjectItemCaseSensitive(json, "files");
        if (cJSON_IsNumber(files))
            TT_ASSERT_GE_INT(files->valueint, 40);

        cJSON *symbols = cJSON_GetObjectItemCaseSensitive(json, "symbols");
        if (cJSON_IsNumber(symbols))
            TT_ASSERT_GE_INT(symbols->valueint, 500);

        cJSON *languages = cJSON_GetObjectItemCaseSensitive(json, "languages");
        TT_ASSERT(cJSON_IsObject(languages), "should have languages object");

        cJSON *kinds = cJSON_GetObjectItemCaseSensitive(json, "kinds");
        TT_ASSERT_NOT_NULL(kinds);

        cJSON_Delete(json);
    }
}

/* ---------- codebase:detect (with index) ---------- */

TT_TEST(test_full_codebase_detect)
{
    full_ensure_index();
    if (!g_full_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "codebase:detect --path %s", g_full_fixture);
    cJSON *json = NULL;
    int rc = tt_e2e_run(cmd, &json);

    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_NOT_NULL(json);
    if (json) {
        cJSON *is_cb = cJSON_GetObjectItemCaseSensitive(json, "is_codebase");
        TT_ASSERT(cJSON_IsTrue(is_cb), "should be a codebase");

        cJSON *has_idx = cJSON_GetObjectItemCaseSensitive(json, "has_index");
        TT_ASSERT(cJSON_IsTrue(has_idx), "should have index");

        cJSON *action = cJSON_GetObjectItemCaseSensitive(json, "action");
        if (cJSON_IsString(action))
            TT_ASSERT_EQ_STR("ready", action->valuestring);

        cJSON_Delete(json);
    }
}

/* ---------- codebase:detect (without index) ---------- */

TT_TEST(test_full_codebase_detect_no_index)
{
    const char *fixture = tt_test_fixture("full-project");
    if (!fixture) return;

    /* Clear index first */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "cache:clear --path %s", fixture);
    tt_e2e_run(cmd, NULL);

    snprintf(cmd, sizeof(cmd), "codebase:detect --path %s", fixture);
    cJSON *json = NULL;
    int rc = tt_e2e_run(cmd, &json);

    TT_ASSERT_EQ_INT(0, rc);
    if (json) {
        cJSON *is_cb = cJSON_GetObjectItemCaseSensitive(json, "is_codebase");
        TT_ASSERT(cJSON_IsTrue(is_cb), "should still be a codebase");

        cJSON *has_idx = cJSON_GetObjectItemCaseSensitive(json, "has_index");
        TT_ASSERT(cJSON_IsFalse(has_idx), "should NOT have index");

        cJSON *action = cJSON_GetObjectItemCaseSensitive(json, "action");
        if (cJSON_IsString(action))
            TT_ASSERT_EQ_STR("index:create", action->valuestring);

        cJSON_Delete(json);
    }

    /* Restore index for any subsequent tests */
    g_full_indexed = 0;
    full_ensure_index();
}

/* ---------- cache:clear (runs LAST) ---------- */

TT_TEST(test_full_cache_clear)
{
    full_ensure_index();
    if (!g_full_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "cache:clear --path %s", g_full_fixture);
    cJSON *json = NULL;
    int rc = tt_e2e_run(cmd, &json);

    TT_ASSERT_EQ_INT(0, rc);
    if (json) {
        /* cache:clear returns {deleted: path, freed_bytes: N} */
        cJSON *deleted = cJSON_GetObjectItemCaseSensitive(json, "deleted");
        TT_ASSERT(cJSON_IsString(deleted), "should have deleted path");
        cJSON *freed = cJSON_GetObjectItemCaseSensitive(json, "freed_bytes");
        TT_ASSERT(cJSON_IsNumber(freed), "should have freed_bytes");
        cJSON_Delete(json);
    }

    g_full_indexed = 0;
}

void run_e2e_full_manage_tests(void)
{
    TT_RUN(test_full_stats);
    TT_RUN(test_full_codebase_detect);
    TT_RUN(test_full_codebase_detect_no_index);
    /* cache:clear MUST be last -- destroys the index */
    TT_RUN(test_full_cache_clear);
}
