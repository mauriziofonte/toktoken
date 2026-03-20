/*
 * test_int_v020.c -- Integration tests for v0.2.0 features.
 *
 * Tests: find:dead, inspect:cycles, inspect:blast, index:file,
 *        search detail levels, token budget, scope filters,
 *        centrality ranking, multi-symbol bundle, markdown output.
 *
 * Creates a multi-file project with imports to test import-graph features.
 */

#include "test_framework.h"
#include "test_helpers.h"

#include "cmd_search.h"
#include "cmd_inspect.h"
#include "cmd_index.h"
#include "cmd_find.h"
#include "cmd_bundle.h"
#include "cli.h"
#include "json_output.h"
#include "error.h"
#include "platform.h"
#include "database.h"
#include "index_store.h"
#include "str_util.h"

#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- Test fixture ---- */

static char *test_dir = NULL;

static void init_opts(tt_cli_opts_t *opts)
{
    memset(opts, 0, sizeof(*opts));
    opts->path = test_dir;
    opts->truncate_width = 120;
}

/*
 * Project layout:
 *
 * src/main.py           -- imports utils.py (no one imports main.py -> "dead" file)
 * src/utils.py          -- imports helpers.py (imported by main.py)
 * src/helpers.py        -- imported by utils.py
 * src/orphan.py         -- no imports in or out (all symbols "dead")
 * src/cycle_a.py        -- imports cycle_b.py (cycle A<->B)
 * src/cycle_b.py        -- imports cycle_a.py (cycle A<->B)
 * lib/service.py        -- imports helpers.py (cross-dir import)
 * lib/cycle_c.py        -- imports cycle_a.py, and cycle_a imports cycle_c -> 3-file cycle
 * tests/test_main.py    -- test file (for --exclude-tests)
 */
static void setup_fixture(void)
{
    test_dir = tt_test_tmpdir();
    if (!test_dir) {
        fprintf(stderr, "  FATAL: tt_test_tmpdir() returned NULL\n");
        return;
    }

    /* main.py: imports utils, has main() entry point */
    tt_test_write_file(test_dir, "src/main.py",
        "from utils import calculate_sum\n"
        "\n"
        "def main():\n"
        "    result = calculate_sum(1, 2)\n"
        "    print(result)\n"
        "\n"
        "def unused_in_main():\n"
        "    pass\n");

    /* utils.py: exports calculate_sum, helper_call; imports helpers */
    tt_test_write_file(test_dir, "src/utils.py",
        "from helpers import format_value\n"
        "\n"
        "def calculate_sum(a, b):\n"
        "    return format_value(a + b)\n"
        "\n"
        "def helper_call():\n"
        "    return format_value(42)\n"
        "\n"
        "def unreferenced_util():\n"
        "    \"\"\"Nobody imports this by name.\"\"\"\n"
        "    pass\n");

    /* helpers.py: exports format_value */
    tt_test_write_file(test_dir, "src/helpers.py",
        "def format_value(v):\n"
        "    return str(v)\n"
        "\n"
        "def internal_helper():\n"
        "    pass\n");

    /* orphan.py: no imports, nobody imports it */
    tt_test_write_file(test_dir, "src/orphan.py",
        "def lonely_function():\n"
        "    return 'alone'\n"
        "\n"
        "def another_lonely():\n"
        "    return 'also alone'\n");

    /* cycle_a.py: imports cycle_b */
    tt_test_write_file(test_dir, "src/cycle_a.py",
        "from cycle_b import func_b\n"
        "from cycle_c import func_c\n"
        "\n"
        "def func_a():\n"
        "    return func_b() + func_c()\n");

    /* cycle_b.py: imports cycle_a -> creates A<->B cycle */
    tt_test_write_file(test_dir, "src/cycle_b.py",
        "from cycle_a import func_a\n"
        "\n"
        "def func_b():\n"
        "    return func_a()\n");

    /* lib/service.py: imports helpers (cross-dir) */
    tt_test_write_file(test_dir, "lib/service.py",
        "from helpers import format_value\n"
        "\n"
        "def serve():\n"
        "    return format_value('data')\n");

    /* lib/cycle_c.py: imports cycle_a -> creates 3-file cycle A->B->A and A->C->A */
    tt_test_write_file(test_dir, "lib/cycle_c.py",
        "from cycle_a import func_a\n"
        "\n"
        "def func_c():\n"
        "    return func_a()\n");

    /* tests/test_main.py: test file */
    tt_test_write_file(test_dir, "tests/test_main.py",
        "from main import main\n"
        "\n"
        "def test_main_works():\n"
        "    main()\n");
}

static void cleanup_fixture(void)
{
    if (test_dir) {
        tt_test_rmdir(test_dir);
        free(test_dir);
        test_dir = NULL;
    }
}

static void index_fixture(void)
{
    tt_cli_opts_t opts;
    init_opts(&opts);
    opts.truncate_width = 120;

    cJSON *result = tt_cmd_index_create_exec(&opts);
    if (result) {
        if (cJSON_GetObjectItem(result, "error")) {
            const char *msg = cJSON_GetStringValue(
                cJSON_GetObjectItem(result, "message"));
            fprintf(stderr, "  index:create failed: %s\n", msg ? msg : "unknown");
        }
        cJSON_Delete(result);
    } else {
        fprintf(stderr, "  index:create returned NULL: %s\n", tt_error_get());
    }
}

