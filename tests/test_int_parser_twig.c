/*
 * test_int_parser_twig.c -- Integration tests for parser_twig module.
 */

#include "test_framework.h"
#include "test_helpers.h"
#include "parser_twig.h"
#include "symbol.h"
#include "symbol_kind.h"

#include <stdlib.h>
#include <string.h>

static int sym_has_name(const tt_symbol_t *syms, int count, const char *name)
{
    for (int i = 0; i < count; i++) {
        if (syms[i].name && strcmp(syms[i].name, name) == 0)
            return 1;
    }
    return 0;
}

static const tt_symbol_t *sym_find_qname(const tt_symbol_t *syms, int count,
                                          const char *qname)
{
    for (int i = 0; i < count; i++) {
        if (syms[i].qualified_name && strcmp(syms[i].qualified_name, qname) == 0)
            return &syms[i];
    }
    return NULL;
}

static int sym_count_kind(const tt_symbol_t *syms, int count,
                          tt_symbol_kind_e kind)
{
    int n = 0;
    for (int i = 0; i < count; i++)
        if (syms[i].kind == kind)
            n++;
    return n;
}

#define TWIG_ROOT(buf) do { \
    const char *f = tt_test_fixtures_dir(); \
    if (!f) return; \
    snprintf(buf, sizeof(buf), "%s/twig-project", f); \
} while (0)

/* ---- Dashboard fixture: tests multiple construct types ---- */

TT_TEST(test_twig_extracts_directives)
{
    char root[512]; TWIG_ROOT(root);
    const char *files[] = {"templates/pages/dashboard.html.twig"};
    tt_symbol_t *syms = NULL; int count = 0;

    int rc = tt_parse_twig(root, files, 1, &syms, &count);
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT(count > 0, "should extract symbols from dashboard");
    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "extends.base.html.twig"));

    tt_symbol_array_free(syms, count);
}

TT_TEST(test_twig_kind_directive)
{
    char root[512]; TWIG_ROOT(root);
    const char *files[] = {"templates/pages/dashboard.html.twig"};
    tt_symbol_t *syms = NULL; int count = 0;

    tt_parse_twig(root, files, 1, &syms, &count);

    /* Template refs and blocks should be TT_KIND_DIRECTIVE */
    const tt_symbol_t *s = sym_find_qname(syms, count, "extends.base.html.twig");
    TT_ASSERT_NOT_NULL(s);
    if (s) TT_ASSERT_EQ_INT(TT_KIND_DIRECTIVE, s->kind);

    s = sym_find_qname(syms, count, "block.title");
    TT_ASSERT_NOT_NULL(s);
    if (s) TT_ASSERT_EQ_INT(TT_KIND_DIRECTIVE, s->kind);

    tt_symbol_array_free(syms, count);
}

TT_TEST(test_twig_language_is_twig)
{
    char root[512]; TWIG_ROOT(root);
    const char *files[] = {"templates/pages/dashboard.html.twig"};
    tt_symbol_t *syms = NULL; int count = 0;

    tt_parse_twig(root, files, 1, &syms, &count);

    for (int i = 0; i < count; i++) {
        TT_ASSERT_EQ_STR("twig", syms[i].language);
    }

    tt_symbol_array_free(syms, count);
}

TT_TEST(test_twig_id_format)
{
    char root[512]; TWIG_ROOT(root);
    const char *files[] = {"templates/pages/dashboard.html.twig"};
    tt_symbol_t *syms = NULL; int count = 0;

    tt_parse_twig(root, files, 1, &syms, &count);

    for (int i = 0; i < count; i++) {
        TT_ASSERT(strncmp(syms[i].id, "templates/pages/dashboard.html.twig::", 37) == 0,
                   "ID should start with file path");
        TT_ASSERT(strstr(syms[i].id, "#") != NULL,
                   "ID should contain #kind");
    }

    tt_symbol_array_free(syms, count);
}

TT_TEST(test_twig_empty_file)
{
    char root[512]; TWIG_ROOT(root);
    const char *files[] = {"templates/empty.html.twig"};
    tt_symbol_t *syms = NULL; int count = 0;

    int rc = tt_parse_twig(root, files, 1, &syms, &count);
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_INT(0, count);

    tt_symbol_array_free(syms, count);
}

