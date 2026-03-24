/*
 * test_e2e_full_search.c -- E2E tests for search:symbols/text/cooccurrence/similar.
 */

#include "test_e2e_full_helpers.h"

/* ---------- search:symbols cross-language ---------- */

TT_TEST(test_full_search_symbols_cross_lang)
{
    full_ensure_index();
    if (!g_full_indexed) return;

    cJSON *json = full_search_symbol("process", "");
    TT_ASSERT_NOT_NULL(json);
    if (json) {
        cJSON *n = cJSON_GetObjectItemCaseSensitive(json, "n");
        TT_ASSERT(cJSON_IsNumber(n) && n->valueint >= 2,
                   "should find >= 2 symbols matching 'process'");
        cJSON_Delete(json);
    }
}

/* ---------- search:symbols with kind filter ---------- */

TT_TEST(test_full_search_symbols_kind_filter)
{
    full_ensure_index();
    if (!g_full_indexed) return;

    cJSON *json = full_search_symbol("Service", "--kind class");
    TT_ASSERT_NOT_NULL(json);
    if (json) {
        cJSON *n = cJSON_GetObjectItemCaseSensitive(json, "n");
        TT_ASSERT(cJSON_IsNumber(n) && n->valueint >= 1,
                   "should find >= 1 class matching 'Service'");

        /* Verify all results are classes */
        cJSON *results = cJSON_GetObjectItemCaseSensitive(json, "results");
        if (cJSON_IsArray(results)) {
            cJSON *r;
            cJSON_ArrayForEach(r, results) {
                cJSON *k = cJSON_GetObjectItemCaseSensitive(r, "kind");
                if (cJSON_IsString(k))
                    TT_ASSERT_EQ_STR("class", k->valuestring);
            }
        }
        cJSON_Delete(json);
    }
}

/* ---------- search:symbols --count ---------- */

TT_TEST(test_full_search_symbols_count)
{
    full_ensure_index();
    if (!g_full_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "search:symbols validate --count --path %s", g_full_fixture);
    cJSON *json = NULL;
    tt_e2e_run(cmd, &json);

    TT_ASSERT_NOT_NULL(json);
    if (json) {
        cJSON *count = cJSON_GetObjectItemCaseSensitive(json, "count");
        TT_ASSERT(cJSON_IsNumber(count), "should have count field");
        if (cJSON_IsNumber(count))
            TT_ASSERT_GE_INT(count->valueint, 2);
        cJSON_Delete(json);
    }
}

/* ---------- search:text ---------- */

TT_TEST(test_full_search_text)
{
    full_ensure_index();
    if (!g_full_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "search:text return --path %s", g_full_fixture);
    cJSON *json = NULL;
    int rc = tt_e2e_run(cmd, &json);

    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_NOT_NULL(json);
    if (json) {
        cJSON *n = cJSON_GetObjectItemCaseSensitive(json, "n");
        TT_ASSERT(cJSON_IsNumber(n) && n->valueint >= 20,
                   "should find >= 20 lines containing 'return'");

        /* Verify result structure: f=file, l=line, t=text */
        cJSON *results = cJSON_GetObjectItemCaseSensitive(json, "results");
        if (cJSON_IsArray(results) && cJSON_GetArraySize(results) > 0) {
            cJSON *first = cJSON_GetArrayItem(results, 0);
            TT_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(first, "f"));
            TT_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(first, "l"));
            TT_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(first, "t"));
        }
        cJSON_Delete(json);
    }
}

/* ---------- search:text --group-by file ---------- */

TT_TEST(test_full_search_text_group_by)
{
    full_ensure_index();
    if (!g_full_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "search:text import --group-by file --path %s", g_full_fixture);
    cJSON *json = NULL;
    int rc = tt_e2e_run(cmd, &json);

    TT_ASSERT_EQ_INT(0, rc);
    if (json) {
        cJSON *groups = cJSON_GetObjectItemCaseSensitive(json, "groups");
        TT_ASSERT(groups != NULL, "should have groups field");
        if (cJSON_IsArray(groups))
            TT_ASSERT_GE_INT(cJSON_GetArraySize(groups), 5);
        cJSON_Delete(json);
    }
}

/* ---------- search:text --filter (path-based filtering) ---------- */

TT_TEST(test_full_search_text_filter)
{
    full_ensure_index();
    if (!g_full_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "search:text def --filter python --path %s", g_full_fixture);
    cJSON *json = NULL;
    int rc = tt_e2e_run(cmd, &json);

    TT_ASSERT_EQ_INT(0, rc);
    if (json) {
        cJSON *results = cJSON_GetObjectItemCaseSensitive(json, "results");
        if (cJSON_IsArray(results)) {
            TT_ASSERT_GE_INT(cJSON_GetArraySize(results), 1);
            cJSON *r;
            cJSON_ArrayForEach(r, results) {
                cJSON *f = cJSON_GetObjectItemCaseSensitive(r, "f");
                if (cJSON_IsString(f))
                    TT_ASSERT_STR_CONTAINS(f->valuestring, "python");
            }
        }
        cJSON_Delete(json);
    }
}

