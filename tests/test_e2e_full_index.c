/*
 * test_e2e_full_index.c -- E2E tests for index:create/update on full-project fixture.
 */

#include "test_e2e_full_helpers.h"

/* ---------- index:create ---------- */

TT_TEST(test_full_index_create)
{
    /* Force fresh index */
    g_full_indexed = 0;
    g_full_fixture = tt_test_fixture("full-project");
    if (!g_full_fixture) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "cache:clear --path %s", g_full_fixture);
    tt_e2e_run(cmd, NULL);

    snprintf(cmd, sizeof(cmd), "index:create --full --path %s", g_full_fixture);
    cJSON *json = NULL;
    int rc = tt_e2e_run(cmd, &json);

    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_NOT_NULL(json);

    if (json) {
        cJSON *files = cJSON_GetObjectItemCaseSensitive(json, "files");
        TT_ASSERT(cJSON_IsNumber(files), "should have files field");
        if (cJSON_IsNumber(files))
            TT_ASSERT_GE_INT(files->valueint, 45);

        cJSON *symbols = cJSON_GetObjectItemCaseSensitive(json, "symbols");
        TT_ASSERT(cJSON_IsNumber(symbols), "should have symbols field");
        if (cJSON_IsNumber(symbols))
            TT_ASSERT_GE_INT(symbols->valueint, 500);

        /* languages is an object {lang: count}, not an array */
        cJSON *languages = cJSON_GetObjectItemCaseSensitive(json, "languages");
        TT_ASSERT(cJSON_IsObject(languages), "should have languages object");
        if (cJSON_IsObject(languages)) {
            int lang_count = cJSON_GetArraySize(languages);
            TT_ASSERT_GE_INT(lang_count, 18);
        }

        cJSON_Delete(json);
    }

    g_full_indexed = (rc == 0);
}

/* ---------- index:update (no changes) ---------- */

TT_TEST(test_full_index_update_no_changes)
{
    full_ensure_index();
    if (!g_full_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "index:update --path %s", g_full_fixture);
    cJSON *json = NULL;
    int rc = tt_e2e_run(cmd, &json);

    TT_ASSERT_EQ_INT(0, rc);

    if (json) {
        cJSON *added = cJSON_GetObjectItemCaseSensitive(json, "added");
        cJSON *changed = cJSON_GetObjectItemCaseSensitive(json, "changed");
        /* Allow small variance (timing-dependent file discovery) */
        int total = 0;
        if (cJSON_IsNumber(added)) total += added->valueint;
        if (cJSON_IsNumber(changed)) total += changed->valueint;
        TT_ASSERT_LE_INT(total, 2);
        cJSON_Delete(json);
    }
}

/* ---------- index:file ---------- */

TT_TEST(test_full_index_file)
{
    full_ensure_index();
    if (!g_full_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "index:file \"src/python/main.py\" --path %s", g_full_fixture);
    cJSON *json = NULL;
    int rc = tt_e2e_run(cmd, &json);

    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_NOT_NULL(json);
    if (json) {
        cJSON *file = cJSON_GetObjectItemCaseSensitive(json, "file");
        if (cJSON_IsString(file))
            TT_ASSERT_STR_CONTAINS(file->valuestring, "main.py");

        cJSON *dur = cJSON_GetObjectItemCaseSensitive(json, "duration_seconds");
        TT_ASSERT(cJSON_IsNumber(dur), "should have duration_seconds");

        cJSON_Delete(json);
    }
}

void run_e2e_full_index_tests(void)
{
    TT_RUN(test_full_index_create);
    TT_RUN(test_full_index_update_no_changes);
    TT_RUN(test_full_index_file);
}
