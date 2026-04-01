/*
 * test_int_pipeline.c -- Integration tests for the parallel indexing pipeline.
 *
 * Requires universal-ctags in PATH and a writable temp directory.
 */

#include "test_framework.h"
#include "test_helpers.h"
#include "index_pipeline.h"
#include "database.h"
#include "index_store.h"
#include "schema.h"
#include "platform.h"
#include "str_util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

static const char *ctags_path(void)
{
    static const char *names[] = {
        "ctags", "universal-ctags", "ctags-universal", NULL
    };
    static char resolved[512];
    for (int i = 0; names[i]; i++)
    {
        char *path = tt_which(names[i]);
        if (path)
        {
            snprintf(resolved, sizeof(resolved), "%s", path);
            free(path);
            return resolved;
        }
    }
    return NULL;
}

/*
 * Helper: create a temp project with PHP source files and return tmpdir.
 * Writes mini PHP files to tmpdir/src/FileN.php.
 * Sets *paths and *count to the relative path array.
 */
static char *create_test_project(const char ***paths, int *count,
                                  int num_files)
{
    char *tmpdir = tt_test_tmpdir();
    if (!tmpdir)
        return NULL;

    const char **p = malloc((size_t)num_files * sizeof(char *));
    if (!p)
    {
        tt_test_rmdir(tmpdir);
        free(tmpdir);
        return NULL;
    }

    for (int i = 0; i < num_files; i++)
    {
        char rel[64];
        snprintf(rel, sizeof(rel), "src/File%d.php", i);
        char content[512];
        snprintf(content, sizeof(content),
                 "<?php\nclass File%d {\n"
                 "    public function method%d(): string { return ''; }\n"
                 "    public function other%d(): int { return 0; }\n"
                 "}\n", i, i, i);
        tt_test_write_file(tmpdir, rel, content);
        p[i] = tt_strdup(rel);
    }

    *paths = p;
    *count = num_files;
    return tmpdir;
}

static void free_paths(const char **paths, int count)
{
    for (int i = 0; i < count; i++)
        free((void *)paths[i]);
    free(paths);
}

/* ---- Tests ---- */

