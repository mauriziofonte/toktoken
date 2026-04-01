/*
 * test_cli.c -- Unit tests for CLI argument parser.
 */

#include "test_framework.h"
#include "cli.h"

#include <stdlib.h>
#include <string.h>

/* Helper: build argc/argv from a string literal list.
 * argv[0] = "toktoken", argv[1] = "cmd", then the rest. */
#define PARSE(...) do { \
    char *_args[] = { "toktoken", "cmd", __VA_ARGS__ }; \
    int _argc = (int)(sizeof(_args) / sizeof(_args[0])); \
    rc = tt_cli_parse(&opts, _argc, _args); \
} while(0)

#define PARSE_NO_CMD(...) do { \
    char *_args[] = { "toktoken", __VA_ARGS__ }; \
    int _argc = (int)(sizeof(_args) / sizeof(_args[0])); \
    rc = tt_cli_parse(&opts, _argc, _args); \
} while(0)

/* ---- Long flag tests ---- */

TT_TEST(test_cli_long_bool_compact)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("--compact");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.compact);
    TT_ASSERT_FALSE(opts.count);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_long_bool_count)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("--count");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.count);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_long_bool_version)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("--version");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.version);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_long_bool_help)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("--help");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.help);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_long_bool_regex)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("--regex");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.regex);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_long_bool_case_sensitive)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("--case-sensitive");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.case_sensitive);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_long_bool_debug)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("--debug");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.debug);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_long_bool_all)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("--all");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.all);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_long_bool_force)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("--force");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.force);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_long_bool_full)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("--full");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.full);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_long_bool_unique)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("--unique");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.unique);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_long_bool_no_sig)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("--no-sig");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.no_sig);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_long_bool_no_summary)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("--no-summary");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.no_summary);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_long_bool_full_clone)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("--full-clone");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.full_clone);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_long_bool_update_only)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("--update-only");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.update_only);
    tt_cli_opts_free(&opts);
}

/* ---- Long value flag tests ---- */

TT_TEST(test_cli_long_value_path)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("--path", "/tmp/foo");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_STR("/tmp/foo", opts.path);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_long_value_format)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("--format", "table");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_STR("table", opts.format);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_long_value_limit)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("--limit", "42");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_INT(42, opts.limit);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_long_value_kind)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("--kind", "function,class");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_STR("function,class", opts.kind);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_long_value_language)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("--language", "python");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_STR("python", opts.language);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_long_value_context)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("--context", "5");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_INT(5, opts.context);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_long_value_depth)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("--depth", "3");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_INT(3, opts.depth);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_long_value_max_files)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("--max-files", "10000");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_INT(10000, opts.max_files);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_long_value_lines)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("--lines", "10-50");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_STR("10-50", opts.lines);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_long_value_group_by)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("--group-by", "file");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_STR("file", opts.group_by);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_long_value_sort)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("--sort", "name");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_STR("name", opts.sort);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_long_value_exclude)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("--exclude", "vendor|node_modules");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_STR("vendor|node_modules", opts.exclude);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_long_value_filter)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("--filter", "src|lib");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_STR("src|lib", opts.filter);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_long_value_branch)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("--branch", "develop");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_STR("develop", opts.branch);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_long_value_truncate_clamp)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("--truncate", "5");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_INT(20, opts.truncate_width); /* clamped to min 20 */
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_long_value_truncate_valid)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("--truncate", "200");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_INT(200, opts.truncate_width);
    tt_cli_opts_free(&opts);
}

/* ---- Short flag tests ---- */

