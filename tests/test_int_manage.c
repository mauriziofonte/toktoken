/*
 * test_int_manage.c -- Integration tests for Phase 10: config, stats,
 *                      projects:list, cache:clear, codebase:detect.
 *
 * Converted from legacy test_manage.c to test_framework.h format.
 */

#include "test_framework.h"
#include "test_helpers.h"
#include "config.h"
#include "cmd_manage.h"
#include "cmd_index.h"
#include "cli.h"
#include "json_output.h"
#include "platform.h"
#include "storage_paths.h"
#include "str_util.h"
#include "error.h"
#include "database.h"

#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef TT_PLATFORM_WINDOWS
#include <unistd.h>
#endif
#include "test_compat.h"

/* ---- Fixture helpers ---- */

static char *s_tmpdir;

static void manage_setup(void)
{
    s_tmpdir = tt_test_tmpdir();
}

static void manage_cleanup(void)
{
    if (s_tmpdir) {
        tt_test_rmdir(s_tmpdir);
        free(s_tmpdir);
        s_tmpdir = NULL;
    }
}

/*
 * Create a small indexed project for stats/cache/projects tests.
 * Sets s_tmpdir to a fresh temp directory with PHP files and indexes them.
 */
static void create_indexed_project(void)
{
    manage_setup();

    tt_test_write_file(s_tmpdir, "src/Auth.php",
        "<?php\n"
        "class Auth {\n"
        "    public function login() {}\n"
        "    public function logout() {}\n"
        "}\n");

    tt_test_write_file(s_tmpdir, "src/Utils.php",
        "<?php\n"
        "function helper_one() {}\n"
        "function helper_two() {}\n");

    /* Index the project */
    tt_cli_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.path = s_tmpdir;
    opts.truncate_width = 120;

    cJSON *result = tt_cmd_index_create_exec(&opts);
    if (result) cJSON_Delete(result);
}

/* ---- Config tests ---- */

TT_TEST(test_config_defaults)
{
    tt_config_t config;
    tt_config_load(&config, "/nonexistent/path");

    TT_ASSERT_EQ_INT(config.max_file_size_kb, 2048);
    TT_ASSERT_EQ_INT(config.max_files, 200000);
    TT_ASSERT_EQ_INT(config.staleness_days, 7);
    TT_ASSERT_EQ_INT(config.ctags_timeout_seconds, 120);
    TT_ASSERT_EQ_INT(config.extra_ignore_count, 0);
    TT_ASSERT_EQ_INT(config.language_count, 0);
    TT_ASSERT_NOT_NULL(config.log_level);
    TT_ASSERT_EQ_STR(config.log_level, "info");

    tt_config_free(&config);
}

TT_TEST(test_config_project_file)
{
    manage_setup();

    tt_test_write_file(s_tmpdir, ".toktoken.json",
        "{\n"
        "    \"index\": {\n"
        "        \"max_file_size_kb\": 1024,\n"
        "        \"staleness_days\": 14,\n"
        "        \"extra_ignore_patterns\": [\"*.generated.*\", \"*.min.js\"]\n"
        "    }\n"
        "}\n");

    tt_config_t config;
    tt_config_load(&config, s_tmpdir);

    TT_ASSERT_EQ_INT(config.max_file_size_kb, 1024);
    TT_ASSERT_EQ_INT(config.staleness_days, 14);
    TT_ASSERT_EQ_INT(config.extra_ignore_count, 2);
    TT_ASSERT_EQ_INT(config.max_files, 200000);

    tt_config_free(&config);
    manage_cleanup();
}

TT_TEST(test_config_env_staleness)
{
    setenv("TOKTOKEN_STALENESS_DAYS", "3", 1);

    tt_config_t config;
    tt_config_load(&config, "/nonexistent");

    TT_ASSERT_EQ_INT(config.staleness_days, 3);

    tt_config_free(&config);
    unsetenv("TOKTOKEN_STALENESS_DAYS");
}

TT_TEST(test_config_env_extra_ignore_csv)
{
    setenv("TOKTOKEN_EXTRA_IGNORE", "*.log, *.tmp ,*.bak", 1);

    tt_config_t config;
    tt_config_load(&config, "/nonexistent");

    TT_ASSERT_EQ_INT(config.extra_ignore_count, 3);

    tt_config_free(&config);
    unsetenv("TOKTOKEN_EXTRA_IGNORE");
}

TT_TEST(test_config_env_extra_ignore_json)
{
    setenv("TOKTOKEN_EXTRA_IGNORE", "[\"*.log\", \"*.cache\"]", 1);

    tt_config_t config;
    tt_config_load(&config, "/nonexistent");

    TT_ASSERT_EQ_INT(config.extra_ignore_count, 2);

    tt_config_free(&config);
    unsetenv("TOKTOKEN_EXTRA_IGNORE");
}