/* ---- Helper: find symbol ID by searching ---- */

static char *find_symbol_id(const char *query, const char *kind_filter)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    const char *pos[] = {query};
    opts.positional = pos;
    opts.positional_count = 1;
    if (kind_filter) opts.kind = kind_filter;

    cJSON *result = tt_cmd_search_symbols_exec(&opts);
    if (!result) return NULL;

    cJSON *results = cJSON_GetObjectItem(result, "results");
    if (!results || cJSON_GetArraySize(results) == 0) {
        cJSON_Delete(result);
        return NULL;
    }

    const char *id = cJSON_GetStringValue(
        cJSON_GetObjectItem(cJSON_GetArrayItem(results, 0), "id"));
    char *dup = id ? strdup(id) : NULL;
    cJSON_Delete(result);
    return dup;
}

/* =================================================================
 * find:dead tests
 * ================================================================= */

TT_TEST(test_find_dead_basic)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    cJSON *result = tt_cmd_find_dead_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT(cJSON_GetObjectItem(result, "error") == NULL, "should not have error");

    cJSON *results = cJSON_GetObjectItem(result, "results");
    TT_ASSERT_NOT_NULL(results);
    TT_ASSERT(cJSON_IsArray(results), "results should be array");
    TT_ASSERT(cJSON_GetArraySize(results) > 0, "should find dead/unreferenced symbols");

    cJSON *summary = cJSON_GetObjectItem(result, "summary");
    TT_ASSERT_NOT_NULL(summary);
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(summary, "dead"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(summary, "unreferenced"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(summary, "total"));

    /* Check result structure */
    cJSON *first = cJSON_GetArrayItem(results, 0);
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "id"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "name"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "classification"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "confidence"));

    cJSON_Delete(result);
}

TT_TEST(test_find_dead_orphan_is_dead)
{
    /* orphan.py has no importers -> all its symbols should be "dead" */
    tt_cli_opts_t opts;
    init_opts(&opts);
    opts.filter = "orphan";

    cJSON *result = tt_cmd_find_dead_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    cJSON *results = cJSON_GetObjectItem(result, "results");
    TT_ASSERT_NOT_NULL(results);

    int dead_count = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, results) {
        const char *cls = cJSON_GetStringValue(
            cJSON_GetObjectItem(item, "classification"));
        if (cls && strcmp(cls, "dead") == 0)
            dead_count++;
    }
    TT_ASSERT(dead_count > 0, "orphan.py symbols should be classified as dead");

    cJSON_Delete(result);
}

TT_TEST(test_find_dead_entry_point_excluded)
{
    /* main() should be excluded as entry point */
    tt_cli_opts_t opts;
    init_opts(&opts);

    cJSON *result = tt_cmd_find_dead_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    cJSON *results = cJSON_GetObjectItem(result, "results");
    cJSON *item;
    cJSON_ArrayForEach(item, results) {
        const char *name = cJSON_GetStringValue(
            cJSON_GetObjectItem(item, "name"));
        TT_ASSERT(name == NULL || strcmp(name, "main") != 0,
                  "main() should be excluded as entry point");
    }

    cJSON_Delete(result);
}

TT_TEST(test_find_dead_test_prefix_excluded)
{
    /* test_* functions should be excluded as entry points */
    tt_cli_opts_t opts;
    init_opts(&opts);

    cJSON *result = tt_cmd_find_dead_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    cJSON *results = cJSON_GetObjectItem(result, "results");
    cJSON *item;
    cJSON_ArrayForEach(item, results) {
        const char *name = cJSON_GetStringValue(
            cJSON_GetObjectItem(item, "name"));
        TT_ASSERT(name == NULL || strncmp(name, "test_", 5) != 0,
                  "test_* should be excluded as entry point");
    }

    cJSON_Delete(result);
}

TT_TEST(test_find_dead_exclude_tests)
{
    /* --exclude-tests should filter test files */
    tt_cli_opts_t opts;
    init_opts(&opts);
    opts.exclude_tests = true;

    cJSON *result = tt_cmd_find_dead_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    cJSON *results = cJSON_GetObjectItem(result, "results");
    cJSON *item;
    cJSON_ArrayForEach(item, results) {
        const char *file = cJSON_GetStringValue(
            cJSON_GetObjectItem(item, "file"));
        if (file) {
            TT_ASSERT(strstr(file, "test_") == NULL ||
                      strstr(file, "tests/") == NULL,
                      "test files should be excluded with --exclude-tests");
        }
    }

    cJSON_Delete(result);
}

TT_TEST(test_find_dead_kind_filter)
{
    tt_cli_opts_t opts;
    init_opts(&opts);
    opts.kind = "function";

    cJSON *result = tt_cmd_find_dead_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    cJSON *results = cJSON_GetObjectItem(result, "results");
    cJSON *item;
    cJSON_ArrayForEach(item, results) {
        const char *kind = cJSON_GetStringValue(
            cJSON_GetObjectItem(item, "kind"));
        TT_ASSERT(kind && strcmp(kind, "function") == 0,
                  "all results should be function kind");
    }

    cJSON_Delete(result);
}