TT_TEST(test_cli_short_compact)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-c");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.compact);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_short_count)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-n");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.count);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_short_version)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-v");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.version);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_short_help)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-h");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.help);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_short_unique)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-u");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.unique);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_short_regex)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-r");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.regex);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_short_case_sensitive)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-s");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.case_sensitive);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_short_debug)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-D");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.debug);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_short_all)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-a");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.all);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_short_force)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-F");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.force);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_short_full)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-f");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.full);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_short_limit_separate)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-l", "25");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_INT(25, opts.limit);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_short_limit_attached)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-l25");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_INT(25, opts.limit);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_short_context_separate)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-C", "3");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_INT(3, opts.context);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_short_context_attached)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-C3");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_INT(3, opts.context);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_short_depth_separate)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-d", "2");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_INT(2, opts.depth);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_short_depth_attached)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-d2");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_INT(2, opts.depth);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_short_max_files_separate)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-m", "5000");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_INT(5000, opts.max_files);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_short_max_files_attached)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-m5000");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_INT(5000, opts.max_files);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_short_path_separate)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-p", "/tmp/project");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_STR("/tmp/project", opts.path);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_short_path_attached)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-p/tmp/project");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_STR("/tmp/project", opts.path);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_short_kind_separate)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-k", "function");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_STR("function", opts.kind);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_short_language_separate)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-L", "c");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_STR("c", opts.language);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_short_format_separate)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-o", "table");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_STR("table", opts.format);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_short_exclude_separate)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-e", "vendor");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_STR("vendor", opts.exclude);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_short_sort_separate)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-S", "name");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_STR("name", opts.sort);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_short_group_by_separate)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-g", "file");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_STR("file", opts.group_by);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_short_truncate_separate)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-t", "80");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_INT(80, opts.truncate_width);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_short_truncate_attached_clamp)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-t5");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_INT(20, opts.truncate_width); /* clamped */
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_short_ignore_separate)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-i", "vendor");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_INT(1, opts.ignore_count);
    TT_ASSERT_EQ_STR("vendor", opts.ignore_patterns[0]);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_short_ignore_attached)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-ivendor");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_INT(1, opts.ignore_count);
    TT_ASSERT_EQ_STR("vendor", opts.ignore_patterns[0]);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_short_branch_separate)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-b", "main");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_STR("main", opts.branch);
    tt_cli_opts_free(&opts);
}

/* ---- Aggregated short flags ---- */

TT_TEST(test_cli_aggregated_booleans)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-cn");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.compact);
    TT_ASSERT_TRUE(opts.count);
    TT_ASSERT_FALSE(opts.unique);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_aggregated_three_booleans)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-cnu");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.compact);
    TT_ASSERT_TRUE(opts.count);
    TT_ASSERT_TRUE(opts.unique);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_aggregated_four_booleans)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-cnrs");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.compact);
    TT_ASSERT_TRUE(opts.count);
    TT_ASSERT_TRUE(opts.regex);
    TT_ASSERT_TRUE(opts.case_sensitive);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_aggregated_bools_then_value_attached)
{
    /* -cnl10: compact + count + limit=10 */
    tt_cli_opts_t opts;
    int rc;
    PARSE("-cnl10");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.compact);
    TT_ASSERT_TRUE(opts.count);
    TT_ASSERT_EQ_INT(10, opts.limit);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_aggregated_bools_then_value_separate)
{
    /* -cn -l 10: compact + count, then limit=10 */
    tt_cli_opts_t opts;
    int rc;
    PARSE("-cn", "-l", "10");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.compact);
    TT_ASSERT_TRUE(opts.count);
    TT_ASSERT_EQ_INT(10, opts.limit);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_aggregated_value_stops_aggregation)
{
    /* -cl10n should NOT parse 'n' as boolean -- 'l' consumes "10n" as value */
    tt_cli_opts_t opts;
    int rc;
    PARSE("-cl10");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.compact);
    TT_ASSERT_EQ_INT(10, opts.limit);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_aggregated_with_string_value)
{
    /* -ck function: compact + kind=function */
    tt_cli_opts_t opts;
    int rc;
    PARSE("-ck", "function");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.compact);
    TT_ASSERT_EQ_STR("function", opts.kind);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_aggregated_with_string_attached)
{
    /* -ckfunction: compact + kind="function" */
    tt_cli_opts_t opts;
    int rc;
    PARSE("-ckfunction");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.compact);
    TT_ASSERT_EQ_STR("function", opts.kind);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_aggregated_bool_mixed_case)
{
    /* -cDFf: compact + debug + force + full */
    tt_cli_opts_t opts;
    int rc;
    PARSE("-cDFf");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.compact);
    TT_ASSERT_TRUE(opts.debug);
    TT_ASSERT_TRUE(opts.force);
    TT_ASSERT_TRUE(opts.full);
    tt_cli_opts_free(&opts);
}

