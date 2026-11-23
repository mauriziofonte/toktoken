/*
 * test_text_render.c -- Unit tests for the agent-line renderers.
 *
 * The renderers turn the JSON a command produces into grep-style lines.
 * These tests build that JSON by hand, so every line shape, option and
 * edge case is checked without indexing a project.
 */

#include "test_framework.h"
#include "text_render.h"
#include "cli.h"

#include <cJSON.h>
#include <stdlib.h>
#include <string.h>

/* ---- helpers: build the JSON shape each command emits ---- */

static cJSON *sym_result(cJSON **items, int n, int total)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++)
        cJSON_AddItemToArray(arr, items[i]);
    cJSON_AddItemToObject(root, "results", arr);
    cJSON_AddNumberToObject(root, "n", total >= 0 ? total : n);
    return root;
}

static cJSON *sym_item(const char *id, const char *file, int line,
                       const char *kind, const char *name)
{
    cJSON *o = cJSON_CreateObject();
    if (id)
        cJSON_AddStringToObject(o, "id", id);
    cJSON_AddStringToObject(o, "file", file);
    cJSON_AddNumberToObject(o, "line", line);
    cJSON_AddStringToObject(o, "kind", kind);
    cJSON_AddStringToObject(o, "name", name);
    return o;
}

static cJSON *text_hit(const char *file, int line, const char *text)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "f", file);
    cJSON_AddNumberToObject(o, "l", line);
    cJSON_AddStringToObject(o, "t", text);
    return o;
}

static cJSON *symbol_doc(const char *file, int line, int end, const char *kind,
                         const char *name, const char *source)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "file", file);
    cJSON_AddNumberToObject(item, "line", line);
    cJSON_AddNumberToObject(item, "end", end);
    cJSON_AddStringToObject(item, "kind", kind);
    cJSON_AddStringToObject(item, "name", name);
    cJSON_AddStringToObject(item, "source", source);
    cJSON_AddItemToArray(arr, item);
    cJSON_AddItemToObject(root, "symbols", arr);
    return root;
}

static void free_members(tt_member_t *m, int n)
{
    if (!m)
        return;
    for (int i = 0; i < n; i++)
        free(m[i].decl);
    free(m);
}

/* ---- tt_text_format_requested ---- */

TT_TEST(test_text_format_requested)
{
    tt_cli_opts_t opts = {0};
    TT_ASSERT(!tt_text_format_requested(&opts), "unset format is not text");

    opts.format = "json";
    TT_ASSERT(!tt_text_format_requested(&opts), "json is not text");

    opts.format = "text";
    TT_ASSERT(tt_text_format_requested(&opts), "text is text");

    TT_ASSERT(!tt_text_format_requested(NULL), "NULL opts is not text");
}

/* ---- search:symbols ---- */

TT_TEST(test_render_search_symbols_derivable_id_omitted)
{
    cJSON *items[] = {
        sym_item("src/a.c::do_thing#function", "src/a.c", 12, "function", "do_thing"),
    };
    cJSON *r = sym_result(items, 1, 1);

    char *out = tt_render_search_symbols_text(r);
    TT_ASSERT_NOT_NULL(out);
    if (out)
    {
        TT_ASSERT_STR_CONTAINS(out, "src/a.c:12:function do_thing\n");
        TT_ASSERT(!strstr(out, "id="), "derivable id is not printed");
        TT_ASSERT_STR_CONTAINS(out, "n=1\n");
        free(out);
    }
    cJSON_Delete(r);
}

TT_TEST(test_render_search_symbols_id_override)
{
    cJSON *items[] = {
        sym_item("src/a.c::do_thing#function~2", "src/a.c", 12, "function", "do_thing"),
    };
    cJSON *r = sym_result(items, 1, 1);

    char *out = tt_render_search_symbols_text(r);
    TT_ASSERT_NOT_NULL(out);
    if (out)
    {
        TT_ASSERT_STR_CONTAINS(out, "id=src/a.c::do_thing#function~2");
        free(out);
    }
    cJSON_Delete(r);
}

TT_TEST(test_render_search_symbols_not_found_and_total)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddItemToObject(r, "results", cJSON_CreateArray());
    cJSON_AddNumberToObject(r, "n", 9);
    cJSON *nf = cJSON_CreateArray();
    cJSON_AddItemToArray(nf, cJSON_CreateString("no_such_name"));
    cJSON_AddItemToObject(r, "not_found", nf);

    char *out = tt_render_search_symbols_text(r);
    TT_ASSERT_NOT_NULL(out);
    if (out)
    {
        TT_ASSERT_STR_CONTAINS(out, "// not found: no_such_name\n");
        TT_ASSERT_STR_CONTAINS(out, "n=9\n");
        free(out);
    }
    cJSON_Delete(r);
}