TT_TEST(test_config_env_staleness_min_clamp)
{
    setenv("TOKTOKEN_STALENESS_DAYS", "0", 1);

    tt_config_t config;
    tt_config_load(&config, "/nonexistent");

    TT_ASSERT_EQ_INT(config.staleness_days, 1);

    tt_config_free(&config);
    unsetenv("TOKTOKEN_STALENESS_DAYS");
}

/* ---- Stats tests ---- */

TT_TEST(test_stats_no_index)
{
    tt_cli_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.path = "/nonexistent/project";
    opts.truncate_width = 120;

    cJSON *result = tt_cmd_stats_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(result, "error"));

    const char *err = cJSON_GetObjectItemCaseSensitive(result, "error")->valuestring;
    TT_ASSERT_EQ_STR(err, "no_index");

    cJSON_Delete(result);
}

TT_TEST(test_stats_basic)
{
    create_indexed_project();

    tt_cli_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.path = s_tmpdir;
    opts.truncate_width = 120;

    cJSON *result = tt_cmd_stats_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT(cJSON_GetObjectItemCaseSensitive(result, "error") == NULL,
              "stats has no error");

    cJSON *files = cJSON_GetObjectItemCaseSensitive(result, "files");
    TT_ASSERT(files != NULL && cJSON_IsNumber(files), "stats has files count");
    TT_ASSERT_GE_INT(files->valueint, 2);

    cJSON *symbols = cJSON_GetObjectItemCaseSensitive(result, "symbols");
    TT_ASSERT(symbols != NULL && cJSON_IsNumber(symbols), "stats has symbols count");

    cJSON *langs = cJSON_GetObjectItemCaseSensitive(result, "languages");
    TT_ASSERT(langs != NULL && cJSON_IsObject(langs), "stats has languages object");

    cJSON *kinds = cJSON_GetObjectItemCaseSensitive(result, "kinds");
    TT_ASSERT(kinds != NULL && cJSON_IsObject(kinds), "stats has kinds object");

    cJSON *dirs = cJSON_GetObjectItemCaseSensitive(result, "dirs");
    TT_ASSERT(dirs != NULL && cJSON_IsObject(dirs), "stats has dirs object");

    cJSON_Delete(result);
    manage_cleanup();
}

/* ---- cache:clear tests ---- */

TT_TEST(test_cache_clear_no_index)
{
    tt_cli_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.path = "/nonexistent/project";
    opts.truncate_width = 120;

    cJSON *result = tt_cmd_cache_clear_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(result, "error"));

    cJSON_Delete(result);
}

TT_TEST(test_cache_clear_single)
{
    create_indexed_project();

    /* Verify index exists */
    TT_ASSERT_TRUE(tt_database_exists(s_tmpdir));

    tt_cli_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.path = s_tmpdir;
    opts.truncate_width = 120;

    cJSON *result = tt_cmd_cache_clear_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT(cJSON_GetObjectItemCaseSensitive(result, "error") == NULL,
              "cache:clear has no error");

    cJSON *deleted = cJSON_GetObjectItemCaseSensitive(result, "deleted");
    TT_ASSERT(deleted != NULL && cJSON_IsString(deleted), "cache:clear has deleted path");

    cJSON *freed = cJSON_GetObjectItemCaseSensitive(result, "freed_bytes");
    TT_ASSERT(freed != NULL && cJSON_IsNumber(freed), "cache:clear has freed_bytes");
    TT_ASSERT(freed->valuedouble > 0, "cache:clear freed > 0 bytes");

    /* Verify index no longer exists */
    TT_ASSERT_FALSE(tt_database_exists(s_tmpdir));

    cJSON_Delete(result);
    manage_cleanup();
}

TT_TEST(test_cache_clear_all_no_force)
{
    tt_cli_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.all = true;
    opts.force = false;
    opts.truncate_width = 120;

    cJSON *result = tt_cmd_cache_clear_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    const char *err = cJSON_GetObjectItemCaseSensitive(result, "error")->valuestring;
    TT_ASSERT_EQ_STR(err, "confirmation_required");

    cJSON_Delete(result);
}