/* ---- Mixed long and short flags ---- */

TT_TEST(test_cli_mixed_long_short)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("--compact", "-l", "20", "--kind", "function", "-D");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.compact);
    TT_ASSERT_EQ_INT(20, opts.limit);
    TT_ASSERT_EQ_STR("function", opts.kind);
    TT_ASSERT_TRUE(opts.debug);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_mixed_aggregated_with_long)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-cn", "--max-files", "5000", "-l10", "--full");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.compact);
    TT_ASSERT_TRUE(opts.count);
    TT_ASSERT_EQ_INT(5000, opts.max_files);
    TT_ASSERT_EQ_INT(10, opts.limit);
    TT_ASSERT_TRUE(opts.full);
    tt_cli_opts_free(&opts);
}

/* ---- Positional arguments ---- */

TT_TEST(test_cli_positional_single)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("myquery");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_INT(1, opts.positional_count);
    TT_ASSERT_EQ_STR("myquery", opts.positional[0]);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_positional_with_flags)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-c", "myquery", "--limit", "10");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.compact);
    TT_ASSERT_EQ_INT(10, opts.limit);
    TT_ASSERT_EQ_INT(1, opts.positional_count);
    TT_ASSERT_EQ_STR("myquery", opts.positional[0]);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_positional_multiple)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("arg1", "arg2", "arg3");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_INT(3, opts.positional_count);
    TT_ASSERT_EQ_STR("arg1", opts.positional[0]);
    TT_ASSERT_EQ_STR("arg2", opts.positional[1]);
    TT_ASSERT_EQ_STR("arg3", opts.positional[2]);
    tt_cli_opts_free(&opts);
}

/* ---- Repeatable --ignore / -i ---- */

TT_TEST(test_cli_ignore_repeatable_long)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("--ignore", "vendor", "--ignore", "node_modules", "--ignore", ".cache");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_INT(3, opts.ignore_count);
    TT_ASSERT_EQ_STR("vendor", opts.ignore_patterns[0]);
    TT_ASSERT_EQ_STR("node_modules", opts.ignore_patterns[1]);
    TT_ASSERT_EQ_STR(".cache", opts.ignore_patterns[2]);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_ignore_repeatable_short)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-i", "vendor", "-i", "dist");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_INT(2, opts.ignore_count);
    TT_ASSERT_EQ_STR("vendor", opts.ignore_patterns[0]);
    TT_ASSERT_EQ_STR("dist", opts.ignore_patterns[1]);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_ignore_repeatable_mixed)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-ivendor", "--ignore", "dist");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_INT(2, opts.ignore_count);
    TT_ASSERT_EQ_STR("vendor", opts.ignore_patterns[0]);
    TT_ASSERT_EQ_STR("dist", opts.ignore_patterns[1]);
    tt_cli_opts_free(&opts);
}

/* ---- Default values ---- */

TT_TEST(test_cli_defaults)
{
    tt_cli_opts_t opts;
    int rc;
    char *args[] = { "toktoken", "cmd" };
    rc = tt_cli_parse(&opts, 2, args);
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_INT(120, opts.truncate_width);
    TT_ASSERT_EQ_INT(0, opts.limit);
    TT_ASSERT_EQ_INT(0, opts.context);
    TT_ASSERT_EQ_INT(0, opts.depth);
    TT_ASSERT_EQ_INT(0, opts.max_files);
    TT_ASSERT_FALSE(opts.compact);
    TT_ASSERT_FALSE(opts.count);
    TT_ASSERT_FALSE(opts.regex);
    TT_ASSERT_FALSE(opts.debug);
    TT_ASSERT_FALSE(opts.full);
    TT_ASSERT_NULL(opts.path);
    TT_ASSERT_NULL(opts.kind);
    TT_ASSERT_NULL(opts.language);
    TT_ASSERT_EQ_INT(0, opts.positional_count);
    TT_ASSERT_EQ_INT(0, opts.ignore_count);
    tt_cli_opts_free(&opts);
}