TT_TEST(test_pipeline_basic)
{
    const char *ctags = ctags_path();
    if (!ctags) return;

    const char **paths = NULL;
    int count = 0;
    char *tmpdir = create_test_project(&paths, &count, 5);
    TT_ASSERT_NOT_NULL(tmpdir);

    tt_database_t db;
    TT_ASSERT_EQ_INT(0, tt_database_open(&db, tmpdir));

    tt_index_store_t store;
    TT_ASSERT_EQ_INT(0, tt_store_init(&store, &db));

    tt_pipeline_config_t config = {0};
    config.project_root = tmpdir;
    config.ctags_path = ctags;
    config.file_paths = paths;
    config.file_count = count;
    config.num_workers = 2;
    config.ctags_timeout_sec = 30;
    config.max_file_size_bytes = 10 * 1024 * 1024;
    config.db = &db;
    config.store = &store;

    tt_pipeline_result_t result;
    int rc = tt_pipeline_run(&config, &result);
    TT_ASSERT_EQ_INT(0, rc);

    /* 5 files, each with 1 class + 2 methods = ~15 symbols from ctags */
    TT_ASSERT(result.files_indexed == 5, "should index 5 files");
    TT_ASSERT(result.symbols_indexed > 0, "should have symbols");
    TT_ASSERT(result.errors_count == 0, "should have no errors");

    tt_pipeline_result_free(&result);
    tt_store_close(&store);
    tt_database_close(&db);
    free_paths(paths, count);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_pipeline_single_file)
{
    const char *ctags = ctags_path();
    if (!ctags) return;

    const char **paths = NULL;
    int count = 0;
    char *tmpdir = create_test_project(&paths, &count, 1);
    TT_ASSERT_NOT_NULL(tmpdir);

    tt_database_t db;
    TT_ASSERT_EQ_INT(0, tt_database_open(&db, tmpdir));

    tt_index_store_t store;
    TT_ASSERT_EQ_INT(0, tt_store_init(&store, &db));

    tt_pipeline_config_t config = {0};
    config.project_root = tmpdir;
    config.ctags_path = ctags;
    config.file_paths = paths;
    config.file_count = count;
    config.num_workers = 1;
    config.ctags_timeout_sec = 30;
    config.max_file_size_bytes = 10 * 1024 * 1024;
    config.db = &db;
    config.store = &store;

    tt_pipeline_result_t result;
    int rc = tt_pipeline_run(&config, &result);
    TT_ASSERT_EQ_INT(0, rc);

    TT_ASSERT(result.files_indexed == 1, "should index 1 file");
    TT_ASSERT(result.symbols_indexed > 0, "should have symbols");

    tt_pipeline_result_free(&result);
    tt_store_close(&store);
    tt_database_close(&db);
    free_paths(paths, count);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_pipeline_many_workers)
{
    const char *ctags = ctags_path();
    if (!ctags) return;

    const char **paths = NULL;
    int count = 0;
    char *tmpdir = create_test_project(&paths, &count, 20);
    TT_ASSERT_NOT_NULL(tmpdir);

    tt_database_t db;
    TT_ASSERT_EQ_INT(0, tt_database_open(&db, tmpdir));

    tt_index_store_t store;
    TT_ASSERT_EQ_INT(0, tt_store_init(&store, &db));

    tt_pipeline_config_t config = {0};
    config.project_root = tmpdir;
    config.ctags_path = ctags;
    config.file_paths = paths;
    config.file_count = count;
    config.num_workers = 4;
    config.ctags_timeout_sec = 30;
    config.max_file_size_bytes = 10 * 1024 * 1024;
    config.db = &db;
    config.store = &store;

    tt_pipeline_result_t result;
    int rc = tt_pipeline_run(&config, &result);
    TT_ASSERT_EQ_INT(0, rc);

    TT_ASSERT(result.files_indexed == 20, "should index 20 files");
    TT_ASSERT(result.symbols_indexed > 0, "should have symbols");
    TT_ASSERT(result.errors_count == 0, "should have no errors");

    tt_pipeline_result_free(&result);
    tt_store_close(&store);
    tt_database_close(&db);
    free_paths(paths, count);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_pipeline_progress_tracking)
{
    const char *ctags = ctags_path();
    if (!ctags) return;

    const char **paths = NULL;
    int count = 0;
    char *tmpdir = create_test_project(&paths, &count, 10);
    TT_ASSERT_NOT_NULL(tmpdir);

    tt_database_t db;
    TT_ASSERT_EQ_INT(0, tt_database_open(&db, tmpdir));

    tt_index_store_t store;
    TT_ASSERT_EQ_INT(0, tt_store_init(&store, &db));

    tt_progress_t progress = {0};
    progress.total_files = count;

    tt_pipeline_config_t config = {0};
    config.project_root = tmpdir;
    config.ctags_path = ctags;
    config.file_paths = paths;
    config.file_count = count;
    config.num_workers = 2;
    config.ctags_timeout_sec = 30;
    config.max_file_size_bytes = 10 * 1024 * 1024;
    config.db = &db;
    config.store = &store;

    tt_pipeline_handle_t *handle = NULL;
    int rc = tt_pipeline_start(&config, &progress, &handle);
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_NOT_NULL(handle);

    tt_pipeline_result_t result;
    rc = tt_pipeline_join(handle, &result);
    TT_ASSERT_EQ_INT(0, rc);

    /* After join, progress counters should reflect completed work */
    int64_t files_done = atomic_load(&progress.files_indexed);
    TT_ASSERT(files_done > 0, "progress should track files");

    tt_pipeline_result_free(&result);
    tt_store_close(&store);
    tt_database_close(&db);
    free_paths(paths, count);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_pipeline_empty_files)
{
    const char *ctags = ctags_path();
    if (!ctags) return;

    char *tmpdir = tt_test_tmpdir();
    TT_ASSERT_NOT_NULL(tmpdir);

    /* Create empty files that produce no symbols */
    tt_test_write_file(tmpdir, "src/empty.php", "<?php\n");
    const char *paths[] = { "src/empty.php" };

    tt_database_t db;
    TT_ASSERT_EQ_INT(0, tt_database_open(&db, tmpdir));

    tt_index_store_t store;
    TT_ASSERT_EQ_INT(0, tt_store_init(&store, &db));

    tt_pipeline_config_t config = {0};
    config.project_root = tmpdir;
    config.ctags_path = ctags;
    config.file_paths = paths;
    config.file_count = 1;
    config.num_workers = 1;
    config.ctags_timeout_sec = 30;
    config.max_file_size_bytes = 10 * 1024 * 1024;
    config.db = &db;
    config.store = &store;

    tt_pipeline_result_t result;
    int rc = tt_pipeline_run(&config, &result);
    TT_ASSERT_EQ_INT(0, rc);

    /* File with just "<?php\n" may produce 0 symbols -- that is fine */
    TT_ASSERT(result.files_indexed >= 0, "should not crash on empty files");
    TT_ASSERT(result.errors_count == 0, "should have no errors");

    tt_pipeline_result_free(&result);
    tt_store_close(&store);
    tt_database_close(&db);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_pipeline_symbols_in_db)
{
    const char *ctags = ctags_path();
    if (!ctags) return;

    const char **paths = NULL;
    int count = 0;
    char *tmpdir = create_test_project(&paths, &count, 3);
    TT_ASSERT_NOT_NULL(tmpdir);

    tt_database_t db;
    TT_ASSERT_EQ_INT(0, tt_database_open(&db, tmpdir));

    tt_index_store_t store;
    TT_ASSERT_EQ_INT(0, tt_store_init(&store, &db));

    tt_pipeline_config_t config = {0};
    config.project_root = tmpdir;
    config.ctags_path = ctags;
    config.file_paths = paths;
    config.file_count = count;
    config.num_workers = 2;
    config.ctags_timeout_sec = 30;
    config.max_file_size_bytes = 10 * 1024 * 1024;
    config.db = &db;
    config.store = &store;

    tt_pipeline_result_t result;
    int rc = tt_pipeline_run(&config, &result);
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT(result.symbols_indexed > 0, "should have symbols");

    /* Verify symbols are actually in the database by querying */
    tt_search_results_t search_results;
    rc = tt_store_search_symbols(&store, "method", NULL, 0, NULL, NULL, 50, false,
                                  &search_results);
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT(search_results.count > 0, "should find method symbols in DB");
    tt_search_results_free(&search_results);

    tt_pipeline_result_free(&result);
    tt_store_close(&store);
    tt_database_close(&db);
    free_paths(paths, count);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_pipeline_fixture_project)
{
    const char *ctags = ctags_path();
    if (!ctags) return;

    const char *fixtures = tt_test_fixtures_dir();
    if (!fixtures) return;

    char project_path[512];
    snprintf(project_path, sizeof(project_path), "%s/mini-project", fixtures);

    /* Use the mini-project fixture (App.php + Service.php) */
    const char *paths[] = { "src/App.php", "src/Service.php" };

    tt_database_t db;
    TT_ASSERT_EQ_INT(0, tt_database_open(&db, project_path));

    tt_index_store_t store;
    TT_ASSERT_EQ_INT(0, tt_store_init(&store, &db));

    tt_pipeline_config_t config = {0};
    config.project_root = project_path;
    config.ctags_path = ctags;
    config.file_paths = paths;
    config.file_count = 2;
    config.num_workers = 2;
    config.ctags_timeout_sec = 30;
    config.max_file_size_bytes = 10 * 1024 * 1024;
    config.db = &db;
    config.store = &store;

    tt_pipeline_result_t result;
    int rc = tt_pipeline_run(&config, &result);
    TT_ASSERT_EQ_INT(0, rc);

    TT_ASSERT(result.files_indexed == 2, "should index 2 fixture files");
    TT_ASSERT(result.symbols_indexed > 0, "should have symbols");

    /* App.php has: class App, getVersion, run, search, helper = 5 symbols */
    /* Service.php has its own symbols */
    TT_ASSERT(result.symbols_indexed >= 5, "should have at least 5 symbols");

    tt_pipeline_result_free(&result);
    tt_store_close(&store);
    tt_database_close(&db);
}

