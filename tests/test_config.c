/*
 * test_config.c -- Unit tests for config module.
 */

#include "test_framework.h"
#include "test_helpers.h"
#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "test_compat.h"

TT_TEST(test_cfg_default_values)
{
    tt_config_t config;
    tt_config_load(&config, "/nonexistent_path_xyz");

    TT_ASSERT_EQ_INT(2048, config.max_file_size_kb);
    TT_ASSERT_EQ_INT(200000, config.max_files);
    TT_ASSERT_EQ_INT(7, config.staleness_days);
    TT_ASSERT_EQ_INT(120, config.ctags_timeout_seconds);
    TT_ASSERT_EQ_INT(0, config.extra_ignore_count);
    TT_ASSERT_EQ_INT(0, config.language_count);
    TT_ASSERT_EQ_STR("info", config.log_level);

    tt_config_free(&config);
}

TT_TEST(test_cfg_project_config_overrides_index)
{
    char *tmpdir = tt_test_tmpdir();
    TT_ASSERT_NOT_NULL(tmpdir);

    tt_test_write_file(tmpdir, ".toktoken.json",
        "{\"index\":{\"max_file_size_kb\":1000,\"max_files\":5000}}");

    tt_config_t config;
    tt_config_load(&config, tmpdir);

    TT_ASSERT_EQ_INT(1000, config.max_file_size_kb);
    TT_ASSERT_EQ_INT(5000, config.max_files);
    /* Non-overridden defaults remain */
    TT_ASSERT_EQ_INT(7, config.staleness_days);

    tt_config_free(&config);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_cfg_project_config_only_index_section)
{
    char *tmpdir = tt_test_tmpdir();
    TT_ASSERT_NOT_NULL(tmpdir);

    /* Project config should only affect index section, not logging */
    tt_test_write_file(tmpdir, ".toktoken.json",
        "{\"logging\":{\"level\":\"debug\"}}");

    tt_config_t config;
    tt_config_load(&config, tmpdir);

    /* logging section from project config should be ignored */
    TT_ASSERT_EQ_STR("info", config.log_level);

    tt_config_free(&config);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_cfg_invalid_json_ignored)
{
    char *tmpdir = tt_test_tmpdir();
    TT_ASSERT_NOT_NULL(tmpdir);

    tt_test_write_file(tmpdir, ".toktoken.json", "not valid json {{{");

    tt_config_t config;
    tt_config_load(&config, tmpdir);

    TT_ASSERT_EQ_INT(2048, config.max_file_size_kb);

    tt_config_free(&config);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_cfg_empty_file_ignored)
{
    char *tmpdir = tt_test_tmpdir();
    TT_ASSERT_NOT_NULL(tmpdir);

    tt_test_write_file(tmpdir, ".toktoken.json", "");

    tt_config_t config;
    tt_config_load(&config, tmpdir);

    TT_ASSERT_EQ_INT(2048, config.max_file_size_kb);

    tt_config_free(&config);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_cfg_non_object_index_ignored)
{
    char *tmpdir = tt_test_tmpdir();
    TT_ASSERT_NOT_NULL(tmpdir);

    tt_test_write_file(tmpdir, ".toktoken.json",
        "{\"index\":\"not-an-object\"}");

    tt_config_t config;
    tt_config_load(&config, tmpdir);

    TT_ASSERT_EQ_INT(2048, config.max_file_size_kb);

    tt_config_free(&config);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_cfg_extra_ignore_comma_separated)
{
    setenv("TOKTOKEN_EXTRA_IGNORE", "vendor,node_modules,.cache", 1);

    tt_config_t config;
    tt_config_load(&config, "/nonexistent");

    TT_ASSERT_GE_INT(config.extra_ignore_count, 3);

    int found_vendor = 0, found_nm = 0, found_cache = 0;
    for (int i = 0; i < config.extra_ignore_count; i++) {
        if (strcmp(config.extra_ignore_patterns[i], "vendor") == 0) found_vendor = 1;
        if (strcmp(config.extra_ignore_patterns[i], "node_modules") == 0) found_nm = 1;
        if (strcmp(config.extra_ignore_patterns[i], ".cache") == 0) found_cache = 1;
    }
    TT_ASSERT(found_vendor, "should contain vendor");
    TT_ASSERT(found_nm, "should contain node_modules");
    TT_ASSERT(found_cache, "should contain .cache");

    tt_config_free(&config);
    unsetenv("TOKTOKEN_EXTRA_IGNORE");
}

TT_TEST(test_cfg_extra_ignore_json_array)
{
    setenv("TOKTOKEN_EXTRA_IGNORE", "[\"dist\",\"build\"]", 1);

    tt_config_t config;
    tt_config_load(&config, "/nonexistent");

    int found_dist = 0, found_build = 0;
    for (int i = 0; i < config.extra_ignore_count; i++) {
        if (strcmp(config.extra_ignore_patterns[i], "dist") == 0) found_dist = 1;
        if (strcmp(config.extra_ignore_patterns[i], "build") == 0) found_build = 1;
    }
    TT_ASSERT(found_dist, "should contain dist");
    TT_ASSERT(found_build, "should contain build");

    tt_config_free(&config);
    unsetenv("TOKTOKEN_EXTRA_IGNORE");
}

TT_TEST(test_cfg_extra_ignore_filters_empty)
{
    setenv("TOKTOKEN_EXTRA_IGNORE", "vendor,,,.cache", 1);

    tt_config_t config;
    tt_config_load(&config, "/nonexistent");

    for (int i = 0; i < config.extra_ignore_count; i++) {
        TT_ASSERT(config.extra_ignore_patterns[i][0] != '\0',
                   "should not contain empty patterns");
    }

    tt_config_free(&config);
    unsetenv("TOKTOKEN_EXTRA_IGNORE");
}