TT_TEST(test_find_dead_no_index)
{
    tt_cli_opts_t opts;
    init_opts(&opts);
    opts.path = "/tmp/nonexistent_project_v020_test";

    cJSON *result = tt_cmd_find_dead_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    const char *err = cJSON_GetStringValue(cJSON_GetObjectItem(result, "error"));
    TT_ASSERT_EQ_STR(err, "no_index");

    cJSON_Delete(result);
}

TT_TEST(test_find_dead_confidence_levels)
{
    /* Python should get "medium" confidence */
    tt_cli_opts_t opts;
    init_opts(&opts);

    cJSON *result = tt_cmd_find_dead_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    cJSON *results = cJSON_GetObjectItem(result, "results");
    if (cJSON_GetArraySize(results) > 0) {
        cJSON *first = cJSON_GetArrayItem(results, 0);
        const char *conf = cJSON_GetStringValue(
            cJSON_GetObjectItem(first, "confidence"));
        TT_ASSERT_NOT_NULL(conf);
        /* Python -> "medium" */
        TT_ASSERT(strcmp(conf, "high") == 0 ||
                  strcmp(conf, "medium") == 0 ||
                  strcmp(conf, "low") == 0,
                  "confidence should be high/medium/low");
    }

    cJSON_Delete(result);
}

TT_TEST(test_find_dead_limit)
{
    tt_cli_opts_t opts;
    init_opts(&opts);
    opts.limit = 2;

    cJSON *result = tt_cmd_find_dead_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    cJSON *results = cJSON_GetObjectItem(result, "results");
    TT_ASSERT_LE_INT(cJSON_GetArraySize(results), 2);

    cJSON_Delete(result);
}

/* =================================================================
 * inspect:cycles tests
 * ================================================================= */

TT_TEST(test_inspect_cycles_basic)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    cJSON *result = tt_cmd_inspect_cycles_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT(cJSON_GetObjectItem(result, "error") == NULL, "should not have error");

    cJSON *cycles = cJSON_GetObjectItem(result, "cycles");
    TT_ASSERT_NOT_NULL(cycles);
    TT_ASSERT(cJSON_IsArray(cycles), "cycles should be array");

    cJSON *summary = cJSON_GetObjectItem(result, "summary");
    TT_ASSERT_NOT_NULL(summary);
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(summary, "total_cycles"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(summary, "cross_dir_cycles"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(summary, "max_length"));

    int total = (int)cJSON_GetObjectItem(summary, "total_cycles")->valuedouble;
    TT_ASSERT_GT_INT(total, 0);

    /* Each cycle should have files, length, cross_dir */
    if (cJSON_GetArraySize(cycles) > 0) {
        cJSON *first = cJSON_GetArrayItem(cycles, 0);
        TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "files"));
        TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "length"));
        TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "cross_dir"));

        int len = (int)cJSON_GetObjectItem(first, "length")->valuedouble;
        TT_ASSERT(len >= 2, "cycle length should be >= 2");
    }

    cJSON_Delete(result);
}

TT_TEST(test_inspect_cycles_min_length)
{
    /* Filter cycles with min_length = 3 */
    tt_cli_opts_t opts;
    init_opts(&opts);
    opts.min_length = 3;

    cJSON *result = tt_cmd_inspect_cycles_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    cJSON *cycles = cJSON_GetObjectItem(result, "cycles");
    TT_ASSERT_NOT_NULL(cycles);

    /* All returned cycles should have length >= 3 */
    cJSON *item;
    cJSON_ArrayForEach(item, cycles) {
        int len = (int)cJSON_GetObjectItem(item, "length")->valuedouble;
        TT_ASSERT_GE_INT(len, 3);
    }

    /* Summary should have filtered_count */
    cJSON *summary = cJSON_GetObjectItem(result, "summary");
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(summary, "filtered_count"));

    cJSON_Delete(result);
}

TT_TEST(test_inspect_cycles_cross_dir)
{
    /* Filter for cross-directory cycles only */
    tt_cli_opts_t opts;
    init_opts(&opts);
    opts.cross_dir = true;

    cJSON *result = tt_cmd_inspect_cycles_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    cJSON *cycles = cJSON_GetObjectItem(result, "cycles");
    TT_ASSERT_NOT_NULL(cycles);

    /* All returned cycles should be cross-directory */
    cJSON *item;
    cJSON_ArrayForEach(item, cycles) {
        cJSON *cd = cJSON_GetObjectItem(item, "cross_dir");
        TT_ASSERT(cJSON_IsTrue(cd), "filtered cycles should be cross_dir=true");
    }

    cJSON_Delete(result);
}

TT_TEST(test_inspect_cycles_no_index)
{
    tt_cli_opts_t opts;
    init_opts(&opts);
    opts.path = "/tmp/nonexistent_project_v020_test";

    cJSON *result = tt_cmd_inspect_cycles_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    const char *err = cJSON_GetStringValue(cJSON_GetObjectItem(result, "error"));
    TT_ASSERT_EQ_STR(err, "no_index");

    cJSON_Delete(result);
}

