/*
 * test_int_parser_razor.c -- Integration tests for parser_razor module.
 */

#include "test_framework.h"
#include "test_helpers.h"
#include "parser_razor.h"
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

static int sym_count_kind(const tt_symbol_t *syms, int count,
                          tt_symbol_kind_e kind)
{
    int n = 0;
    for (int i = 0; i < count; i++)
        if (syms[i].kind == kind)
            n++;
    return n;
}

TT_TEST(test_razor_functions_block)
{
    const char *fixture = tt_test_fixtures_dir();
    if (!fixture) return;

    char root[512];
    snprintf(root, sizeof(root), "%s/razor-project", fixture);

    const char *files[] = {"Views/Home.cshtml"};
    tt_symbol_t *syms = NULL;
    int count = 0;

    int rc = tt_parse_razor(root, files, 1, &syms, &count);
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT(count > 0, "should extract symbols");

    /* Should find HandleSubmit method */
    TT_ASSERT(sym_has_name(syms, count, "HandleSubmit"),
              "should extract HandleSubmit from @functions block");

    tt_symbol_array_free(syms, count);
}

TT_TEST(test_razor_directives)
{
    const char *fixture = tt_test_fixtures_dir();
    if (!fixture) return;

    char root[512];
    snprintf(root, sizeof(root), "%s/razor-project", fixture);

    const char *files[] = {"Views/Home.cshtml"};
    tt_symbol_t *syms = NULL;
    int count = 0;

    int rc = tt_parse_razor(root, files, 1, &syms, &count);
    TT_ASSERT_EQ_INT(0, rc);

    /* Should find @model directive */
    TT_ASSERT(sym_has_name(syms, count, "HomeViewModel"),
              "should extract @model directive");

    /* Should find @using directive */
    TT_ASSERT(sym_has_name(syms, count, "MyApp.Services"),
              "should extract @using directive");

    /* Directives should be TT_KIND_DIRECTIVE */
    int directive_count = sym_count_kind(syms, count, TT_KIND_DIRECTIVE);
    TT_ASSERT_GE_INT(directive_count, 2);

    tt_symbol_array_free(syms, count);
}

TT_TEST(test_razor_html_ids)
{
    const char *fixture = tt_test_fixtures_dir();
    if (!fixture) return;

    char root[512];
    snprintf(root, sizeof(root), "%s/razor-project", fixture);

    const char *files[] = {"Views/Home.cshtml"};
    tt_symbol_t *syms = NULL;
    int count = 0;

    int rc = tt_parse_razor(root, files, 1, &syms, &count);
    TT_ASSERT_EQ_INT(0, rc);

    /* Should find HTML ids */
    TT_ASSERT(sym_has_name(syms, count, "page-header"),
              "should extract id=\"page-header\"");
    TT_ASSERT(sym_has_name(syms, count, "content-area"),
              "should extract id=\"content-area\"");

    /* HTML ids should be TT_KIND_CONSTANT */
    int const_count = sym_count_kind(syms, count, TT_KIND_CONSTANT);
    TT_ASSERT_GE_INT(const_count, 2);

    tt_symbol_array_free(syms, count);
}

TT_TEST(test_razor_empty)
{
    tt_symbol_t *syms = NULL;
    int count = 0;

    /* NULL inputs should not crash */
    int rc = tt_parse_razor(NULL, NULL, 0, &syms, &count);
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_INT(0, count);
}

void run_int_parser_razor_tests(void)
{
    TT_RUN(test_razor_functions_block);
    TT_RUN(test_razor_directives);
    TT_RUN(test_razor_html_ids);
    TT_RUN(test_razor_empty);
}