/* ---- Error handling ---- */

TT_TEST(test_cli_unknown_long_flag)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("--nonexistent");
    TT_ASSERT_EQ_INT(-1, rc);
}

TT_TEST(test_cli_unknown_short_flag)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-Z");
    TT_ASSERT_EQ_INT(-1, rc);
}

TT_TEST(test_cli_unknown_in_aggregation)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("-cZn");
    TT_ASSERT_EQ_INT(-1, rc);
}

/* ---- Negative number as value ---- */

TT_TEST(test_cli_negative_number_value)
{
    tt_cli_opts_t opts;
    int rc;
    PARSE("--limit", "-5");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_INT(-5, opts.limit);
    tt_cli_opts_free(&opts);
}

/* ---- Complex real-world combinations ---- */

TT_TEST(test_cli_real_world_search)
{
    /* toktoken search:symbols "kmalloc" -cnl10 --kind function -Lc */
    tt_cli_opts_t opts;
    int rc;
    PARSE("kmalloc", "-cnl10", "--kind", "function", "-Lc");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.compact);
    TT_ASSERT_TRUE(opts.count);
    TT_ASSERT_EQ_INT(10, opts.limit);
    TT_ASSERT_EQ_STR("function", opts.kind);
    TT_ASSERT_EQ_STR("c", opts.language);
    TT_ASSERT_EQ_INT(1, opts.positional_count);
    TT_ASSERT_EQ_STR("kmalloc", opts.positional[0]);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_real_world_index_create)
{
    /* toktoken index:create -fm10000 -i vendor -i dist --languages c,python */
    tt_cli_opts_t opts;
    int rc;
    PARSE("-fm10000", "-i", "vendor", "-i", "dist", "--languages", "c,python");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.full);
    TT_ASSERT_EQ_INT(10000, opts.max_files);
    TT_ASSERT_EQ_INT(2, opts.ignore_count);
    TT_ASSERT_EQ_STR("vendor", opts.ignore_patterns[0]);
    TT_ASSERT_EQ_STR("dist", opts.ignore_patterns[1]);
    TT_ASSERT_EQ_STR("c,python", opts.languages);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_real_world_text_search)
{
    /* toktoken search:text "BUG_ON" -cg file -l20 */
    tt_cli_opts_t opts;
    int rc;
    PARSE("BUG_ON", "-cg", "file", "-l20");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.compact);
    TT_ASSERT_EQ_STR("file", opts.group_by);
    TT_ASSERT_EQ_INT(20, opts.limit);
    TT_ASSERT_EQ_INT(1, opts.positional_count);
    TT_ASSERT_EQ_STR("BUG_ON", opts.positional[0]);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_real_world_inspect)
{
    /* toktoken inspect:tree -cd3 */
    tt_cli_opts_t opts;
    int rc;
    PARSE("-cd3");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.compact);
    TT_ASSERT_EQ_INT(3, opts.depth);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_real_world_all_booleans)
{
    /* toktoken search:symbols "foo" -cnursDfa */
    tt_cli_opts_t opts;
    int rc;
    PARSE("foo", "-cnursDfa");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(opts.compact);
    TT_ASSERT_TRUE(opts.count);
    TT_ASSERT_TRUE(opts.unique);
    TT_ASSERT_TRUE(opts.regex);
    TT_ASSERT_TRUE(opts.case_sensitive);
    TT_ASSERT_TRUE(opts.debug);
    TT_ASSERT_TRUE(opts.full);
    TT_ASSERT_TRUE(opts.all);
    TT_ASSERT_EQ_INT(1, opts.positional_count);
    TT_ASSERT_EQ_STR("foo", opts.positional[0]);
    tt_cli_opts_free(&opts);
}