TT_TEST(test_inspect_cycles_no_cycles)
{
    /* Create a project with no cycles */
    char *no_cycle_dir = tt_test_tmpdir();
    if (!no_cycle_dir) return;

    tt_test_write_file(no_cycle_dir, "a.py", "def func_a():\n    pass\n");
    tt_test_write_file(no_cycle_dir, "b.py", "from a import func_a\ndef func_b():\n    return func_a()\n");

    tt_cli_opts_t idx_opts;
    memset(&idx_opts, 0, sizeof(idx_opts));
    idx_opts.path = no_cycle_dir;
    idx_opts.truncate_width = 120;
    cJSON *idx = tt_cmd_index_create_exec(&idx_opts);
    if (idx) cJSON_Delete(idx);

    tt_cli_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.path = no_cycle_dir;
    opts.truncate_width = 120;

    cJSON *result = tt_cmd_inspect_cycles_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    cJSON *cycles = cJSON_GetObjectItem(result, "cycles");
    TT_ASSERT_NOT_NULL(cycles);
    TT_ASSERT_EQ_INT(cJSON_GetArraySize(cycles), 0);

    cJSON *summary = cJSON_GetObjectItem(result, "summary");
    int total = (int)cJSON_GetObjectItem(summary, "total_cycles")->valuedouble;
    TT_ASSERT_EQ_INT(total, 0);

    cJSON_Delete(result);
    tt_test_rmdir(no_cycle_dir);
    free(no_cycle_dir);
}

/* =================================================================
 * inspect:blast tests
 * ================================================================= */

TT_TEST(test_inspect_blast_basic)
{
    /* Find a symbol to blast-radius */
    char *sym_id = find_symbol_id("format_value", "function");
    if (!sym_id) {
        TT_ASSERT(0, "could not find format_value symbol");
        return;
    }

    tt_cli_opts_t opts;
    init_opts(&opts);
    const char *pos[] = {sym_id};
    opts.positional = pos;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_inspect_blast_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT(cJSON_GetObjectItem(result, "error") == NULL, "should not have error");

    /* Check structure */
    cJSON *target = cJSON_GetObjectItem(result, "target");
    TT_ASSERT_NOT_NULL(target);
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(target, "id"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(target, "name"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(target, "file"));

    cJSON *confirmed = cJSON_GetObjectItem(result, "confirmed");
    TT_ASSERT_NOT_NULL(confirmed);
    TT_ASSERT(cJSON_IsArray(confirmed), "confirmed should be array");

    cJSON *potential_arr = cJSON_GetObjectItem(result, "potential");
    TT_ASSERT_NOT_NULL(potential_arr);
    TT_ASSERT(cJSON_IsArray(potential_arr), "potential should be array");

    cJSON *summary = cJSON_GetObjectItem(result, "summary");
    TT_ASSERT_NOT_NULL(summary);
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(summary, "confirmed_count"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(summary, "potential_count"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(summary, "max_depth"));

    int conf_count = (int)cJSON_GetObjectItem(summary, "confirmed_count")->valuedouble;
    int pot_count = (int)cJSON_GetObjectItem(summary, "potential_count")->valuedouble;
    TT_ASSERT_GT_INT(conf_count, 0);
    TT_ASSERT_GE_INT(pot_count, 0);

    cJSON_Delete(result);
    free(sym_id);
}

TT_TEST(test_inspect_blast_depth_limit)
{
    char *sym_id = find_symbol_id("format_value", "function");
    if (!sym_id) {
        TT_ASSERT(0, "could not find format_value symbol");
        return;
    }

    tt_cli_opts_t opts;
    init_opts(&opts);
    const char *pos[] = {sym_id};
    opts.positional = pos;
    opts.positional_count = 1;
    opts.depth = 1;

    cJSON *result = tt_cmd_inspect_blast_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    int depth = (int)cJSON_GetObjectItem(result, "depth")->valuedouble;
    TT_ASSERT_EQ_INT(depth, 1);

    /* max_depth in summary should be <= requested depth */
    cJSON *summary = cJSON_GetObjectItem(result, "summary");
    int max_depth = (int)cJSON_GetObjectItem(summary, "max_depth")->valuedouble;
    TT_ASSERT_LE_INT(max_depth, 1);

    cJSON_Delete(result);
    free(sym_id);
}

TT_TEST(test_inspect_blast_not_found)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    const char *pos[] = {"nonexistent::fake#function"};
    opts.positional = pos;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_inspect_blast_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    const char *err = cJSON_GetStringValue(cJSON_GetObjectItem(result, "error"));
    TT_ASSERT_EQ_STR(err, "not_found");

    cJSON_Delete(result);
}

TT_TEST(test_inspect_blast_confirmed_has_references)
{
    /* Confirmed entries should have references > 0 */
    char *sym_id = find_symbol_id("format_value", "function");
    if (!sym_id) {
        TT_ASSERT(0, "could not find format_value symbol");
        return;
    }

    tt_cli_opts_t opts;
    init_opts(&opts);
    const char *pos[] = {sym_id};
    opts.positional = pos;
    opts.positional_count = 1;
    opts.depth = 2;

    cJSON *result = tt_cmd_inspect_blast_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    cJSON *confirmed = cJSON_GetObjectItem(result, "confirmed");
    cJSON *item;
    cJSON_ArrayForEach(item, confirmed) {
        TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(item, "file"));
        TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(item, "depth"));
        cJSON *refs = cJSON_GetObjectItem(item, "references");
        TT_ASSERT_NOT_NULL(refs);
        TT_ASSERT(refs->valuedouble > 0, "confirmed should have references > 0");
    }

    cJSON_Delete(result);
    free(sym_id);
}