/* ---- Base fixture: blocks + HTML IDs ---- */

TT_TEST(test_twig_blocks)
{
    char root[512]; TWIG_ROOT(root);
    const char *files[] = {"templates/base.html.twig"};
    tt_symbol_t *syms = NULL; int count = 0;

    tt_parse_twig(root, files, 1, &syms, &count);

    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "block.title"));
    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "block.navigation"));
    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "block.content"));
    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "block.footer"));

    tt_symbol_array_free(syms, count);
}

TT_TEST(test_twig_includes)
{
    char root[512]; TWIG_ROOT(root);
    const char *files[] = {"templates/pages/dashboard.html.twig"};
    tt_symbol_t *syms = NULL; int count = 0;

    tt_parse_twig(root, files, 1, &syms, &count);

    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "include.components/header.html.twig"));
    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "include.components/sidebar.html.twig"));

    tt_symbol_array_free(syms, count);
}

TT_TEST(test_twig_extends)
{
    char root[512]; TWIG_ROOT(root);
    const char *files[] = {"templates/pages/dashboard.html.twig"};
    tt_symbol_t *syms = NULL; int count = 0;

    tt_parse_twig(root, files, 1, &syms, &count);

    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "extends.base.html.twig"));

    tt_symbol_array_free(syms, count);
}

TT_TEST(test_twig_embed)
{
    char root[512]; TWIG_ROOT(root);
    const char *files[] = {"templates/pages/dashboard.html.twig"};
    tt_symbol_t *syms = NULL; int count = 0;

    tt_parse_twig(root, files, 1, &syms, &count);

    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "embed.blocks/card.html.twig"));

    tt_symbol_array_free(syms, count);
}

/* ---- Macros fixture ---- */

TT_TEST(test_twig_macros)
{
    char root[512]; TWIG_ROOT(root);
    const char *files[] = {"templates/macros/forms.html.twig"};
    tt_symbol_t *syms = NULL; int count = 0;

    tt_parse_twig(root, files, 1, &syms, &count);

    int method_count = sym_count_kind(syms, count, TT_KIND_METHOD);
    TT_ASSERT_EQ_INT(3, method_count);

    TT_ASSERT(sym_has_name(syms, count, "input"), "should find macro input");
    TT_ASSERT(sym_has_name(syms, count, "textarea"), "should find macro textarea");
    TT_ASSERT(sym_has_name(syms, count, "select"), "should find macro select");

    /* Signatures should contain '(' */
    for (int i = 0; i < count; i++) {
        if (syms[i].kind == TT_KIND_METHOD) {
            TT_ASSERT(strchr(syms[i].signature, '(') != NULL,
                       "macro signature should contain '('");
        }
    }

    tt_symbol_array_free(syms, count);
}

TT_TEST(test_twig_macro_whitespace_mods)
{
    char root[512]; TWIG_ROOT(root);
    const char *files[] = {"templates/macros/forms.html.twig"};
    tt_symbol_t *syms = NULL; int count = 0;

    tt_parse_twig(root, files, 1, &syms, &count);

    /* {%- macro input -%} should still be extracted */
    TT_ASSERT(sym_has_name(syms, count, "input"),
              "should extract macro from {%- ... -%} tag");

    tt_symbol_array_free(syms, count);
}

/* ---- Set variables ---- */

TT_TEST(test_twig_set_variables)
{
    char root[512]; TWIG_ROOT(root);
    const char *files[] = {"templates/pages/dashboard.html.twig"};
    tt_symbol_t *syms = NULL; int count = 0;

    tt_parse_twig(root, files, 1, &syms, &count);

    const tt_symbol_t *s = sym_find_qname(syms, count, "page_title");
    TT_ASSERT_NOT_NULL(s);
    if (s) TT_ASSERT_EQ_INT(TT_KIND_VARIABLE, s->kind);

    s = sym_find_qname(syms, count, "total_items");
    TT_ASSERT_NOT_NULL(s);
    if (s) TT_ASSERT_EQ_INT(TT_KIND_VARIABLE, s->kind);

    tt_symbol_array_free(syms, count);
}

