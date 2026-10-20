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

/* ---------- --no-comments ---------- */

/* Blank and comment-only lines go away, real line numbers stay. */
TT_TEST(test_text_inspect_file_no_comments)
{
    c_ensure_index();
    if (!g_c_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "inspect:file src/widget.c --lines 14-25 -o text --no-comments --path %s",
             g_c_fixture);

    char *out = NULL;
    tt_e2e_run_raw(cmd, &out);
    TT_ASSERT_NOT_NULL(out);
    if (!out) return;

    TT_ASSERT_STR_CONTAINS(out, "17: int widget_draw(Widget *w, const char *label)\n");
    TT_ASSERT_STR_CONTAINS(out, "22:     int len = clamp((int)strlen(label), 0, w->width);\n");
    TT_ASSERT_STR_NOT_CONTAINS(out, "14:"); /* block comment start */
    TT_ASSERT_STR_NOT_CONTAINS(out, "19:"); /* inline comment */
    free(out);
}

TT_TEST(test_text_inspect_symbol_no_comments)
{
    c_ensure_index();
    if (!g_c_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "inspect:symbol src/widget.c::widget_draw#function -o text --no-comments --path %s",
             g_c_fixture);

    char *out = NULL;
    tt_e2e_run_raw(cmd, &out);
    TT_ASSERT_NOT_NULL(out);
    if (!out) return;

    TT_ASSERT_STR_CONTAINS(out, "// src/widget.c:17-25 function widget_draw\n");
    TT_ASSERT_STR_NOT_CONTAINS(out, "nothing to draw");
    TT_ASSERT_STR_CONTAINS(out, "return len;");
    free(out);
}

/* Comment and blank context lines are dropped; hit lines are kept. */
TT_TEST(test_text_search_text_no_comments_context)
{
    c_ensure_index();
    if (!g_c_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "search:text clamp -C 3 -o text --no-comments --path %s", g_c_fixture);

    char *out = NULL;
    tt_e2e_run_raw(cmd, &out);
    TT_ASSERT_NOT_NULL(out);
    if (!out) return;

    TT_ASSERT_STR_CONTAINS(out, "src/widget.c:5:static int clamp(int v, int lo, int hi)\n");
    TT_ASSERT_STR_NOT_CONTAINS(out, "-4-// Internal helper.");
    TT_ASSERT_STR_CONTAINS(out, "src/widget.c-2-#include <string.h>\n");
    free(out);
}

/* ---------- --defs: exact-name definitions ---------- */

/* Exact names only: members, parameters and header prototypes are dropped. */
TT_TEST(test_defs_exact_definitions_only)
{
    c_ensure_index();
    if (!g_c_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "search:symbols widget_draw -o text --defs --path %s", g_c_fixture);

    char *out = NULL;
    tt_e2e_run_raw(cmd, &out);
    TT_ASSERT_NOT_NULL(out);
    if (!out) return;

    TT_ASSERT_STR_CONTAINS(out, "src/widget.c:17:function widget_draw\n");
    TT_ASSERT_STR_NOT_CONTAINS(out, "src/widget.h:18:"); /* header prototype */
    TT_ASSERT_STR_NOT_CONTAINS(out, "variable label");   /* parameter noise */
    TT_ASSERT_STR_CONTAINS(out, "n=1\n");
    free(out);
}

/* Several names per call, with misses listed in not_found. */
TT_TEST(test_defs_several_names_and_not_found)
{
    c_ensure_index();
    if (!g_c_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "search:symbols widget_draw Widget nope --defs --path %s", g_c_fixture);

    cJSON *json = NULL;
    tt_e2e_run(cmd, &json);
    TT_ASSERT_NOT_NULL(json);
    if (!json) return;

    cJSON *n = cJSON_GetObjectItemCaseSensitive(json, "n");
    TT_ASSERT(cJSON_IsNumber(n) && n->valueint == 3, "n should count the three definitions");

    cJSON *results = cJSON_GetObjectItemCaseSensitive(json, "results");
    TT_ASSERT(cJSON_IsArray(results), "results should be an array");
    if (cJSON_IsArray(results)) {
        cJSON *r;
        cJSON_ArrayForEach(r, results) {
            cJSON *name = cJSON_GetObjectItemCaseSensitive(r, "name");
            const char *v = cJSON_IsString(name) ? name->valuestring : "";
            TT_ASSERT(strcmp(v, "widget_draw") == 0 || strcmp(v, "Widget") == 0,
                       "every result must match a requested name exactly");
        }
    }

    cJSON *nf = cJSON_GetObjectItemCaseSensitive(json, "not_found");
    TT_ASSERT(cJSON_IsArray(nf) && cJSON_GetArraySize(nf) == 1, "nope should be not_found");
    if (cJSON_IsArray(nf)) {
        cJSON *first = cJSON_GetArrayItem(nf, 0);
        TT_ASSERT(cJSON_IsString(first) && strcmp(first->valuestring, "nope") == 0,
                   "not_found should name the missing symbol");
    }
    cJSON_Delete(json);
}