/* =================================================================
 * index:file tests
 * ================================================================= */

TT_TEST(test_index_file_no_index)
{
    /* When project path doesn't exist, file stat fails first -> file_not_found.
     * When project path exists but no DB -> no_index.
     * Either error is acceptable. */
    tt_cli_opts_t opts;
    init_opts(&opts);
    opts.path = "/tmp/nonexistent_project_v020_test";

    const char *pos[] = {"src/main.py"};
    opts.positional = pos;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_index_file_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    const char *err = cJSON_GetStringValue(cJSON_GetObjectItem(result, "error"));
    TT_ASSERT_NOT_NULL(err);
    TT_ASSERT(strcmp(err, "no_index") == 0 || strcmp(err, "file_not_found") == 0,
              "should return no_index or file_not_found");

    cJSON_Delete(result);
}

TT_TEST(test_index_file_unchanged)
{
    /* Reindex an already-indexed file without changes -> changed: false */
    tt_cli_opts_t opts;
    init_opts(&opts);

    const char *pos[] = {"src/main.py"};
    opts.positional = pos;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_index_file_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT(cJSON_GetObjectItem(result, "error") == NULL, "should not have error");

    cJSON *changed = cJSON_GetObjectItem(result, "changed");
    TT_ASSERT_NOT_NULL(changed);
    TT_ASSERT(cJSON_IsFalse(changed), "unchanged file should return changed: false");

    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(result, "file"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(result, "duration_seconds"));

    cJSON_Delete(result);
}

TT_TEST(test_index_file_modified)
{
    /* Modify a file then reindex -> changed: true */
    tt_test_write_file(test_dir, "src/orphan.py",
        "def lonely_function():\n"
        "    return 'alone'\n"
        "\n"
        "def another_lonely():\n"
        "    return 'also alone'\n"
        "\n"
        "def brand_new_function():\n"
        "    return 'new'\n");

    tt_cli_opts_t opts;
    init_opts(&opts);

    const char *pos[] = {"src/orphan.py"};
    opts.positional = pos;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_index_file_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT(cJSON_GetObjectItem(result, "error") == NULL, "should not have error");

    cJSON *changed = cJSON_GetObjectItem(result, "changed");
    TT_ASSERT_NOT_NULL(changed);
    TT_ASSERT(cJSON_IsTrue(changed), "modified file should return changed: true");

    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(result, "symbols"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(result, "imports"));

    /* Verify new function is searchable */
    cJSON_Delete(result);

    tt_cli_opts_t search_opts;
    init_opts(&search_opts);
    const char *spos[] = {"brand_new_function"};
    search_opts.positional = spos;
    search_opts.positional_count = 1;

    cJSON *sr = tt_cmd_search_symbols_exec(&search_opts);
    TT_ASSERT_NOT_NULL(sr);
    cJSON *results = cJSON_GetObjectItem(sr, "results");
    TT_ASSERT(cJSON_GetArraySize(results) > 0,
              "newly indexed function should be searchable");
    cJSON_Delete(sr);
}

TT_TEST(test_index_file_not_found)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    const char *pos[] = {"src/doesnt_exist.py"};
    opts.positional = pos;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_index_file_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    const char *err = cJSON_GetStringValue(cJSON_GetObjectItem(result, "error"));
    TT_ASSERT_EQ_STR(err, "file_not_found");

    cJSON_Delete(result);
}

TT_TEST(test_index_file_missing_argument)
{
    tt_cli_opts_t opts;
    init_opts(&opts);
    /* No positional args */

    cJSON *result = tt_cmd_index_file_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    const char *err = cJSON_GetStringValue(cJSON_GetObjectItem(result, "error"));
    TT_ASSERT_EQ_STR(err, "missing_argument");

    cJSON_Delete(result);
}

/* =================================================================
 * search:symbols detail levels
 * ================================================================= */

TT_TEST(test_search_detail_compact)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    const char *pos[] = {"func"};
    opts.positional = pos;
    opts.positional_count = 1;
    opts.detail = "compact";

    cJSON *result = tt_cmd_search_symbols_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    cJSON *results = cJSON_GetObjectItem(result, "results");
    if (cJSON_GetArraySize(results) > 0) {
        cJSON *first = cJSON_GetArrayItem(results, 0);
        /* Compact: id, name, kind, file, line */
        TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "id"));
        TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "name"));
        TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "kind"));
        TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "file"));
        TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "line"));
        /* Should NOT have qname, sig, summary, end_line */
        TT_ASSERT(cJSON_GetObjectItem(first, "qname") == NULL,
                  "compact should not have qname");
        TT_ASSERT(cJSON_GetObjectItem(first, "sig") == NULL,
                  "compact should not have sig");
        TT_ASSERT(cJSON_GetObjectItem(first, "summary") == NULL,
                  "compact should not have summary");
        TT_ASSERT(cJSON_GetObjectItem(first, "end_line") == NULL,
                  "compact should not have end_line");
        TT_ASSERT(cJSON_GetObjectItem(first, "source") == NULL,
                  "compact should not have source");
    }

    cJSON_Delete(result);
}

