/*
 * test_e2e_text_format.c -- E2E tests for --format text agent-line output.
 *
 * Covers search:symbols, search:text, inspect:symbol and inspect:file
 * rendered as grep-style lines from the c-project fixture.
 */

#include "test_framework.h"
#include "test_helpers.h"
#include "test_e2e_helpers.h"

#include <stdlib.h>
#include <string.h>

/* Shared state: fixture path + index-creation flag. */
static const char *g_c_fixture;
static int g_c_indexed;

static void c_ensure_index(void)
{
    if (g_c_indexed)
        return;

    g_c_fixture = tt_test_fixture("c-project");
    if (!g_c_fixture)
        return;

    char cmd[1024];

    snprintf(cmd, sizeof(cmd), "cache:clear --path %s", g_c_fixture);
    tt_e2e_run(cmd, NULL);

    snprintf(cmd, sizeof(cmd), "index:create --path %s", g_c_fixture);
    cJSON *json = NULL;
    int rc = tt_e2e_run(cmd, &json);
    g_c_indexed = (rc == 0);
    if (json)
        cJSON_Delete(json);
}

/* ---------- search:symbols ---------- */

TT_TEST(test_text_search_symbols_lines)
{
    c_ensure_index();
    if (!g_c_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "search:symbols widget -o text --path %s", g_c_fixture);

    char *out = NULL;
    int rc = tt_e2e_run_raw(cmd, &out);
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_NOT_NULL(out);
    if (!out) return;

    TT_ASSERT_STR_CONTAINS(out, "src/widget.c:17:function widget_draw\n");
    TT_ASSERT_STR_CONTAINS(out, "src/widget.h:5:class Widget\n");
    TT_ASSERT_STR_CONTAINS(out, "n=");
    TT_ASSERT_STR_NOT_CONTAINS(out, "{\"q\"");
    free(out);
}

/* The id is only printed when it is not derivable from file/kind/name. */
TT_TEST(test_text_search_symbols_id_only_when_needed)
{
    c_ensure_index();
    if (!g_c_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "search:symbols label -o text --path %s", g_c_fixture);

    char *out = NULL;
    tt_e2e_run_raw(cmd, &out);
    TT_ASSERT_NOT_NULL(out);
    if (!out) return;

    /* Qualified name differs from the bare name -> id must be shown. */
    TT_ASSERT_STR_CONTAINS(out, "id=src/widget.c::widget_draw.label#variable");
    free(out);
}

/* ---------- search:text ---------- */

TT_TEST(test_text_search_text_lines)
{
    c_ensure_index();
    if (!g_c_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "search:text clamp -o text --path %s", g_c_fixture);

    char *out = NULL;
    tt_e2e_run_raw(cmd, &out);
    TT_ASSERT_NOT_NULL(out);
    if (!out) return;

    TT_ASSERT_STR_CONTAINS(out, "src/widget.c:5:static int clamp(int v, int lo, int hi)\n");
    TT_ASSERT_STR_CONTAINS(out, "src/widget.c:22:");
    TT_ASSERT_STR_CONTAINS(out, "n=");
    free(out);
}

/* Context lines use the grep '-' separator with their real line numbers. */
TT_TEST(test_text_search_text_context_lines)
{
    c_ensure_index();
    if (!g_c_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "search:text clamp -C 1 -o text --path %s", g_c_fixture);

    char *out = NULL;
    tt_e2e_run_raw(cmd, &out);
    TT_ASSERT_NOT_NULL(out);
    if (!out) return;

    TT_ASSERT_STR_CONTAINS(out, "src/widget.c-4-// Internal helper.\n");
    TT_ASSERT_STR_CONTAINS(out, "src/widget.c:5:static int clamp(int v, int lo, int hi)\n");
    TT_ASSERT_STR_CONTAINS(out, "src/widget.c-6-{\n");
    free(out);
}

/* --count has no grep equivalent, so it keeps JSON output. */
TT_TEST(test_text_search_text_count_keeps_json)
{
    c_ensure_index();
    if (!g_c_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "search:text clamp -o text --count --path %s", g_c_fixture);

    cJSON *json = NULL;
    tt_e2e_run(cmd, &json);
    TT_ASSERT_NOT_NULL(json);
    if (json) {
        cJSON *count = cJSON_GetObjectItemCaseSensitive(json, "count");
        TT_ASSERT(cJSON_IsNumber(count), "count mode should stay JSON");
        cJSON_Delete(json);
    }
}

/* ---------- inspect:symbol ---------- */

TT_TEST(test_text_inspect_symbol_header)
{
    c_ensure_index();
    if (!g_c_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "inspect:symbol src/widget.c::widget_draw#function -o text --path %s",
             g_c_fixture);

    char *out = NULL;
    int rc = tt_e2e_run_raw(cmd, &out);
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_NOT_NULL(out);
    if (!out) return;

    TT_ASSERT_STR_CONTAINS(out, "// src/widget.c:17-25 function widget_draw\n");
    TT_ASSERT_STR_CONTAINS(out, "int len = clamp((int)strlen(label), 0, w->width);");
    TT_ASSERT_STR_NOT_CONTAINS(out, "\"source\"");
    free(out);
}

TT_TEST(test_text_inspect_symbol_not_found)
{
    c_ensure_index();
    if (!g_c_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "inspect:symbol src/widget.c::widget_draw#function nope::missing#function "
             "-o text --path %s", g_c_fixture);

    char *out = NULL;
    int rc = tt_e2e_run_raw(cmd, &out);
    TT_ASSERT_EQ_INT(2, rc);
    TT_ASSERT_NOT_NULL(out);
    if (!out) return;

    TT_ASSERT_STR_CONTAINS(out, "// not found: nope::missing#function\n");
    free(out);
}

/* ---------- inspect:file ---------- */

TT_TEST(test_text_inspect_file_numbered)
{
    c_ensure_index();
    if (!g_c_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "inspect:file src/widget.c --lines 17-20 -o text --path %s", g_c_fixture);

    char *out = NULL;
    tt_e2e_run_raw(cmd, &out);
    TT_ASSERT_NOT_NULL(out);
    if (!out) return;

    TT_ASSERT_STR_CONTAINS(out, "17: int widget_draw(Widget *w, const char *label)\n");
    TT_ASSERT_STR_CONTAINS(out, "20:     if (!w || !label)\n");
    TT_ASSERT_STR_NOT_CONTAINS(out, "\"content\"");
    free(out);
}

void run_e2e_text_format_tests(void)
{
    TT_RUN(test_text_search_symbols_lines);
    TT_RUN(test_text_search_symbols_id_only_when_needed);
    TT_RUN(test_text_search_text_lines);
    TT_RUN(test_text_search_text_context_lines);
    TT_RUN(test_text_search_text_count_keeps_json);
    TT_RUN(test_text_inspect_symbol_header);
    TT_RUN(test_text_inspect_symbol_not_found);
    TT_RUN(test_text_inspect_file_numbered);
}