/* ---- Edge cases ---- */

TT_TEST(test_cli_no_args)
{
    tt_cli_opts_t opts;
    char *args[] = { "toktoken" };
    int rc = tt_cli_parse(&opts, 1, args);
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_INT(0, opts.positional_count);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_only_command)
{
    tt_cli_opts_t opts;
    char *args[] = { "toktoken", "stats" };
    int rc = tt_cli_parse(&opts, 2, args);
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_INT(0, opts.positional_count);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_value_flag_at_end_no_value)
{
    /* --limit at end with no value: silently ignored */
    tt_cli_opts_t opts;
    int rc;
    PARSE("--limit");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_INT(0, opts.limit); /* default stays */
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_short_value_at_end_no_value)
{
    /* -l at end with no value: silently ignored */
    tt_cli_opts_t opts;
    int rc;
    PARSE("-l");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_INT(0, opts.limit);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_include_flag)
{
    /* --include vendor -I node_modules */
    tt_cli_opts_t opts;
    int rc;
    PARSE("--include", "vendor", "-I", "node_modules");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_INT(2, opts.include_count);
    TT_ASSERT_EQ_STR("vendor", opts.include_patterns[0]);
    TT_ASSERT_EQ_STR("node_modules", opts.include_patterns[1]);
    tt_cli_opts_free(&opts);
}

TT_TEST(test_cli_include_with_ignore)
{
    /* --include vendor -i dist */
    tt_cli_opts_t opts;
    int rc;
    PARSE("--include", "vendor", "-i", "dist");
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_EQ_INT(1, opts.include_count);
    TT_ASSERT_EQ_STR("vendor", opts.include_patterns[0]);
    TT_ASSERT_EQ_INT(1, opts.ignore_count);
    TT_ASSERT_EQ_STR("dist", opts.ignore_patterns[0]);
    tt_cli_opts_free(&opts);
}