TT_TEST(test_search_detail_full)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    const char *pos[] = {"calculate_sum"};
    opts.positional = pos;
    opts.positional_count = 1;
    opts.detail = "full";

    cJSON *result = tt_cmd_search_symbols_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    cJSON *results = cJSON_GetObjectItem(result, "results");
    if (cJSON_GetArraySize(results) > 0) {
        cJSON *first = cJSON_GetArrayItem(results, 0);
        /* Full should have standard fields */
        TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "id"));
        TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "name"));
        TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "kind"));
        TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "file"));
        TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "line"));
        /* Full should have source code */
        cJSON *source = cJSON_GetObjectItem(first, "source");
        if (source) {
            const char *src_str = cJSON_GetStringValue(source);
            TT_ASSERT_NOT_NULL(src_str);
            TT_ASSERT(strlen(src_str) > 0, "full detail source should not be empty");
        }
    }

    cJSON_Delete(result);
}

TT_TEST(test_search_detail_standard_default)
{
    /* Default (no detail flag) should be standard */
    tt_cli_opts_t opts;
    init_opts(&opts);

    const char *pos[] = {"calculate_sum"};
    opts.positional = pos;
    opts.positional_count = 1;
    /* No detail, no compact */

    cJSON *result = tt_cmd_search_symbols_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    cJSON *results = cJSON_GetObjectItem(result, "results");
    if (cJSON_GetArraySize(results) > 0) {
        cJSON *first = cJSON_GetArrayItem(results, 0);
        /* Standard should NOT have source or end_line */
        TT_ASSERT(cJSON_GetObjectItem(first, "source") == NULL,
                  "standard should not have source");
        TT_ASSERT(cJSON_GetObjectItem(first, "end_line") == NULL,
                  "standard should not have end_line");
    }

    cJSON_Delete(result);
}

/* =================================================================
 * Token budget
 * ================================================================= */

TT_TEST(test_search_token_budget)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    const char *pos[] = {"func"};
    opts.positional = pos;
    opts.positional_count = 1;
    opts.token_budget = 100; /* very small budget */

    cJSON *result = tt_cmd_search_symbols_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    cJSON *results = cJSON_GetObjectItem(result, "results");
    TT_ASSERT_NOT_NULL(results);

    /* With a very small budget, should get fewer results than unlimited */
    int budget_count = cJSON_GetArraySize(results);

    /* token_budget metadata */
    cJSON *tb = cJSON_GetObjectItem(result, "token_budget");
    if (tb) {
        TT_ASSERT_EQ_INT((int)tb->valuedouble, 100);
    }

    cJSON_Delete(result);

    /* Compare with unlimited */
    tt_cli_opts_t opts2;
    init_opts(&opts2);
    const char *pos2[] = {"func"};
    opts2.positional = pos2;
    opts2.positional_count = 1;

    cJSON *result2 = tt_cmd_search_symbols_exec(&opts2);
    TT_ASSERT_NOT_NULL(result2);

    int unlimited_count = cJSON_GetArraySize(
        cJSON_GetObjectItem(result2, "results"));

    /* Budget should limit results (or at minimum return 1) */
    TT_ASSERT(budget_count >= 1,
              "token budget should return at least 1 result");
    TT_ASSERT(budget_count <= unlimited_count,
              "token budget should not return more than unlimited");

    cJSON_Delete(result2);
}

TT_TEST(test_search_token_budget_metadata)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    const char *pos[] = {"func"};
    opts.positional = pos;
    opts.positional_count = 1;
    opts.token_budget = 5000;

    cJSON *result = tt_cmd_search_symbols_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    cJSON *tb = cJSON_GetObjectItem(result, "token_budget");
    TT_ASSERT_NOT_NULL(tb);
    TT_ASSERT_EQ_INT((int)tb->valuedouble, 5000);

    cJSON_Delete(result);
}

/* =================================================================
 * Scope filters
 * ================================================================= */

TT_TEST(test_search_scope_imports_of)
{
    /* Scope to files imported by main.py */
    tt_cli_opts_t opts;
    init_opts(&opts);

    const char *pos[] = {"func"};
    opts.positional = pos;
    opts.positional_count = 1;
    opts.scope_imports_of = "src/main.py";

    cJSON *result = tt_cmd_search_symbols_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT(cJSON_GetObjectItem(result, "error") == NULL, "should not have error");

    cJSON *results = cJSON_GetObjectItem(result, "results");
    TT_ASSERT_NOT_NULL(results);

    /* All results should be from files imported by main.py (or main.py itself) */
    /* main.py imports utils -> results should include utils.py symbols */
    cJSON_Delete(result);
}

TT_TEST(test_search_scope_importers_of)
{
    /* Scope to files that import helpers.py */
    tt_cli_opts_t opts;
    init_opts(&opts);

    const char *pos[] = {"func"};
    opts.positional = pos;
    opts.positional_count = 1;
    opts.scope_importers_of = "src/helpers.py";

    cJSON *result = tt_cmd_search_symbols_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT(cJSON_GetObjectItem(result, "error") == NULL, "should not have error");

    cJSON_Delete(result);
}