/* ---------- search:cooccurrence ---------- */

TT_TEST(test_full_search_cooccurrence)
{
    full_ensure_index();
    if (!g_full_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "search:cooccurrence \"HandleGet,HandlePost\" --path %s", g_full_fixture);
    cJSON *json = NULL;
    int rc = tt_e2e_run(cmd, &json);

    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_NOT_NULL(json);
    if (json) {
        cJSON *n = cJSON_GetObjectItemCaseSensitive(json, "n");
        TT_ASSERT(cJSON_IsNumber(n) && n->valueint >= 1,
                   "should find >= 1 cooccurrence");

        cJSON *results = cJSON_GetObjectItemCaseSensitive(json, "results");
        if (cJSON_IsArray(results) && cJSON_GetArraySize(results) > 0) {
            cJSON *first = cJSON_GetArrayItem(results, 0);
            cJSON *f = cJSON_GetObjectItemCaseSensitive(first, "file");
            if (cJSON_IsString(f))
                TT_ASSERT_STR_CONTAINS(f->valuestring, "api.go");

            /* Verify both symbols are present in the result */
            cJSON *a = cJSON_GetObjectItemCaseSensitive(first, "a");
            cJSON *b = cJSON_GetObjectItemCaseSensitive(first, "b");
            TT_ASSERT_NOT_NULL(a);
            TT_ASSERT_NOT_NULL(b);
            if (a) {
                cJSON *an = cJSON_GetObjectItemCaseSensitive(a, "name");
                if (cJSON_IsString(an))
                    TT_ASSERT_EQ_STR("HandleGet", an->valuestring);
            }
            if (b) {
                cJSON *bn = cJSON_GetObjectItemCaseSensitive(b, "name");
                if (cJSON_IsString(bn))
                    TT_ASSERT_EQ_STR("HandlePost", bn->valuestring);
            }
        }
        cJSON_Delete(json);
    }
}

/* ---------- search:similar ---------- */

TT_TEST(test_full_search_similar)
{
    full_ensure_index();
    if (!g_full_indexed) return;

    /* Use process_data which has a matching prototype in engine.h */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "search:similar \"src/c/engine.c::process_data#function\" --path %s",
             g_full_fixture);
    cJSON *json = NULL;
    int rc = tt_e2e_run(cmd, &json);

    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_NOT_NULL(json);
    if (json) {
        cJSON *results = cJSON_GetObjectItemCaseSensitive(json, "results");
        TT_ASSERT(cJSON_IsArray(results), "should have results array");
        if (cJSON_IsArray(results)) {
            TT_ASSERT_GE_INT(cJSON_GetArraySize(results), 1);
            /* The header prototype should be in the results */
            int found_header = 0;
            cJSON *r;
            cJSON_ArrayForEach(r, results) {
                cJSON *f = cJSON_GetObjectItemCaseSensitive(r, "file");
                if (cJSON_IsString(f) && strstr(f->valuestring, "engine.h"))
                    found_header = 1;
            }
            TT_ASSERT(found_header,
                       "similar should find process_data prototype in engine.h");
        }
        cJSON_Delete(json);
    }
}

/* ---------- query length limit ---------- */

TT_TEST(test_full_search_symbols_query_too_long)
{
    full_ensure_index();
    if (!g_full_indexed) return;

    /* Build a 501-char query string */
    char long_q[502];
    memset(long_q, 'a', 501);
    long_q[501] = '\0';

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "search:symbols \"%s\" --path %s",
             long_q, g_full_fixture);
    cJSON *json = NULL;
    int rc = tt_e2e_run(cmd, &json);
    TT_ASSERT(rc != 0, "should return non-zero exit for too-long query");
    if (json) {
        TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(json, "error"));
        cJSON_Delete(json);
    }
}

TT_TEST(test_full_search_text_query_too_long)
{
    full_ensure_index();
    if (!g_full_indexed) return;

    char long_q[502];
    memset(long_q, 'b', 501);
    long_q[501] = '\0';

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "search:text \"%s\" --path %s",
             long_q, g_full_fixture);
    cJSON *json = NULL;
    int rc = tt_e2e_run(cmd, &json);
    TT_ASSERT(rc != 0, "should return non-zero exit for too-long query");
    if (json) {
        TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(json, "error"));
        cJSON_Delete(json);
    }
}

void run_e2e_full_search_tests(void)
{
    TT_RUN(test_full_search_symbols_cross_lang);
    TT_RUN(test_full_search_symbols_kind_filter);
    TT_RUN(test_full_search_symbols_count);
    TT_RUN(test_full_search_text);
    TT_RUN(test_full_search_text_group_by);
    TT_RUN(test_full_search_text_filter);
    TT_RUN(test_full_search_cooccurrence);
    TT_RUN(test_full_search_similar);
    TT_RUN(test_full_search_symbols_query_too_long);
    TT_RUN(test_full_search_text_query_too_long);
}