TT_TEST(test_cfg_extra_ignore_deduplicates)
{
    setenv("TOKTOKEN_EXTRA_IGNORE", "vendor,vendor,vendor", 1);

    tt_config_t config;
    tt_config_load(&config, "/nonexistent");

    int count = 0;
    for (int i = 0; i < config.extra_ignore_count; i++) {
        if (strcmp(config.extra_ignore_patterns[i], "vendor") == 0)
            count++;
    }
    TT_ASSERT_EQ_INT(1, count);

    tt_config_free(&config);
    unsetenv("TOKTOKEN_EXTRA_IGNORE");
}

TT_TEST(test_cfg_staleness_days_env_override)
{
    setenv("TOKTOKEN_STALENESS_DAYS", "14", 1);

    tt_config_t config;
    tt_config_load(&config, "/nonexistent");

    TT_ASSERT_EQ_INT(14, config.staleness_days);

    tt_config_free(&config);
    unsetenv("TOKTOKEN_STALENESS_DAYS");
}

TT_TEST(test_cfg_staleness_days_zero_clamped)
{
    setenv("TOKTOKEN_STALENESS_DAYS", "0", 1);

    tt_config_t config;
    tt_config_load(&config, "/nonexistent");

    TT_ASSERT_EQ_INT(1, config.staleness_days);

    tt_config_free(&config);
    unsetenv("TOKTOKEN_STALENESS_DAYS");
}

TT_TEST(test_cfg_staleness_days_negative_clamped)
{
    setenv("TOKTOKEN_STALENESS_DAYS", "-5", 1);

    tt_config_t config;
    tt_config_load(&config, "/nonexistent");

    TT_ASSERT_EQ_INT(1, config.staleness_days);

    tt_config_free(&config);
    unsetenv("TOKTOKEN_STALENESS_DAYS");
}

TT_TEST(test_cfg_env_takes_precedence_over_project)
{
    char *tmpdir = tt_test_tmpdir();
    TT_ASSERT_NOT_NULL(tmpdir);

    tt_test_write_file(tmpdir, ".toktoken.json",
        "{\"index\":{\"staleness_days\":14}}");
    setenv("TOKTOKEN_STALENESS_DAYS", "30", 1);

    tt_config_t config;
    tt_config_load(&config, tmpdir);

    TT_ASSERT_EQ_INT(30, config.staleness_days);

    tt_config_free(&config);
    unsetenv("TOKTOKEN_STALENESS_DAYS");
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_cfg_extra_ignore_merges_with_project)
{
    char *tmpdir = tt_test_tmpdir();
    TT_ASSERT_NOT_NULL(tmpdir);

    tt_test_write_file(tmpdir, ".toktoken.json",
        "{\"index\":{\"extra_ignore_patterns\":[\"project-pattern\"]}}");
    setenv("TOKTOKEN_EXTRA_IGNORE", "env-pattern", 1);

    tt_config_t config;
    tt_config_load(&config, tmpdir);

    int found_proj = 0, found_env = 0;
    for (int i = 0; i < config.extra_ignore_count; i++) {
        if (strcmp(config.extra_ignore_patterns[i], "project-pattern") == 0)
            found_proj = 1;
        if (strcmp(config.extra_ignore_patterns[i], "env-pattern") == 0)
            found_env = 1;
    }
    TT_ASSERT(found_proj, "should contain project-pattern");
    TT_ASSERT(found_env, "should contain env-pattern");

    tt_config_free(&config);
    unsetenv("TOKTOKEN_EXTRA_IGNORE");
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_cfg_extra_extensions_env)
{
    char *tmpdir = tt_test_tmpdir();
    if (!tmpdir) return;

    setenv("TOKTOKEN_EXTRA_EXTENSIONS", "xyz:python,abc:ruby", 1);

    tt_config_t config;
    tt_config_load(&config, tmpdir);

    TT_ASSERT_EQ_INT(config.extra_ext_count, 2);
    TT_ASSERT_EQ_STR(config.extra_ext_keys[0], "xyz");
    TT_ASSERT_EQ_STR(config.extra_ext_languages[0], "python");
    TT_ASSERT_EQ_STR(config.extra_ext_keys[1], "abc");
    TT_ASSERT_EQ_STR(config.extra_ext_languages[1], "ruby");

    tt_config_free(&config);
    unsetenv("TOKTOKEN_EXTRA_EXTENSIONS");
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

void run_config_tests(void)
{
    TT_RUN(test_cfg_default_values);
    TT_RUN(test_cfg_project_config_overrides_index);
    TT_RUN(test_cfg_project_config_only_index_section);
    TT_RUN(test_cfg_invalid_json_ignored);
    TT_RUN(test_cfg_empty_file_ignored);
    TT_RUN(test_cfg_non_object_index_ignored);
    TT_RUN(test_cfg_extra_ignore_comma_separated);
    TT_RUN(test_cfg_extra_ignore_json_array);
    TT_RUN(test_cfg_extra_ignore_filters_empty);
    TT_RUN(test_cfg_extra_ignore_deduplicates);
    TT_RUN(test_cfg_staleness_days_env_override);
    TT_RUN(test_cfg_staleness_days_zero_clamped);
    TT_RUN(test_cfg_staleness_days_negative_clamped);
    TT_RUN(test_cfg_env_takes_precedence_over_project);
    TT_RUN(test_cfg_extra_ignore_merges_with_project);
    TT_RUN(test_cfg_extra_extensions_env);
}
