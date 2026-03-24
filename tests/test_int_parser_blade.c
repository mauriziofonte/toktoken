/*
 * test_int_parser_blade.c -- Integration tests for parser_blade module.
 */

#include "test_framework.h"
#include "test_helpers.h"
#include "parser_blade.h"
#include "symbol.h"
#include "symbol_kind.h"

#include "str_util.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

TT_TEST(test_int_blade_extracts_directives)
{
    const char *fixture = tt_test_fixtures_dir();
    if (!fixture) return;

    char root[512];
    snprintf(root, sizeof(root), "%s/blade-project", fixture);

    const char *files[] = {"resources/views/sample.blade.php"};
    tt_symbol_t *syms = NULL;
    int count = 0;

    int rc = tt_parse_blade(root, files, 1, &syms, &count);
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT(count > 0, "should extract directives");

    TT_ASSERT(sym_has_name(syms, count, "layouts.app"), "should find layouts.app");

    tt_symbol_array_free(syms, count);
}

TT_TEST(test_int_blade_kind_is_directive)
{
    const char *fixture = tt_test_fixtures_dir();
    if (!fixture) return;

    char root[512];
    snprintf(root, sizeof(root), "%s/blade-project", fixture);

    const char *files[] = {"resources/views/sample.blade.php"};
    tt_symbol_t *syms = NULL;
    int count = 0;

    tt_parse_blade(root, files, 1, &syms, &count);

    for (int i = 0; i < count; i++) {
        TT_ASSERT_EQ_STR("directive", tt_kind_to_str(syms[i].kind));
    }

    tt_symbol_array_free(syms, count);
}

TT_TEST(test_int_blade_language_is_blade)
{
    const char *fixture = tt_test_fixtures_dir();
    if (!fixture) return;

    char root[512];
    snprintf(root, sizeof(root), "%s/blade-project", fixture);

    const char *files[] = {"resources/views/sample.blade.php"};
    tt_symbol_t *syms = NULL;
    int count = 0;

    tt_parse_blade(root, files, 1, &syms, &count);

    for (int i = 0; i < count; i++) {
        TT_ASSERT_EQ_STR("blade", syms[i].language);
    }

    tt_symbol_array_free(syms, count);
}

TT_TEST(test_int_blade_id_format)
{
    const char *fixture = tt_test_fixtures_dir();
    if (!fixture) return;

    char root[512];
    snprintf(root, sizeof(root), "%s/blade-project", fixture);

    const char *files[] = {"resources/views/sample.blade.php"};
    tt_symbol_t *syms = NULL;
    int count = 0;

    tt_parse_blade(root, files, 1, &syms, &count);

    for (int i = 0; i < count; i++) {
        TT_ASSERT(strncmp(syms[i].id, "resources/views/sample.blade.php::", 33) == 0,
                   "ID should start with file path");
        TT_ASSERT(strstr(syms[i].id, "#directive") != NULL,
                   "ID should end with #directive");
    }

    tt_symbol_array_free(syms, count);
}

TT_TEST(test_int_blade_empty_file_list)
{
    tt_symbol_t *syms = NULL;
    int count = 0;

    int rc = tt_parse_blade("/tmp", NULL, 0, &syms, &count);
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_INT(0, count);
}

/* ---- Legacy blade tests using tests/fixtures/sample.blade.php ---- */

/* Helper: get project root for fixture-based tests */
static const char *blade_fixture_root(void)
{
    const char *f = tt_test_fixtures_dir();
    return f;  /* NULL if not found */
}

TT_TEST(test_int_blade_directives_detail)
{
    const char *fixtures = blade_fixture_root();
    if (!fixtures) return;

    const char *files[] = {"sample.blade.php"};
    tt_symbol_t *syms = NULL;
    int count = 0;

    int rc = tt_parse_blade(fixtures, files, 1, &syms, &count);
    TT_ASSERT_EQ_INT(rc, 0);
    TT_ASSERT_EQ_INT(count, 8);
    if (count != 8) { tt_symbol_array_free(syms, count); return; }

    const tt_symbol_t *s = sym_find_qname(syms, count, "extends.layouts.app");
    TT_ASSERT_NOT_NULL(s);
    if (s) {
        TT_ASSERT_EQ_STR(s->name, "layouts.app");
        TT_ASSERT_EQ_INT(s->line, 1);
    }

    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "section.title"));
    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "include.partials.header"));
    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "yield.sidebar"));

    tt_symbol_array_free(syms, count);
}

TT_TEST(test_int_blade_no_dedup_suffix)
{
    const char *fixtures = blade_fixture_root();
    if (!fixtures) return;

    const char *files[] = {"sample.blade.php"};
    tt_symbol_t *syms = NULL;
    int count = 0;

    tt_parse_blade(fixtures, files, 1, &syms, &count);

    for (int i = 0; i < count; i++) {
        TT_ASSERT_STR_NOT_CONTAINS(syms[i].qualified_name, "~");
    }

    tt_symbol_array_free(syms, count);
}

