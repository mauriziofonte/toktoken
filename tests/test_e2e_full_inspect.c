/*
 * test_e2e_full_inspect.c -- E2E tests for inspect:* tools on full-project fixture.
 */

#include "test_e2e_full_helpers.h"

/* ---------- inspect:outline ---------- */

TT_TEST(test_full_inspect_outline_php)
{
    full_ensure_index();
    if (!g_full_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "inspect:outline \"src/php/Controller.php\" --path %s", g_full_fixture);
    cJSON *json = NULL;
    int rc = tt_e2e_run(cmd, &json);

    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_NOT_NULL(json);
    if (json) {
        cJSON *symbols = cJSON_GetObjectItemCaseSensitive(json, "symbols");
        TT_ASSERT(cJSON_IsArray(symbols), "should have symbols array");
        if (cJSON_IsArray(symbols))
            TT_ASSERT_GE_INT(cJSON_GetArraySize(symbols), 1);
        cJSON_Delete(json);
    }
}

TT_TEST(test_full_inspect_outline_lang_field)
{
    full_ensure_index();
    if (!g_full_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "inspect:outline \"src/python/analyzer.py\" --path %s", g_full_fixture);
    cJSON *json = NULL;
    int rc = tt_e2e_run(cmd, &json);

    TT_ASSERT_EQ_INT(0, rc);
    if (json) {
        cJSON *lang = cJSON_GetObjectItemCaseSensitive(json, "lang");
        if (cJSON_IsString(lang))
            TT_ASSERT_EQ_STR("python", lang->valuestring);
        cJSON_Delete(json);
    }
}

/* ---------- inspect:symbol ---------- */

TT_TEST(test_full_inspect_symbol)
{
    full_ensure_index();
    if (!g_full_indexed) return;

    cJSON *search = full_search_symbol("Analyzer", "");
    const char *id = full_first_symbol_id(search);
    if (!id) {
        if (search) cJSON_Delete(search);
        TT_ASSERT(0, "could not find Analyzer symbol");
        return;
    }

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "inspect:symbol \"%s\" --path %s", id, g_full_fixture);
    cJSON_Delete(search);

    cJSON *json = NULL;
    int rc = tt_e2e_run(cmd, &json);

    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_NOT_NULL(json);
    if (json) {
        cJSON *name = cJSON_GetObjectItemCaseSensitive(json, "name");
        if (cJSON_IsString(name))
            TT_ASSERT_EQ_STR("Analyzer", name->valuestring);

        cJSON *source = cJSON_GetObjectItemCaseSensitive(json, "source");
        if (cJSON_IsString(source))
            TT_ASSERT_STR_CONTAINS(source->valuestring, "class");

        cJSON *file = cJSON_GetObjectItemCaseSensitive(json, "file");
        if (cJSON_IsString(file))
            TT_ASSERT_STR_CONTAINS(file->valuestring, "analyzer.py");

        cJSON_Delete(json);
    }
}

/* ---------- inspect:file ---------- */

TT_TEST(test_full_inspect_file)
{
    full_ensure_index();
    if (!g_full_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "inspect:file \"src/ts/app.ts\" --path %s", g_full_fixture);
    cJSON *json = NULL;
    int rc = tt_e2e_run(cmd, &json);

    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_NOT_NULL(json);
    if (json) {
        cJSON *total = cJSON_GetObjectItemCaseSensitive(json, "total_lines");
        if (cJSON_IsNumber(total))
            TT_ASSERT_GE_INT(total->valueint, 30);

        cJSON *content = cJSON_GetObjectItemCaseSensitive(json, "content");
        if (cJSON_IsString(content))
            TT_ASSERT_STR_CONTAINS(content->valuestring, "import");

        cJSON_Delete(json);
    }
}

TT_TEST(test_full_inspect_file_lines)
{
    full_ensure_index();
    if (!g_full_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "inspect:file \"src/ts/app.ts\" --lines 1-5 --path %s", g_full_fixture);
    cJSON *json = NULL;
    int rc = tt_e2e_run(cmd, &json);

    TT_ASSERT_EQ_INT(0, rc);
    if (json) {
        cJSON *content = cJSON_GetObjectItemCaseSensitive(json, "content");
        TT_ASSERT(cJSON_IsString(content), "should have content");
        cJSON_Delete(json);
    }
}

/* ---------- inspect:tree ---------- */