TT_TEST(test_cache_clear_all_force_purge)
{
    manage_setup();
    tt_test_write_file(s_tmpdir, "main.c", "int main() { return 0; }");

    tt_cli_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.path = s_tmpdir;
    opts.truncate_width = 120;

    cJSON *idx = tt_cmd_index_create_exec(&opts);
    TT_ASSERT_NOT_NULL(idx);
    cJSON_Delete(idx);
    TT_ASSERT_TRUE(tt_database_exists(s_tmpdir));

    /* Verify base dir exists */
    char *base = tt_storage_base_dir();
    TT_ASSERT(base != NULL && tt_is_dir(base), "base dir exists before purge");

    /* Purge everything */
    tt_cli_opts_t purge_opts;
    memset(&purge_opts, 0, sizeof(purge_opts));
    purge_opts.all = true;
    purge_opts.force = true;
    purge_opts.truncate_width = 120;

    cJSON *result = tt_cmd_cache_clear_exec(&purge_opts);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT(cJSON_GetObjectItemCaseSensitive(result, "error") == NULL,
              "purge has no error");

    cJSON *purged = cJSON_GetObjectItemCaseSensitive(result, "purged");
    TT_ASSERT(purged != NULL && cJSON_IsString(purged), "has purged path");

    cJSON *freed = cJSON_GetObjectItemCaseSensitive(result, "freed_bytes");
    TT_ASSERT(freed != NULL && cJSON_IsNumber(freed), "has freed_bytes");
    TT_ASSERT(freed->valuedouble > 0, "freed > 0 bytes");

    /* Base dir should be gone */
    TT_ASSERT_FALSE(tt_is_dir(base));

    /* Re-creation: index:create should work */
    cJSON *idx2 = tt_cmd_index_create_exec(&opts);
    TT_ASSERT_NOT_NULL(idx2);
    cJSON_Delete(idx2);
    TT_ASSERT_TRUE(tt_database_exists(s_tmpdir));

    cJSON_Delete(result);
    free(base);
    manage_cleanup();
}

/* ---- codebase:detect tests ---- */

TT_TEST(test_codebase_detect_manifest_root)
{
    manage_setup();
    tt_test_write_file(s_tmpdir, "package.json", "{}");
    tt_test_write_file(s_tmpdir, "src/index.js", "console.log('hello');");

    tt_cli_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.path = s_tmpdir;
    opts.truncate_width = 120;

    int exit_code = 0;
    cJSON *result = tt_cmd_codebase_detect_exec(&opts, &exit_code);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT_EQ_INT(exit_code, 0);

    cJSON *is_cb = cJSON_GetObjectItemCaseSensitive(result, "is_codebase");
    TT_ASSERT(is_cb != NULL && cJSON_IsTrue(is_cb), "is_codebase is true");

    cJSON *detection = cJSON_GetObjectItemCaseSensitive(result, "detection");
    TT_ASSERT(detection != NULL && strcmp(detection->valuestring, "manifest-root") == 0,
              "detection is manifest-root");

    cJSON *ecosystems = cJSON_GetObjectItemCaseSensitive(result, "ecosystems");
    TT_ASSERT(ecosystems != NULL && cJSON_IsArray(ecosystems), "ecosystems is array");
    TT_ASSERT_GE_INT(cJSON_GetArraySize(ecosystems), 1);

    cJSON *action = cJSON_GetObjectItemCaseSensitive(result, "action");
    TT_ASSERT(action != NULL && strcmp(action->valuestring, "index:create") == 0,
              "action is index:create");

    cJSON_Delete(result);
    manage_cleanup();
}

TT_TEST(test_codebase_detect_not_codebase)
{
    manage_setup();
    tt_test_write_file(s_tmpdir, "readme.txt", "Just a readme");

    tt_cli_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.path = s_tmpdir;
    opts.truncate_width = 120;

    int exit_code = 0;
    cJSON *result = tt_cmd_codebase_detect_exec(&opts, &exit_code);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT_EQ_INT(exit_code, 1);

    cJSON *is_cb = cJSON_GetObjectItemCaseSensitive(result, "is_codebase");
    TT_ASSERT(is_cb != NULL && cJSON_IsFalse(is_cb), "is_codebase is false");

    cJSON *action = cJSON_GetObjectItemCaseSensitive(result, "action");
    TT_ASSERT(action != NULL && strcmp(action->valuestring, "skip") == 0,
              "action is skip");

    cJSON_Delete(result);
    manage_cleanup();
}

TT_TEST(test_codebase_detect_invalid_path)
{
    tt_cli_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.path = "/nonexistent/path/to/nowhere";
    opts.truncate_width = 120;

    int exit_code = 0;
    cJSON *result = tt_cmd_codebase_detect_exec(&opts, &exit_code);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(result, "error"));

    cJSON_Delete(result);
}