TT_TEST(test_search_scope_both_error)
{
    /* Both scope flags set -> should return error */
    tt_cli_opts_t opts;
    init_opts(&opts);

    const char *pos[] = {"func"};
    opts.positional = pos;
    opts.positional_count = 1;
    opts.scope_imports_of = "src/main.py";
    opts.scope_importers_of = "src/helpers.py";

    cJSON *result = tt_cmd_search_symbols_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    const char *err = cJSON_GetStringValue(cJSON_GetObjectItem(result, "error"));
    TT_ASSERT_NOT_NULL(err);
    /* Should get invalid_scope or similar error */

    cJSON_Delete(result);
}

/* =================================================================
 * Multi-symbol bundle
 * ================================================================= */

TT_TEST(test_bundle_single_backward_compat)
{
    /* Single ID should return flat structure (backward compatible) */
    char *sym_id = find_symbol_id("calculate_sum", "function");
    if (!sym_id) {
        TT_ASSERT(0, "could not find calculate_sum symbol");
        return;
    }

    tt_cli_opts_t opts;
    init_opts(&opts);
    const char *pos[] = {sym_id};
    opts.positional = pos;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_inspect_bundle_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT(cJSON_GetObjectItem(result, "error") == NULL, "should not have error");

    /* Flat structure: definition, outline */
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(result, "definition"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(result, "outline"));

    /* Should NOT have "symbols" array (that's multi-mode) */
    TT_ASSERT(cJSON_GetObjectItem(result, "symbols") == NULL,
              "single ID should not have symbols array");

    cJSON_Delete(result);
    free(sym_id);
}

TT_TEST(test_bundle_multi_symbol)
{
    /* Multi-symbol: comma-separated IDs */
    char *id1 = find_symbol_id("calculate_sum", "function");
    char *id2 = find_symbol_id("format_value", "function");
    if (!id1 || !id2) {
        free(id1);
        free(id2);
        TT_ASSERT(0, "could not find symbols for multi-bundle test");
        return;
    }

    /* Build comma-separated ID */
    char combined[512];
    snprintf(combined, sizeof(combined), "%s,%s", id1, id2);

    tt_cli_opts_t opts;
    init_opts(&opts);
    const char *pos[] = {combined};
    opts.positional = pos;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_inspect_bundle_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT(cJSON_GetObjectItem(result, "error") == NULL, "should not have error");

    /* Multi structure: symbol_count, symbols array */
    cJSON *count = cJSON_GetObjectItem(result, "symbol_count");
    TT_ASSERT_NOT_NULL(count);
    TT_ASSERT_EQ_INT((int)count->valuedouble, 2);

    cJSON *symbols = cJSON_GetObjectItem(result, "symbols");
    TT_ASSERT_NOT_NULL(symbols);
    TT_ASSERT(cJSON_IsArray(symbols), "symbols should be array");
    TT_ASSERT_EQ_INT(cJSON_GetArraySize(symbols), 2);

    cJSON_Delete(result);
    free(id1);
    free(id2);
}

TT_TEST(test_bundle_multi_with_invalid)
{
    /* One valid + one invalid ID */
    char *id1 = find_symbol_id("calculate_sum", "function");
    if (!id1) {
        TT_ASSERT(0, "could not find calculate_sum");
        return;
    }

    char combined[512];
    snprintf(combined, sizeof(combined), "%s,nonexistent::fake#function", id1);

    tt_cli_opts_t opts;
    init_opts(&opts);
    const char *pos[] = {combined};
    opts.positional = pos;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_inspect_bundle_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    /* Should have symbols array with 1 entry and errors array with 1 entry */
    cJSON *symbols = cJSON_GetObjectItem(result, "symbols");
    TT_ASSERT_NOT_NULL(symbols);
    TT_ASSERT_EQ_INT(cJSON_GetArraySize(symbols), 1);

    cJSON *errors = cJSON_GetObjectItem(result, "errors");
    TT_ASSERT_NOT_NULL(errors);
    TT_ASSERT_EQ_INT(cJSON_GetArraySize(errors), 1);

    cJSON_Delete(result);
    free(id1);
}

/* =================================================================
 * Markdown bundle output
 * ================================================================= */

TT_TEST(test_bundle_markdown)
{
    char *sym_id = find_symbol_id("calculate_sum", "function");
    if (!sym_id) {
        TT_ASSERT(0, "could not find calculate_sum symbol");
        return;
    }

    tt_cli_opts_t opts;
    init_opts(&opts);
    const char *pos[] = {sym_id};
    opts.positional = pos;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_inspect_bundle_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    char *md = tt_bundle_render_markdown(result);
    TT_ASSERT_NOT_NULL(md);
    TT_ASSERT(strlen(md) > 0, "markdown should not be empty");

    /* Check for expected markdown sections */
    TT_ASSERT_STR_CONTAINS(md, "## Definition");
    TT_ASSERT_STR_CONTAINS(md, "calculate_sum");

    free(md);
    cJSON_Delete(result);
    free(sym_id);
}

/* =================================================================
 * include_callers on bundle
 * ================================================================= */