/* ---- Imports fixture ---- */

TT_TEST(test_twig_import_from_use)
{
    char root[512]; TWIG_ROOT(root);
    const char *files[] = {"templates/pages/imports.html.twig"};
    tt_symbol_t *syms = NULL; int count = 0;

    tt_parse_twig(root, files, 1, &syms, &count);

    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "import.macros/forms.html.twig"));
    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "from.macros/forms.html.twig"));
    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "use.blocks/common.html.twig"));

    tt_symbol_array_free(syms, count);
}

TT_TEST(test_twig_self_import_skip)
{
    char root[512]; TWIG_ROOT(root);
    const char *files[] = {"templates/pages/imports.html.twig"};
    tt_symbol_t *syms = NULL; int count = 0;

    tt_parse_twig(root, files, 1, &syms, &count);

    /* _self should NOT appear in results */
    for (int i = 0; i < count; i++) {
        TT_ASSERT(strcmp(syms[i].name, "_self") != 0,
                   "_self should not be extracted");
    }

    tt_symbol_array_free(syms, count);
}

/* ---- HTML attributes ---- */

TT_TEST(test_twig_html_ids)
{
    char root[512]; TWIG_ROOT(root);
    const char *files[] = {"templates/base.html.twig"};
    tt_symbol_t *syms = NULL; int count = 0;

    tt_parse_twig(root, files, 1, &syms, &count);

    TT_ASSERT(sym_has_name(syms, count, "app-root"), "should find id='app-root'");
    TT_ASSERT(sym_has_name(syms, count, "main-nav"), "should find id='main-nav'");
    TT_ASSERT(sym_has_name(syms, count, "content-area"), "should find id='content-area'");
    TT_ASSERT(sym_has_name(syms, count, "page-footer"), "should find id='page-footer'");

    int const_count = sym_count_kind(syms, count, TT_KIND_CONSTANT);
    TT_ASSERT_GE_INT(const_count, 4);

    tt_symbol_array_free(syms, count);
}

TT_TEST(test_twig_data_controller)
{
    char root[512]; TWIG_ROOT(root);
    const char *files[] = {"templates/pages/dashboard.html.twig"};
    tt_symbol_t *syms = NULL; int count = 0;

    tt_parse_twig(root, files, 1, &syms, &count);

    TT_ASSERT(sym_has_name(syms, count, "dashboard--list"),
              "should find data-controller='dashboard--list'");

    const tt_symbol_t *s = sym_find_qname(syms, count, "stimulus.dashboard--list");
    TT_ASSERT_NOT_NULL(s);
    if (s) TT_ASSERT_EQ_INT(TT_KIND_CONSTANT, s->kind);

    tt_symbol_array_free(syms, count);
}

/* ---- Dead zone tests ---- */

TT_TEST(test_twig_comment_skip)
{
    char root[512]; TWIG_ROOT(root);
    const char *files[] = {"templates/pages/dashboard.html.twig"};
    tt_symbol_t *syms = NULL; int count = 0;

    tt_parse_twig(root, files, 1, &syms, &count);

    /* should-not-appear.html.twig was inside {# comment #} */
    TT_ASSERT(!sym_has_name(syms, count, "should-not-appear.html.twig"),
              "commented include should not be extracted");

    tt_symbol_array_free(syms, count);
}

TT_TEST(test_twig_verbatim_skip)
{
    char root[512]; TWIG_ROOT(root);
    const char *files[] = {"templates/edge/verbatim.html.twig"};
    tt_symbol_t *syms = NULL; int count = 0;

    tt_parse_twig(root, files, 1, &syms, &count);

    /* Only real_block should be extracted, not should_not_extract */
    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "block.real_block"));
    TT_ASSERT(sym_find_qname(syms, count, "block.should_not_extract") == NULL,
              "verbatim content should not be extracted");

    tt_symbol_array_free(syms, count);
}

TT_TEST(test_twig_raw_skip)
{
    char root[512]; TWIG_ROOT(root);
    const char *files[] = {"templates/edge/verbatim.html.twig"};
    tt_symbol_t *syms = NULL; int count = 0;

    tt_parse_twig(root, files, 1, &syms, &count);

    /* also-not-extracted.html.twig was inside {% raw %} */
    TT_ASSERT(!sym_has_name(syms, count, "also-not-extracted.html.twig"),
              "raw zone content should not be extracted");

    tt_symbol_array_free(syms, count);
}