TT_TEST(test_full_inspect_tree)
{
    full_ensure_index();
    if (!g_full_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "inspect:tree --path %s", g_full_fixture);
    cJSON *json = NULL;
    int rc = tt_e2e_run(cmd, &json);

    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_NOT_NULL(json);
    if (json) {
        cJSON *files = cJSON_GetObjectItemCaseSensitive(json, "files");
        if (cJSON_IsNumber(files))
            TT_ASSERT_GE_INT(files->valueint, 40);
        cJSON_Delete(json);
    }
}

/* ---------- inspect:bundle ---------- */

TT_TEST(test_full_inspect_bundle)
{
    full_ensure_index();
    if (!g_full_indexed) return;

    cJSON *search = full_search_symbol("Analyzer", "");
    const char *id = full_first_symbol_id(search);
    if (!id) {
        if (search) cJSON_Delete(search);
        TT_ASSERT(0, "could not find Analyzer symbol for bundle test");
        return;
    }

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "inspect:bundle \"%s\" --path %s", id, g_full_fixture);
    cJSON_Delete(search);

    cJSON *json = NULL;
    int rc = tt_e2e_run(cmd, &json);

    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_NOT_NULL(json);
    if (json) {
        cJSON *def = cJSON_GetObjectItemCaseSensitive(json, "definition");
        TT_ASSERT_NOT_NULL(def);
        if (def) {
            cJSON *name = cJSON_GetObjectItemCaseSensitive(def, "name");
            if (cJSON_IsString(name))
                TT_ASSERT_EQ_STR("Analyzer", name->valuestring);
        }

        cJSON *outline = cJSON_GetObjectItemCaseSensitive(json, "outline");
        TT_ASSERT(cJSON_IsArray(outline), "bundle should have outline");
        if (cJSON_IsArray(outline))
            TT_ASSERT_GE_INT(cJSON_GetArraySize(outline), 1);

        cJSON_Delete(json);
    }
}

/* ---------- inspect:dependencies ---------- */

TT_TEST(test_full_inspect_dependencies_ts)
{
    full_ensure_index();
    if (!g_full_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "inspect:dependencies \"src/ts/types.ts\" --depth 3 --path %s",
             g_full_fixture);
    cJSON *json = NULL;
    int rc = tt_e2e_run(cmd, &json);

    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_NOT_NULL(json);
    if (json) {
        cJSON *dependents = cJSON_GetObjectItemCaseSensitive(json, "dependents");
        if (cJSON_IsArray(dependents))
            TT_ASSERT_GE_INT(cJSON_GetArraySize(dependents), 1);
        cJSON_Delete(json);
    }
}

TT_TEST(test_full_inspect_dependencies_js)
{
    full_ensure_index();
    if (!g_full_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "inspect:dependencies \"src/js/middleware.js\" --depth 3 --path %s",
             g_full_fixture);
    cJSON *json = NULL;
    int rc = tt_e2e_run(cmd, &json);

    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_NOT_NULL(json);
    if (json) {
        cJSON *dependents = cJSON_GetObjectItemCaseSensitive(json, "dependents");
        if (cJSON_IsArray(dependents))
            TT_ASSERT_GE_INT(cJSON_GetArraySize(dependents), 1);
        cJSON_Delete(json);
    }
}

/* ---------- inspect:hierarchy ---------- */

TT_TEST(test_full_inspect_hierarchy)
{
    full_ensure_index();
    if (!g_full_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "inspect:hierarchy \"src/php/Controller.php\" --path %s", g_full_fixture);
    cJSON *json = NULL;
    int rc = tt_e2e_run(cmd, &json);

    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_NOT_NULL(json);
    if (json) {
        cJSON *nodes = cJSON_GetObjectItemCaseSensitive(json, "nodes");
        if (cJSON_IsArray(nodes))
            TT_ASSERT_GE_INT(cJSON_GetArraySize(nodes), 1);
        cJSON_Delete(json);
    }
}

/* ---------- inspect:cycles ---------- */