/* A type that only exists in a header is still a definition, so it stays. */
TT_TEST(test_defs_keeps_header_only_definition)
{
    c_ensure_index();
    if (!g_c_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "search:symbols Widget -o text --defs --path %s", g_c_fixture);

    char *out = NULL;
    tt_e2e_run_raw(cmd, &out);
    TT_ASSERT_NOT_NULL(out);
    if (!out) return;

    TT_ASSERT_STR_CONTAINS(out, "src/widget.h:5:class Widget\n");
    TT_ASSERT_STR_NOT_CONTAINS(out, "method draw"); /* struct member */
    free(out);
}

/* --defs without a name is a usage error. */
TT_TEST(test_defs_requires_a_name)
{
    c_ensure_index();
    if (!g_c_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "search:symbols --defs --path %s", g_c_fixture);

    cJSON *json = NULL;
    int rc = tt_e2e_run(cmd, &json);
    TT_ASSERT(rc != 0, "--defs without a name should fail");
    TT_ASSERT_NOT_NULL(json);
    if (json) {
        cJSON *err = cJSON_GetObjectItemCaseSensitive(json, "error");
        TT_ASSERT(cJSON_IsString(err), "should report an error code");
        cJSON_Delete(json);
    }
}

/* ---------- --grep: keep only matching body lines ---------- */

TT_TEST(test_text_inspect_symbol_grep)
{
    c_ensure_index();
    if (!g_c_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "inspect:symbol src/widget.c::widget_draw#function -o text --grep clamp --path %s",
             g_c_fixture);

    char *out = NULL;
    tt_e2e_run_raw(cmd, &out);
    TT_ASSERT_NOT_NULL(out);
    if (!out) return;

    TT_ASSERT_STR_CONTAINS(out, "[grep: 1 of 9 lines]");
    TT_ASSERT_STR_CONTAINS(out, "22:     int len = clamp((int)strlen(label), 0, w->width);\n");
    TT_ASSERT_STR_NOT_CONTAINS(out, "return len;");
    free(out);
}

TT_TEST(test_text_inspect_symbol_grep_alternatives)
{
    c_ensure_index();
    if (!g_c_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "inspect:symbol src/widget.c::widget_draw#function -o text "
             "--grep \"clamp|memcpy\" --path %s", g_c_fixture);

    char *out = NULL;
    tt_e2e_run_raw(cmd, &out);
    TT_ASSERT_NOT_NULL(out);
    if (!out) return;

    TT_ASSERT_STR_CONTAINS(out, "[grep: 2 of 9 lines]");
    TT_ASSERT_STR_CONTAINS(out, "23:     memcpy(w->name, label, len);\n");
    free(out);
}

/* A pattern that matches nothing still reports the symbol it looked at. */
TT_TEST(test_text_inspect_symbol_grep_no_match)
{
    c_ensure_index();
    if (!g_c_indexed) return;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "inspect:symbol src/widget.c::widget_draw#function -o text --grep zzz --path %s",
             g_c_fixture);

    char *out = NULL;
    tt_e2e_run_raw(cmd, &out);
    TT_ASSERT_NOT_NULL(out);
    if (!out) return;

    TT_ASSERT_STR_CONTAINS(out, "// src/widget.c:17-25 function widget_draw [grep: 0 of 9 lines]\n");
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
    TT_RUN(test_text_inspect_file_no_comments);
    TT_RUN(test_text_inspect_symbol_no_comments);
    TT_RUN(test_text_search_text_no_comments_context);
    TT_RUN(test_defs_exact_definitions_only);
    TT_RUN(test_defs_several_names_and_not_found);
    TT_RUN(test_defs_keeps_header_only_definition);
    TT_RUN(test_defs_requires_a_name);
    TT_RUN(test_text_inspect_symbol_grep);
    TT_RUN(test_text_inspect_symbol_grep_alternatives);
    TT_RUN(test_text_inspect_symbol_grep_no_match);
}