void run_cli_tests(void)
{
    /* Long boolean flags */
    TT_RUN(test_cli_long_bool_compact);
    TT_RUN(test_cli_long_bool_count);
    TT_RUN(test_cli_long_bool_version);
    TT_RUN(test_cli_long_bool_help);
    TT_RUN(test_cli_long_bool_regex);
    TT_RUN(test_cli_long_bool_case_sensitive);
    TT_RUN(test_cli_long_bool_debug);
    TT_RUN(test_cli_long_bool_all);
    TT_RUN(test_cli_long_bool_force);
    TT_RUN(test_cli_long_bool_full);
    TT_RUN(test_cli_long_bool_unique);
    TT_RUN(test_cli_long_bool_no_sig);
    TT_RUN(test_cli_long_bool_no_summary);
    TT_RUN(test_cli_long_bool_full_clone);
    TT_RUN(test_cli_long_bool_update_only);

    /* Long value flags */
    TT_RUN(test_cli_long_value_path);
    TT_RUN(test_cli_long_value_format);
    TT_RUN(test_cli_long_value_limit);
    TT_RUN(test_cli_long_value_kind);
    TT_RUN(test_cli_long_value_language);
    TT_RUN(test_cli_long_value_context);
    TT_RUN(test_cli_long_value_depth);
    TT_RUN(test_cli_long_value_max_files);
    TT_RUN(test_cli_long_value_lines);
    TT_RUN(test_cli_long_value_group_by);
    TT_RUN(test_cli_long_value_sort);
    TT_RUN(test_cli_long_value_exclude);
    TT_RUN(test_cli_long_value_filter);
    TT_RUN(test_cli_long_value_branch);
    TT_RUN(test_cli_long_value_truncate_clamp);
    TT_RUN(test_cli_long_value_truncate_valid);

    /* Short flags */
    TT_RUN(test_cli_short_compact);
    TT_RUN(test_cli_short_count);
    TT_RUN(test_cli_short_version);
    TT_RUN(test_cli_short_help);
    TT_RUN(test_cli_short_unique);
    TT_RUN(test_cli_short_regex);
    TT_RUN(test_cli_short_case_sensitive);
    TT_RUN(test_cli_short_debug);
    TT_RUN(test_cli_short_all);
    TT_RUN(test_cli_short_force);
    TT_RUN(test_cli_short_full);
    TT_RUN(test_cli_short_limit_separate);
    TT_RUN(test_cli_short_limit_attached);
    TT_RUN(test_cli_short_context_separate);
    TT_RUN(test_cli_short_context_attached);
    TT_RUN(test_cli_short_depth_separate);
    TT_RUN(test_cli_short_depth_attached);
    TT_RUN(test_cli_short_max_files_separate);
    TT_RUN(test_cli_short_max_files_attached);
    TT_RUN(test_cli_short_path_separate);
    TT_RUN(test_cli_short_path_attached);
    TT_RUN(test_cli_short_kind_separate);
    TT_RUN(test_cli_short_language_separate);
    TT_RUN(test_cli_short_format_separate);
    TT_RUN(test_cli_short_exclude_separate);
    TT_RUN(test_cli_short_sort_separate);
    TT_RUN(test_cli_short_group_by_separate);
    TT_RUN(test_cli_short_truncate_separate);
    TT_RUN(test_cli_short_truncate_attached_clamp);
    TT_RUN(test_cli_short_ignore_separate);
    TT_RUN(test_cli_short_ignore_attached);
    TT_RUN(test_cli_short_branch_separate);

    /* Aggregated short flags */
    TT_RUN(test_cli_aggregated_booleans);
    TT_RUN(test_cli_aggregated_three_booleans);
    TT_RUN(test_cli_aggregated_four_booleans);
    TT_RUN(test_cli_aggregated_bools_then_value_attached);
    TT_RUN(test_cli_aggregated_bools_then_value_separate);
    TT_RUN(test_cli_aggregated_value_stops_aggregation);
    TT_RUN(test_cli_aggregated_with_string_value);
    TT_RUN(test_cli_aggregated_with_string_attached);
    TT_RUN(test_cli_aggregated_bool_mixed_case);

    /* Mixed long + short */
    TT_RUN(test_cli_mixed_long_short);
    TT_RUN(test_cli_mixed_aggregated_with_long);

    /* Positional args */
    TT_RUN(test_cli_positional_single);
    TT_RUN(test_cli_positional_with_flags);
    TT_RUN(test_cli_positional_multiple);

    /* Repeatable --ignore */
    TT_RUN(test_cli_ignore_repeatable_long);
    TT_RUN(test_cli_ignore_repeatable_short);
    TT_RUN(test_cli_ignore_repeatable_mixed);

    /* Defaults */
    TT_RUN(test_cli_defaults);

    /* Error handling */
    TT_RUN(test_cli_unknown_long_flag);
    TT_RUN(test_cli_unknown_short_flag);
    TT_RUN(test_cli_unknown_in_aggregation);

    /* Edge cases */
    TT_RUN(test_cli_negative_number_value);
    TT_RUN(test_cli_real_world_search);
    TT_RUN(test_cli_real_world_index_create);
    TT_RUN(test_cli_real_world_text_search);
    TT_RUN(test_cli_real_world_inspect);
    TT_RUN(test_cli_real_world_all_booleans);
    TT_RUN(test_cli_no_args);
    TT_RUN(test_cli_only_command);
    TT_RUN(test_cli_value_flag_at_end_no_value);
    TT_RUN(test_cli_short_value_at_end_no_value);

    /* --include / -I */
    TT_RUN(test_cli_include_flag);
    TT_RUN(test_cli_include_with_ignore);
}