TT_TEST(test_pipeline_large_set)
{
    const char *ctags = ctags_path();
    if (!ctags) return;

    const char **paths = NULL;
    int count = 0;
    char *tmpdir = create_test_project(&paths, &count, 100);
    TT_ASSERT_NOT_NULL(tmpdir);

    tt_database_t db;
    TT_ASSERT_EQ_INT(0, tt_database_open(&db, tmpdir));

    tt_index_store_t store;
    TT_ASSERT_EQ_INT(0, tt_store_init(&store, &db));

    tt_pipeline_config_t config = {0};
    config.project_root = tmpdir;
    config.ctags_path = ctags;
    config.file_paths = paths;
    config.file_count = count;
    config.num_workers = 4;
    config.ctags_timeout_sec = 60;
    config.max_file_size_bytes = 10 * 1024 * 1024;
    config.db = &db;
    config.store = &store;

    tt_pipeline_result_t result;
    int rc = tt_pipeline_run(&config, &result);
    TT_ASSERT_EQ_INT(0, rc);

    TT_ASSERT(result.files_indexed == 100, "should index 100 files");
    /* 100 files * ~3 symbols each = ~300 symbols minimum */
    TT_ASSERT(result.symbols_indexed >= 100, "should have many symbols");
    TT_ASSERT(result.errors_count == 0, "should have no errors");

    tt_pipeline_result_free(&result);
    tt_store_close(&store);
    tt_database_close(&db);
    free_paths(paths, count);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

void run_int_pipeline_tests(void)
{
    TT_RUN(test_pipeline_basic);
    TT_RUN(test_pipeline_single_file);
    TT_RUN(test_pipeline_many_workers);
    TT_RUN(test_pipeline_progress_tracking);
    TT_RUN(test_pipeline_empty_files);
    TT_RUN(test_pipeline_symbols_in_db);
    TT_RUN(test_pipeline_fixture_project);
    TT_RUN(test_pipeline_large_set);
}