TT_TEST(test_int_blade_keywords)
{
    const char *fixtures = blade_fixture_root();
    if (!fixtures) return;

    const char *files[] = {"sample.blade.php"};
    tt_symbol_t *syms = NULL;
    int count = 0;

    tt_parse_blade(fixtures, files, 1, &syms, &count);

    const tt_symbol_t *s = sym_find_qname(syms, count, "extends.layouts.app");
    TT_ASSERT_NOT_NULL(s);
    if (s) {
        TT_ASSERT_STR_CONTAINS(s->keywords, "\"extends\"");
        TT_ASSERT_STR_CONTAINS(s->keywords, "\"layouts\"");
        TT_ASSERT_STR_CONTAINS(s->keywords, "\"app\"");
    }

    tt_symbol_array_free(syms, count);
}

TT_TEST(test_int_blade_signature)
{
    const char *fixtures = blade_fixture_root();
    if (!fixtures) return;

    const char *files[] = {"sample.blade.php"};
    tt_symbol_t *syms = NULL;
    int count = 0;

    tt_parse_blade(fixtures, files, 1, &syms, &count);

    const tt_symbol_t *s = sym_find_qname(syms, count, "extends.layouts.app");
    TT_ASSERT_NOT_NULL(s);
    if (s) {
        TT_ASSERT_EQ_STR(s->signature, "@extends('layouts.app')");
    }

    tt_symbol_array_free(syms, count);
}

TT_TEST(test_int_blade_normalize_directive)
{
    const char *fixtures = blade_fixture_root();
    if (!fixtures) return;

    const char *files[] = {"sample.blade.php"};
    tt_symbol_t *syms = NULL;
    int count = 0;

    tt_parse_blade(fixtures, files, 1, &syms, &count);
    if (count == 0) return;

    for (int i = 0; i < count; i++) {
        TT_ASSERT_FALSE(tt_str_starts_with(syms[i].qualified_name, "includeIf."));
        TT_ASSERT_FALSE(tt_str_starts_with(syms[i].qualified_name, "includeWhen."));
        TT_ASSERT_FALSE(tt_str_starts_with(syms[i].qualified_name, "includeFirst."));
        TT_ASSERT_FALSE(tt_str_starts_with(syms[i].qualified_name, "prepend."));
    }

    tt_symbol_array_free(syms, count);
}

/* ---- New fixture-based tests (blade-project/) ---- */

#define BLADE_ROOT() \
    const char *fixture = tt_test_fixtures_dir(); \
    if (!fixture) return; \
    char root[512]; \
    snprintf(root, sizeof(root), "%s/blade-project", fixture)

#define BLADE_PARSE(relpath) \
    const char *files[] = {relpath}; \
    tt_symbol_t *syms = NULL; \
    int count = 0; \
    int rc = tt_parse_blade(root, files, 1, &syms, &count); \
    TT_ASSERT_EQ_INT(0, rc)

TT_TEST(test_blade_comment_dead_zone)
{
    BLADE_ROOT();
    BLADE_PARSE("resources/views/pages/dashboard.blade.php");

    for (int i = 0; i < count; i++) {
        TT_ASSERT_FALSE(sym_has_name(syms, count, "should-not-appear"));
    }

    tt_symbol_array_free(syms, count);
}

TT_TEST(test_blade_verbatim_dead_zone)
{
    BLADE_ROOT();
    BLADE_PARSE("resources/views/edge/verbatim.blade.php");

    /* Only section + x-component should be extracted */
    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "section.real-content"));
    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "component.real-component"));
    TT_ASSERT_EQ_INT(count, 2);

    /* Dead zone content must NOT appear */
    TT_ASSERT(sym_find_qname(syms, count, "extends.should-not-extract") == NULL,
              "verbatim content must not extract extends");

    tt_symbol_array_free(syms, count);
}

TT_TEST(test_blade_include_variants)
{
    BLADE_ROOT();
    BLADE_PARSE("resources/views/pages/dashboard.blade.php");

    /* All include variants should be normalized to include.* */
    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "include.partials.header"));
    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "include.partials.optional-banner"));
    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "include.partials.sidebar"));

    /* No un-normalized names */
    for (int i = 0; i < count; i++) {
        TT_ASSERT_FALSE(tt_str_starts_with(syms[i].qualified_name, "includeIf."));
        TT_ASSERT_FALSE(tt_str_starts_with(syms[i].qualified_name, "includeWhen."));
        TT_ASSERT_FALSE(tt_str_starts_with(syms[i].qualified_name, "includeFirst."));
        TT_ASSERT_FALSE(tt_str_starts_with(syms[i].qualified_name, "pushOnce."));
        TT_ASSERT_FALSE(tt_str_starts_with(syms[i].qualified_name, "prependOnce."));
    }

    tt_symbol_array_free(syms, count);
}

TT_TEST(test_blade_includeFirst_list)
{
    BLADE_ROOT();
    BLADE_PARSE("resources/views/pages/dashboard.blade.php");

    /* Both paths from @includeFirst(['partials.custom-footer', 'partials.footer']) */
    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "include.partials.custom-footer"));
    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "include.partials.footer"));

    tt_symbol_array_free(syms, count);
}