TT_TEST(test_codebase_detect_manifest_subdir)
{
    manage_setup();

    /* Create a manifest in a subdirectory */
    tt_test_write_file(s_tmpdir, "myapp/composer.json", "{\"require\": {}}");

    tt_cli_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.path = s_tmpdir;
    opts.truncate_width = 120;

    int exit_code = 0;
    cJSON *result = tt_cmd_codebase_detect_exec(&opts, &exit_code);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT_EQ_INT(exit_code, 0);

    cJSON *detection = cJSON_GetObjectItemCaseSensitive(result, "detection");
    TT_ASSERT(detection != NULL && strcmp(detection->valuestring, "manifest-subdir") == 0,
              "detection is manifest-subdir");

    cJSON_Delete(result);
    manage_cleanup();
}

TT_TEST(test_codebase_detect_heuristic)
{
    manage_setup();

    /* Create .git dir and >= 5 source files (no manifest) */
    char *git_dir = tt_path_join(s_tmpdir, ".git");
    tt_mkdir_p(git_dir);
    free(git_dir);

    tt_test_write_file(s_tmpdir, "a.py", "x = 1");
    tt_test_write_file(s_tmpdir, "b.py", "y = 2");
    tt_test_write_file(s_tmpdir, "c.py", "z = 3");
    tt_test_write_file(s_tmpdir, "d.js", "var a = 1;");
    tt_test_write_file(s_tmpdir, "e.js", "var b = 2;");

    tt_cli_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.path = s_tmpdir;
    opts.truncate_width = 120;

    int exit_code = 0;
    cJSON *result = tt_cmd_codebase_detect_exec(&opts, &exit_code);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT_EQ_INT(exit_code, 0);

    cJSON *detection = cJSON_GetObjectItemCaseSensitive(result, "detection");
    TT_ASSERT(detection != NULL && strcmp(detection->valuestring, "heuristic") == 0,
              "detection is heuristic");

    cJSON_Delete(result);
    manage_cleanup();
}

/* ---- projects:list tests ---- */

TT_TEST(test_projects_list_empty)
{
    tt_cli_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.truncate_width = 120;

    cJSON *result = tt_cmd_projects_list_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    cJSON *projects = cJSON_GetObjectItemCaseSensitive(result, "projects");
    TT_ASSERT(projects != NULL && cJSON_IsArray(projects), "projects is array");

    cJSON *n = cJSON_GetObjectItemCaseSensitive(result, "n");
    TT_ASSERT(n != NULL && cJSON_IsNumber(n), "n is number");

    cJSON_Delete(result);
}

TT_TEST(test_projects_list_with_project)
{
    create_indexed_project();

    tt_cli_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.truncate_width = 120;

    cJSON *result = tt_cmd_projects_list_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    cJSON *projects = cJSON_GetObjectItemCaseSensitive(result, "projects");
    TT_ASSERT(projects != NULL && cJSON_IsArray(projects), "projects is array");
    TT_ASSERT_GE_INT(cJSON_GetArraySize(projects), 1);

    /* Check first project has required fields */
    cJSON *first = cJSON_GetArrayItem(projects, 0);
    TT_ASSERT_NOT_NULL(first);
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(first, "path"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(first, "hash"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(first, "files"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(first, "symbols"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(first, "db_size"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(first, "indexed_at"));

    cJSON_Delete(result);
    manage_cleanup();
}

/* ---- Runner ---- */

void run_int_manage_tests(void)
{
    TT_SUITE("Config Tests");
    TT_RUN(test_config_defaults);
    TT_RUN(test_config_project_file);
    TT_RUN(test_config_env_staleness);
    TT_RUN(test_config_env_extra_ignore_csv);
    TT_RUN(test_config_env_extra_ignore_json);
    TT_RUN(test_config_env_staleness_min_clamp);

    TT_SUITE("Stats Tests");
    TT_RUN(test_stats_no_index);
    TT_RUN(test_stats_basic);

    TT_SUITE("Cache Clear Tests");
    TT_RUN(test_cache_clear_no_index);
    TT_RUN(test_cache_clear_single);
    TT_RUN(test_cache_clear_all_no_force);
    TT_RUN(test_cache_clear_all_force_purge);

    TT_SUITE("Codebase Detect Tests");
    TT_RUN(test_codebase_detect_manifest_root);
    TT_RUN(test_codebase_detect_not_codebase);
    TT_RUN(test_codebase_detect_invalid_path);
    TT_RUN(test_codebase_detect_manifest_subdir);
    TT_RUN(test_codebase_detect_heuristic);

    TT_SUITE("Projects List Tests");
    TT_RUN(test_projects_list_empty);
    TT_RUN(test_projects_list_with_project);
}