TT_TEST(test_render_search_symbols_rejects_missing_results)
{
    cJSON *r = cJSON_CreateObject();
    char *out = tt_render_search_symbols_text(r);
    TT_ASSERT(out == NULL, "no results array -> no text");
    cJSON_Delete(r);
}

/* ---- search:text ---- */

TT_TEST(test_render_search_text_single_hit)
{
    cJSON *items[] = { text_hit("src/a.c", 3, "int x = 1;") };
    cJSON *r = sym_result(items, 1, 1);

    char *out = tt_render_search_text_text(r, false);
    TT_ASSERT_NOT_NULL(out);
    if (out)
    {
        TT_ASSERT_STR_CONTAINS(out, "src/a.c:3:int x = 1;\n");
        TT_ASSERT_STR_CONTAINS(out, "n=1\n");
        free(out);
    }
    cJSON_Delete(r);
}

TT_TEST(test_render_search_text_groups_same_file)
{
    cJSON *items[] = {
        text_hit("src/a.c", 3, "first"),
        text_hit("src/a.c", 8, "second"),
        text_hit("src/b.c", 1, "only"),
    };
    cJSON *r = sym_result(items, 3, 3);

    char *out = tt_render_search_text_text(r, false);
    TT_ASSERT_NOT_NULL(out);
    if (out)
    {
        TT_ASSERT_STR_CONTAINS(out, "src/a.c:\n3:first\n8:second\n");
        TT_ASSERT_STR_CONTAINS(out, "src/b.c:1:only\n");
        free(out);
    }
    cJSON_Delete(r);
}

TT_TEST(test_render_search_text_context_and_no_comments)
{
    cJSON *hit = text_hit("src/a.c", 5, "int y;");
    cJSON *before = cJSON_CreateArray();
    cJSON_AddItemToArray(before, cJSON_CreateString("// note"));
    cJSON_AddItemToObject(hit, "before", before);

    cJSON *items[] = { hit };
    cJSON *r = sym_result(items, 1, 1);

    char *kept = tt_render_search_text_text(r, false);
    TT_ASSERT_NOT_NULL(kept);
    if (kept)
    {
        TT_ASSERT_STR_CONTAINS(kept, "src/a.c-4-// note\n");
        TT_ASSERT_STR_CONTAINS(kept, "src/a.c:5:int y;\n");
        free(kept);
    }

    char *trimmed = tt_render_search_text_text(r, true);
    TT_ASSERT_NOT_NULL(trimmed);
    if (trimmed)
    {
        TT_ASSERT_STR_CONTAINS(trimmed, "src/a.c:5:int y;\n");
        TT_ASSERT(!strstr(trimmed, "// note"), "comment context line is dropped");
        free(trimmed);
    }
    cJSON_Delete(r);
}

/* ---- inspect:file ---- */

TT_TEST(test_render_inspect_file_lines)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "file", "src/a.c");
    cJSON *range = cJSON_CreateArray();
    cJSON_AddItemToArray(range, cJSON_CreateNumber(4));
    cJSON_AddItemToArray(range, cJSON_CreateNumber(6));
    cJSON_AddItemToObject(root, "range", range);
    cJSON_AddStringToObject(root, "content", "int a;\n// gone\nint b;");

    char *out = tt_render_inspect_file_text(root, false);
    TT_ASSERT_NOT_NULL(out);
    if (out)
    {
        TT_ASSERT_STR_CONTAINS(out, "4: int a;\n5: // gone\n6: int b;\n");
        free(out);
    }

    char *trimmed = tt_render_inspect_file_text(root, true);
    TT_ASSERT_NOT_NULL(trimmed);
    if (trimmed)
    {
        TT_ASSERT_STR_CONTAINS(trimmed, "4: int a;\n6: int b;\n");
        TT_ASSERT(!strstr(trimmed, "// gone"), "comment line is dropped");
        free(trimmed);
    }
    cJSON_Delete(root);
}

/* ---- inspect:symbol ---- */

TT_TEST(test_render_inspect_symbol_source)
{
    cJSON *r = symbol_doc("src/a.c", 10, 13, "function", "f",
                          "int f(void)\n{\n    return 0;\n}");
    char *out = tt_render_inspect_symbol_text(r, false, NULL, false);
    TT_ASSERT_NOT_NULL(out);
    if (out)
    {
        TT_ASSERT_STR_CONTAINS(out, "// src/a.c:10-13 function f\n");
        TT_ASSERT_STR_CONTAINS(out, "int f(void)\n{\n    return 0;\n}\n");
        free(out);
    }
    cJSON_Delete(r);
}