TT_TEST(test_blade_inject_directive)
{
    BLADE_ROOT();
    BLADE_PARSE("resources/views/pages/dashboard.blade.php");

    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "inject.metrics"));

    tt_symbol_array_free(syms, count);
}

TT_TEST(test_blade_each_directive)
{
    BLADE_ROOT();
    BLADE_PARSE("resources/views/pages/dashboard.blade.php");

    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "each.partials.item"));

    tt_symbol_array_free(syms, count);
}

TT_TEST(test_blade_use_directive)
{
    BLADE_ROOT();
    BLADE_PARSE("resources/views/pages/dashboard.blade.php");

    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "use.App\\Models\\User"));

    tt_symbol_array_free(syms, count);
}

TT_TEST(test_blade_livewire)
{
    BLADE_ROOT();
    BLADE_PARSE("resources/views/pages/dashboard.blade.php");

    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "livewire.notifications"));

    tt_symbol_array_free(syms, count);
}

TT_TEST(test_blade_prepend_normalized)
{
    BLADE_ROOT();
    BLADE_PARSE("resources/views/pages/dashboard.blade.php");

    /* @prepend('styles') should be normalized to push.styles */
    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "push.styles"));
    for (int i = 0; i < count; i++) {
        TT_ASSERT_FALSE(tt_str_starts_with(syms[i].qualified_name, "prepend."));
    }

    tt_symbol_array_free(syms, count);
}

TT_TEST(test_blade_x_component_extraction)
{
    BLADE_ROOT();
    BLADE_PARSE("resources/views/pages/dashboard.blade.php");

    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "component.alert"));
    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "component.inputs.text-field"));
    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "component.navigation.breadcrumb"));

    tt_symbol_array_free(syms, count);
}

TT_TEST(test_blade_x_component_kind)
{
    BLADE_ROOT();
    BLADE_PARSE("resources/views/edge/components.blade.php");

    const tt_symbol_t *s = sym_find_qname(syms, count, "component.alert");
    TT_ASSERT_NOT_NULL(s);
    if (s) {
        TT_ASSERT_EQ_STR("directive", tt_kind_to_str(s->kind));
        TT_ASSERT_EQ_STR("blade", s->language);
    }

    tt_symbol_array_free(syms, count);
}

TT_TEST(test_blade_x_slot_extraction)
{
    BLADE_ROOT();
    BLADE_PARSE("resources/views/edge/components.blade.php");

    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "slot.header"));
    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "slot.footer"));

    tt_symbol_array_free(syms, count);
}

TT_TEST(test_blade_x_dynamic_skip)
{
    BLADE_ROOT();
    BLADE_PARSE("resources/views/edge/components.blade.php");

    /* <x-dynamic-component> must NOT be extracted */
    for (int i = 0; i < count; i++) {
        TT_ASSERT_FALSE(strstr(syms[i].qualified_name, "dynamic-component") != NULL);
    }

    tt_symbol_array_free(syms, count);
}

TT_TEST(test_blade_dedup_suffix)
{
    BLADE_ROOT();
    BLADE_PARSE("resources/views/edge/dedup.blade.php");

    TT_ASSERT_EQ_INT(count, 5);
    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "include.partials.header"));
    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "include.partials.header~2"));
    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "include.partials.footer"));
    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "include.partials.footer~2"));
    TT_ASSERT_NOT_NULL(sym_find_qname(syms, count, "include.partials.footer~3"));

    tt_symbol_array_free(syms, count);
}

TT_TEST(test_blade_empty_file)
{
    BLADE_ROOT();
    BLADE_PARSE("resources/views/empty.blade.php");

    TT_ASSERT_EQ_INT(count, 0);

    tt_symbol_array_free(syms, count);
}

void run_int_parser_blade_tests(void)
{
    TT_RUN(test_int_blade_extracts_directives);
    TT_RUN(test_int_blade_kind_is_directive);
    TT_RUN(test_int_blade_language_is_blade);
    TT_RUN(test_int_blade_id_format);
    TT_RUN(test_int_blade_empty_file_list);
    TT_RUN(test_int_blade_directives_detail);
    TT_RUN(test_int_blade_no_dedup_suffix);
    TT_RUN(test_int_blade_keywords);
    TT_RUN(test_int_blade_signature);
    TT_RUN(test_int_blade_normalize_directive);
    /* New tests */
    TT_RUN(test_blade_comment_dead_zone);
    TT_RUN(test_blade_verbatim_dead_zone);
    TT_RUN(test_blade_include_variants);
    TT_RUN(test_blade_includeFirst_list);
    TT_RUN(test_blade_inject_directive);
    TT_RUN(test_blade_each_directive);
    TT_RUN(test_blade_use_directive);
    TT_RUN(test_blade_livewire);
    TT_RUN(test_blade_prepend_normalized);
    TT_RUN(test_blade_x_component_extraction);
    TT_RUN(test_blade_x_component_kind);
    TT_RUN(test_blade_x_slot_extraction);
    TT_RUN(test_blade_x_dynamic_skip);
    TT_RUN(test_blade_dedup_suffix);
    TT_RUN(test_blade_empty_file);
}