TT_TEST(test_full_inspect_cycles)
{
    full_ensure_index();
    if (!g_full_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "inspect:cycles --path %s", g_full_fixture);
    cJSON *json = NULL;
    int rc = tt_e2e_run(cmd, &json);

    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_NOT_NULL(json);
    if (json) {
        cJSON *summary = cJSON_GetObjectItemCaseSensitive(json, "summary");
        cJSON *total = summary
            ? cJSON_GetObjectItemCaseSensitive(summary, "total_cycles")
            : NULL;
        TT_ASSERT(cJSON_IsNumber(total) && total->valueint >= 1,
                   "should find >= 1 cycle (cycle_a <-> cycle_b)");

        cJSON *cycles = cJSON_GetObjectItemCaseSensitive(json, "cycles");
        if (cJSON_IsArray(cycles) && cJSON_GetArraySize(cycles) > 0) {
            cJSON *first = cJSON_GetArrayItem(cycles, 0);
            cJSON *length = cJSON_GetObjectItemCaseSensitive(first, "length");
            if (cJSON_IsNumber(length))
                TT_ASSERT_EQ_INT(2, length->valueint);

            /* Verify cycle involves cycle_a and cycle_b */
            cJSON *files_arr = cJSON_GetObjectItemCaseSensitive(first, "files");
            if (cJSON_IsArray(files_arr)) {
                char combined[2048] = "";
                cJSON *f;
                cJSON_ArrayForEach(f, files_arr) {
                    if (cJSON_IsString(f)) {
                        strcat(combined, f->valuestring);
                        strcat(combined, " ");
                    }
                }
                TT_ASSERT_STR_CONTAINS(combined, "cycle_a");
                TT_ASSERT_STR_CONTAINS(combined, "cycle_b");
            }
        }
        cJSON_Delete(json);
    }
}

/* ---------- inspect:blast ---------- */

TT_TEST(test_full_inspect_blast)
{
    full_ensure_index();
    if (!g_full_indexed) return;

    /* Use Config interface from types.ts directly — it's imported by
     * service.ts (depth 1) and transitively by app.ts (depth 2). */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "inspect:blast \"src/ts/types.ts::Config#interface\" --path %s",
             g_full_fixture);

    cJSON *json = NULL;
    int rc = tt_e2e_run(cmd, &json);

    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_NOT_NULL(json);
    if (json) {
        cJSON *target = cJSON_GetObjectItemCaseSensitive(json, "target");
        TT_ASSERT_NOT_NULL(target);
        if (target) {
            cJSON *name = cJSON_GetObjectItemCaseSensitive(target, "name");
            if (cJSON_IsString(name))
                TT_ASSERT_EQ_STR("Config", name->valuestring);
        }

        /* Verify confirmed blast radius includes service.ts */
        cJSON *confirmed = cJSON_GetObjectItemCaseSensitive(json, "confirmed");
        TT_ASSERT(cJSON_IsArray(confirmed), "should have confirmed array");
        if (cJSON_IsArray(confirmed)) {
            TT_ASSERT_GE_INT(cJSON_GetArraySize(confirmed), 1);
            int found_service = 0;
            cJSON *c;
            cJSON_ArrayForEach(c, confirmed) {
                cJSON *f = cJSON_GetObjectItemCaseSensitive(c, "file");
                if (cJSON_IsString(f) && strstr(f->valuestring, "service.ts"))
                    found_service = 1;
            }
            TT_ASSERT(found_service,
                       "blast radius should include service.ts");
        }

        cJSON *summary = cJSON_GetObjectItemCaseSensitive(json, "summary");
        TT_ASSERT_NOT_NULL(summary);
        if (summary) {
            cJSON *cc = cJSON_GetObjectItemCaseSensitive(summary,
                                                          "confirmed_count");
            if (cJSON_IsNumber(cc))
                TT_ASSERT_GE_INT(cc->valueint, 1);
            cJSON *md = cJSON_GetObjectItemCaseSensitive(summary, "max_depth");
            if (cJSON_IsNumber(md))
                TT_ASSERT_GE_INT(md->valueint, 1);
        }

        cJSON_Delete(json);
    }
}

void run_e2e_full_inspect_tests(void)
{
    TT_RUN(test_full_inspect_outline_php);
    TT_RUN(test_full_inspect_outline_lang_field);
    TT_RUN(test_full_inspect_symbol);
    TT_RUN(test_full_inspect_file);
    TT_RUN(test_full_inspect_file_lines);
    TT_RUN(test_full_inspect_tree);
    TT_RUN(test_full_inspect_bundle);
    TT_RUN(test_full_inspect_dependencies_ts);
    TT_RUN(test_full_inspect_dependencies_js);
    TT_RUN(test_full_inspect_hierarchy);
    TT_RUN(test_full_inspect_cycles);
    TT_RUN(test_full_inspect_blast);
}