TT_TEST(test_render_inspect_symbol_single_line_header)
{
    cJSON *r = symbol_doc("src/a.h", 7, 7, "function", "f", "int f(void);");
    char *out = tt_render_inspect_symbol_text(r, false, NULL, false);
    TT_ASSERT_NOT_NULL(out);
    if (out)
    {
        TT_ASSERT_STR_CONTAINS(out, "// src/a.h:7 function f\n");
        free(out);
    }
    cJSON_Delete(r);
}

TT_TEST(test_render_inspect_symbol_grep_keeps_real_line_numbers)
{
    cJSON *r = symbol_doc("src/a.c", 100, 103, "function", "f",
                          "int f(void)\n{\n    return 0;\n}");
    char *out = tt_render_inspect_symbol_text(r, false, "return", false);
    TT_ASSERT_NOT_NULL(out);
    if (out)
    {
        TT_ASSERT_STR_CONTAINS(out, "[grep: 1 of 4 lines]");
        TT_ASSERT_STR_CONTAINS(out, "102:     return 0;\n");
        TT_ASSERT(!strstr(out, "int f(void)"), "non-matching lines are dropped");
        free(out);
    }
    cJSON_Delete(r);
}

TT_TEST(test_render_inspect_symbol_grep_alternatives_case_insensitive)
{
    cJSON *r = symbol_doc("src/a.c", 1, 4, "function", "f",
                          "int f(void)\n{\n    return 0;\n}");
    char *out = tt_render_inspect_symbol_text(r, false, "RETURN|VOID", false);
    TT_ASSERT_NOT_NULL(out);
    if (out)
    {
        TT_ASSERT_STR_CONTAINS(out, "[grep: 2 of 4 lines]");
        free(out);
    }
    cJSON_Delete(r);
}

TT_TEST(test_render_inspect_symbol_not_found)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *errs = cJSON_CreateArray();
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "id", "src/a.c::missing#function");
    cJSON_AddItemToArray(errs, e);
    cJSON_AddItemToObject(root, "errors", errs);

    char *out = tt_render_inspect_symbol_text(root, false, NULL, false);
    TT_ASSERT_NOT_NULL(out);
    if (out)
    {
        TT_ASSERT_STR_CONTAINS(out, "// not found: src/a.c::missing#function\n");
        free(out);
    }
    cJSON_Delete(root);
}

TT_TEST(test_render_inspect_symbol_members_rows)
{
    cJSON *r = symbol_doc("src/a.h", 1, 4, "class", "P",
                          "struct P {\n    int x;\n    int y;\n};");
    char *out = tt_render_inspect_symbol_text(r, false, NULL, true);
    TT_ASSERT_NOT_NULL(out);
    if (out)
    {
        TT_ASSERT_STR_CONTAINS(out, "// src/a.h:1-4 class P [member: 2]\n");
        TT_ASSERT_STR_CONTAINS(out, "2: int x\n3: int y\n");
        free(out);
    }
    cJSON_Delete(r);
}

TT_TEST(test_render_inspect_symbol_members_skipped_for_function)
{
    cJSON *r = symbol_doc("src/a.c", 1, 3, "function", "f",
                          "int f(void)\n{\n    return 0;\n}");
    char *out = tt_render_inspect_symbol_text(r, false, NULL, true);
    TT_ASSERT_NOT_NULL(out);
    if (out)
    {
        TT_ASSERT(!strstr(out, "[member:"), "functions are not member-scanned");
        TT_ASSERT_STR_CONTAINS(out, "return 0;");
        free(out);
    }
    cJSON_Delete(r);
}

/* ---- tt_scan_members ---- */

TT_TEST(test_scan_members_comma_separated)
{
    const char *src = "struct P {\n    int x, y;\n};\n";
    int n = 0;
    tt_member_t *m = tt_scan_members(src, 1, true, &n);
    TT_ASSERT_EQ_INT(n, 2);
    if (m && n == 2)
    {
        TT_ASSERT_STR_CONTAINS(m[0].decl, "int x");
        TT_ASSERT_STR_CONTAINS(m[1].decl, "y");
        TT_ASSERT_EQ_INT(m[0].line, 2);
        TT_ASSERT_EQ_INT(m[1].line, 2);
    }
    free_members(m, n);
}