/* ---- List paths ---- */

TT_TEST(test_twig_list_paths)
{
    char root[512]; TWIG_ROOT(root);
    const char *files[] = {"templates/edge/lists.html.twig"};
    tt_symbol_t *syms = NULL; int count = 0;

    tt_parse_twig(root, files, 1, &syms, &count);

    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "extends.layouts/special.html.twig"));
    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "extends.base.html.twig"));
    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "include.components/fancy.html.twig"));
    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "include.components/simple.html.twig"));

    tt_symbol_array_free(syms, count);
}

/* ---- Dynamic paths ---- */

TT_TEST(test_twig_dynamic_skip)
{
    char root[512]; TWIG_ROOT(root);
    const char *files[] = {"templates/edge/dynamic.html.twig"};
    tt_symbol_t *syms = NULL; int count = 0;

    tt_parse_twig(root, files, 1, &syms, &count);

    /* Only block + HTML id should be extracted (2 symbols) */
    TT_ASSERT_EQ_INT(2, count);
    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "block.actual_content"));
    TT_ASSERT(sym_has_name(syms, count, "dynamic-test"), "should find HTML id");

    tt_symbol_array_free(syms, count);
}

/* ---- Keywords and content hash ---- */

TT_TEST(test_twig_keywords)
{
    char root[512]; TWIG_ROOT(root);
    const char *files[] = {"templates/base.html.twig"};
    tt_symbol_t *syms = NULL; int count = 0;

    tt_parse_twig(root, files, 1, &syms, &count);

    const tt_symbol_t *s = sym_find_qname(syms, count, "block.title");
    TT_ASSERT_NOT_NULL(s);
    if (s) {
        TT_ASSERT_STR_CONTAINS(s->keywords, "\"title\"");
    }

    tt_symbol_array_free(syms, count);
}

TT_TEST(test_twig_content_hash)
{
    char root[512]; TWIG_ROOT(root);
    const char *files[] = {"templates/base.html.twig"};
    tt_symbol_t *syms = NULL; int count = 0;

    tt_parse_twig(root, files, 1, &syms, &count);
    TT_ASSERT(count > 0, "should have symbols");

    for (int i = 0; i < count; i++) {
        TT_ASSERT(syms[i].content_hash != NULL, "hash should not be NULL");
        TT_ASSERT(strlen(syms[i].content_hash) == 16,
                   "content hash should be 16 hex chars");
    }

    tt_symbol_array_free(syms, count);
}

TT_TEST(test_twig_null_inputs)
{
    tt_symbol_t *syms = NULL;
    int count = 0;

    int rc = tt_parse_twig(NULL, NULL, 0, &syms, &count);
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_INT(0, count);
}

void run_int_parser_twig_tests(void)
{
    TT_RUN(test_twig_extracts_directives);
    TT_RUN(test_twig_kind_directive);
    TT_RUN(test_twig_language_is_twig);
    TT_RUN(test_twig_id_format);
    TT_RUN(test_twig_empty_file);
    TT_RUN(test_twig_blocks);
    TT_RUN(test_twig_includes);
    TT_RUN(test_twig_extends);
    TT_RUN(test_twig_embed);
    TT_RUN(test_twig_macros);
    TT_RUN(test_twig_macro_whitespace_mods);
    TT_RUN(test_twig_set_variables);
    TT_RUN(test_twig_import_from_use);
    TT_RUN(test_twig_self_import_skip);
    TT_RUN(test_twig_html_ids);
    TT_RUN(test_twig_data_controller);
    TT_RUN(test_twig_comment_skip);
    TT_RUN(test_twig_verbatim_skip);
    TT_RUN(test_twig_raw_skip);
    TT_RUN(test_twig_list_paths);
    TT_RUN(test_twig_dynamic_skip);
    TT_RUN(test_twig_keywords);
    TT_RUN(test_twig_content_hash);
    TT_RUN(test_twig_null_inputs);
}