TT_TEST(test_bundle_include_callers)
{
    char *sym_id = find_symbol_id("format_value", "function");
    if (!sym_id) {
        TT_ASSERT(0, "could not find format_value symbol");
        return;
    }

    tt_cli_opts_t opts;
    init_opts(&opts);
    const char *pos[] = {sym_id};
    opts.positional = pos;
    opts.positional_count = 1;
    opts.include_callers = true;

    cJSON *result = tt_cmd_inspect_bundle_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT(cJSON_GetObjectItem(result, "error") == NULL, "should not have error");

    /* Callers key is only added when caller_count > 0.
     * For Python functions, ctags-based callers may or may not be found.
     * The key correctness check: include_callers=false should never have callers. */
    cJSON_Delete(result);

    /* Verify without flag: callers should NOT be present */
    tt_cli_opts_t opts2;
    init_opts(&opts2);
    const char *pos2[] = {sym_id};
    opts2.positional = pos2;
    opts2.positional_count = 1;
    opts2.include_callers = false;

    cJSON *result2 = tt_cmd_inspect_bundle_exec(&opts2);
    TT_ASSERT_NOT_NULL(result2);
    TT_ASSERT(cJSON_GetObjectItem(result2, "callers") == NULL,
              "without include_callers, callers key should not be present");

    cJSON_Delete(result2);
    free(sym_id);
}

/* =================================================================
 * has_importers on find:importers
 * ================================================================= */

TT_TEST(test_find_importers_has_importers)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    const char *pos[] = {"src/helpers.py"};
    opts.positional = pos;
    opts.positional_count = 1;
    opts.has_importers = true;

    cJSON *result = tt_cmd_find_importers_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT(cJSON_GetObjectItem(result, "error") == NULL, "should not have error");

    cJSON *importers = cJSON_GetObjectItem(result, "importers");
    TT_ASSERT_NOT_NULL(importers);

    /* Each importer should have has_importers boolean */
    cJSON *item;
    cJSON_ArrayForEach(item, importers) {
        cJSON *hi = cJSON_GetObjectItem(item, "has_importers");
        TT_ASSERT_NOT_NULL(hi);
        TT_ASSERT(cJSON_IsBool(hi), "has_importers should be boolean");
    }

    cJSON_Delete(result);
}

TT_TEST(test_find_importers_without_flag)
{
    /* Without --has-importers, results should NOT have has_importers field */
    tt_cli_opts_t opts;
    init_opts(&opts);

    const char *pos[] = {"src/helpers.py"};
    opts.positional = pos;
    opts.positional_count = 1;
    /* has_importers NOT set */

    cJSON *result = tt_cmd_find_importers_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    cJSON *importers = cJSON_GetObjectItem(result, "importers");
    if (importers && cJSON_GetArraySize(importers) > 0) {
        cJSON *first = cJSON_GetArrayItem(importers, 0);
        TT_ASSERT(cJSON_GetObjectItem(first, "has_importers") == NULL,
                  "without flag, should not have has_importers field");
    }

    cJSON_Delete(result);
}

/* =================================================================
 * Runner
 * ================================================================= */

void run_int_v020_tests(void)
{
    setup_fixture();
    fprintf(stderr, "  Indexing v0.2.0 fixture...\n");
    index_fixture();

    /* find:dead */
    TT_RUN(test_find_dead_basic);
    TT_RUN(test_find_dead_orphan_is_dead);
    TT_RUN(test_find_dead_entry_point_excluded);
    TT_RUN(test_find_dead_test_prefix_excluded);
    TT_RUN(test_find_dead_exclude_tests);
    TT_RUN(test_find_dead_kind_filter);
    TT_RUN(test_find_dead_no_index);
    TT_RUN(test_find_dead_confidence_levels);
    TT_RUN(test_find_dead_limit);

    /* inspect:cycles */
    TT_RUN(test_inspect_cycles_basic);
    TT_RUN(test_inspect_cycles_min_length);
    TT_RUN(test_inspect_cycles_cross_dir);
    TT_RUN(test_inspect_cycles_no_index);
    TT_RUN(test_inspect_cycles_no_cycles);

    /* inspect:blast */
    TT_RUN(test_inspect_blast_basic);
    TT_RUN(test_inspect_blast_depth_limit);
    TT_RUN(test_inspect_blast_not_found);
    TT_RUN(test_inspect_blast_confirmed_has_references);

    /* index:file */
    TT_RUN(test_index_file_no_index);
    TT_RUN(test_index_file_unchanged);
    TT_RUN(test_index_file_modified);
    TT_RUN(test_index_file_not_found);
    TT_RUN(test_index_file_missing_argument);

    /* search:symbols detail levels */
    TT_RUN(test_search_detail_compact);
    TT_RUN(test_search_detail_full);
    TT_RUN(test_search_detail_standard_default);

    /* token budget */
    TT_RUN(test_search_token_budget);
    TT_RUN(test_search_token_budget_metadata);

    /* scope filters */
    TT_RUN(test_search_scope_imports_of);
    TT_RUN(test_search_scope_importers_of);
    TT_RUN(test_search_scope_both_error);

    /* multi-symbol bundle */
    TT_RUN(test_bundle_single_backward_compat);
    TT_RUN(test_bundle_multi_symbol);
    TT_RUN(test_bundle_multi_with_invalid);

    /* markdown output */
    TT_RUN(test_bundle_markdown);

    /* include_callers */
    TT_RUN(test_bundle_include_callers);

    /* has_importers */
    TT_RUN(test_find_importers_has_importers);
    TT_RUN(test_find_importers_without_flag);

    cleanup_fixture();
}