TT_TEST(test_scan_members_multiline_joined_and_nested_collapsed)
{
    const char *src =
        "struct C {\n"
        "    int (*cb)(\n"
        "        int a);\n"
        "    struct { int a; } inner;\n"
        "};\n";
    int n = 0;
    tt_member_t *m = tt_scan_members(src, 10, false, &n);
    TT_ASSERT_EQ_INT(n, 2);
    if (m && n == 2)
    {
        TT_ASSERT_STR_CONTAINS(m[0].decl, "int (*cb)(");
        TT_ASSERT_STR_CONTAINS(m[0].decl, "int a)");
        TT_ASSERT_EQ_INT(m[0].line, 11);
        TT_ASSERT_STR_CONTAINS(m[1].decl, "{...}");
        TT_ASSERT(!strstr(m[1].decl, "int a; }"), "nested body is collapsed");
        TT_ASSERT_EQ_INT(m[1].line, 13);
    }
    free_members(m, n);
}

TT_TEST(test_scan_members_skips_comments_and_preprocessor)
{
    const char *src =
        "enum E {\n"
        "  // a comment\n"
        "  /* block */\n"
        "#ifdef X\n"
        "  A,\n"
        "#endif\n"
        "  B\n"
        "};\n";
    int n = 0;
    tt_member_t *m = tt_scan_members(src, 1, true, &n);
    TT_ASSERT_EQ_INT(n, 2);
    if (m && n == 2)
    {
        TT_ASSERT_STR_CONTAINS(m[0].decl, "A");
        TT_ASSERT_EQ_INT(m[0].line, 5);
        TT_ASSERT_STR_CONTAINS(m[1].decl, "B");
        TT_ASSERT_EQ_INT(m[1].line, 7);
    }
    free_members(m, n);
}

TT_TEST(test_scan_members_skips_access_specifiers)
{
    const char *src =
        "class K {\n"
        "public:\n"
        "    int v;\n"
        "private:\n"
        "    void reset() {}\n"
        "};\n";
    int n = 0;
    tt_member_t *m = tt_scan_members(src, 1, false, &n);
    TT_ASSERT_EQ_INT(n, 2);
    if (m && n == 2)
    {
        TT_ASSERT_STR_CONTAINS(m[0].decl, "int v");
        TT_ASSERT(!strstr(m[0].decl, "public"), "section label is not part of a member");
        TT_ASSERT_STR_CONTAINS(m[1].decl, "void reset() {...}");
        TT_ASSERT(!strstr(m[1].decl, "private"), "section label is not part of a member");
    }
    free_members(m, n);
}

TT_TEST(test_scan_members_bitfield_keeps_colon)
{
    const char *src = "struct B {\n    unsigned f : 3;\n};\n";
    int n = 0;
    tt_member_t *m = tt_scan_members(src, 1, false, &n);
    TT_ASSERT_EQ_INT(n, 1);
    if (m && n == 1)
        TT_ASSERT_STR_CONTAINS(m[0].decl, "f : 3");
    free_members(m, n);
}

TT_TEST(test_scan_members_empty_source)
{
    int n = -1;
    tt_member_t *m = tt_scan_members("", 1, false, &n);
    TT_ASSERT_EQ_INT(n, 0);
    free_members(m, n);
}

/* ---- runner ---- */

void run_text_render_tests(void)
{
    TT_SUITE("Text Render");

    TT_RUN(test_text_format_requested);
    TT_RUN(test_render_search_symbols_derivable_id_omitted);
    TT_RUN(test_render_search_symbols_id_override);
    TT_RUN(test_render_search_symbols_not_found_and_total);
    TT_RUN(test_render_search_symbols_rejects_missing_results);
    TT_RUN(test_render_search_text_single_hit);
    TT_RUN(test_render_search_text_groups_same_file);
    TT_RUN(test_render_search_text_context_and_no_comments);
    TT_RUN(test_render_inspect_file_lines);
    TT_RUN(test_render_inspect_symbol_source);
    TT_RUN(test_render_inspect_symbol_single_line_header);
    TT_RUN(test_render_inspect_symbol_grep_keeps_real_line_numbers);
    TT_RUN(test_render_inspect_symbol_grep_alternatives_case_insensitive);
    TT_RUN(test_render_inspect_symbol_not_found);
    TT_RUN(test_render_inspect_symbol_members_rows);
    TT_RUN(test_render_inspect_symbol_members_skipped_for_function);
    TT_RUN(test_scan_members_comma_separated);
    TT_RUN(test_scan_members_multiline_joined_and_nested_collapsed);
    TT_RUN(test_scan_members_skips_comments_and_preprocessor);
    TT_RUN(test_scan_members_skips_access_specifiers);
    TT_RUN(test_scan_members_bitfield_keeps_colon);
    TT_RUN(test_scan_members_empty_source);
}
