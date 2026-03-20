/*
 * test_int_savings.c -- Integration tests for the token savings subsystem.
 *
 * Covers: tt_savings_calculate, tt_savings_ensure_tables, tt_savings_record,
 * tt_savings_get_totals, tt_savings_reset, tt_savings_raw_from_file_sizes,
 * tt_savings_raw_from_file, tt_savings_track.
 */

#include "test_framework.h"
#include "test_helpers.h"
#include "token_savings.h"
#include "database.h"
#include "schema.h"
#include "platform.h"
#include "str_util.h"

#include <string.h>
#include <stdlib.h>

static char *s_tmpdir;

static void savings_setup(void)
{
    s_tmpdir = tt_test_tmpdir();
}

static void savings_cleanup(void)
{
    if (s_tmpdir) {
        tt_test_rmdir(s_tmpdir);
        free(s_tmpdir);
        s_tmpdir = NULL;
    }
}

/* Helper: open a test DB with schema */
static int open_test_db(tt_database_t *db, const char *name)
{
    char *project_dir = tt_path_join(s_tmpdir, name);
    tt_mkdir_p(project_dir);
    int rc = tt_database_open(db, project_dir);
    free(project_dir);
    return rc;
}

/* ---- tt_savings_calculate ---- */

TT_TEST(test_calculate_positive_savings)
{
    tt_savings_record_t rec = tt_savings_calculate("test_tool", 10000, 2000);
    TT_ASSERT_EQ_STR(rec.tool_name, "test_tool");
    TT_ASSERT_TRUE(rec.raw_bytes == 10000);
    TT_ASSERT_TRUE(rec.response_bytes == 2000);
    /* (10000 - 2000) / 4 = 2000 */
    TT_ASSERT_TRUE(rec.tokens_saved == 2000);
}

TT_TEST(test_calculate_no_savings)
{
    tt_savings_record_t rec = tt_savings_calculate("tool", 100, 200);
    TT_ASSERT_TRUE(rec.tokens_saved == 0);
}

TT_TEST(test_calculate_equal_bytes)
{
    tt_savings_record_t rec = tt_savings_calculate("tool", 500, 500);
    TT_ASSERT_TRUE(rec.tokens_saved == 0);
}

TT_TEST(test_calculate_zero_raw)
{
    tt_savings_record_t rec = tt_savings_calculate("tool", 0, 100);
    TT_ASSERT_TRUE(rec.tokens_saved == 0);
}

/* ---- tt_savings_ensure_tables + record + get_totals ---- */

TT_TEST(test_ensure_tables_creates_tables)
{
    tt_database_t db;
    TT_ASSERT_EQ_INT(open_test_db(&db, "test_tables"), 0);

    int rc = tt_savings_ensure_tables(db.db);
    TT_ASSERT_EQ_INT(rc, 0);

    /* Verify savings_totals table exists by querying it */
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db.db,
        "SELECT COUNT(*) FROM savings_totals", -1, &stmt, NULL);
    TT_ASSERT_EQ_INT(rc, SQLITE_OK);
    TT_ASSERT_EQ_INT(sqlite3_step(stmt), SQLITE_ROW);
    sqlite3_finalize(stmt);

    tt_database_close(&db);
}

TT_TEST(test_record_and_get_totals)
{
    tt_database_t db;
    TT_ASSERT_EQ_INT(open_test_db(&db, "test_record"), 0);

    tt_savings_record_t rec = tt_savings_calculate("search_symbols", 8000, 1000);
    TT_ASSERT_TRUE(rec.tokens_saved > 0);

    int rc = tt_savings_record(&db, &rec);
    TT_ASSERT_EQ_INT(rc, 0);

    tt_savings_totals_t totals;
    rc = tt_savings_get_totals(&db, &totals);
    TT_ASSERT_EQ_INT(rc, 0);
    TT_ASSERT_TRUE(totals.total_calls == 1);
    TT_ASSERT_TRUE(totals.total_raw_bytes == 8000);
    TT_ASSERT_TRUE(totals.total_response_bytes == 1000);
    TT_ASSERT_TRUE(totals.total_tokens_saved == rec.tokens_saved);

    /* Record a second entry */
    tt_savings_record_t rec2 = tt_savings_calculate("search_text", 5000, 500);
    rc = tt_savings_record(&db, &rec2);
    TT_ASSERT_EQ_INT(rc, 0);

    rc = tt_savings_get_totals(&db, &totals);
    TT_ASSERT_EQ_INT(rc, 0);
    TT_ASSERT_TRUE(totals.total_calls == 2);
    TT_ASSERT_TRUE(totals.total_raw_bytes == 13000);
    TT_ASSERT_TRUE(totals.total_response_bytes == 1500);
    TT_ASSERT_TRUE(totals.total_tokens_saved ==
                    rec.tokens_saved + rec2.tokens_saved);

    tt_database_close(&db);
}

TT_TEST(test_get_totals_empty)
{
    tt_database_t db;
    TT_ASSERT_EQ_INT(open_test_db(&db, "test_empty_totals"), 0);

    tt_savings_totals_t totals;
    int rc = tt_savings_get_totals(&db, &totals);
    TT_ASSERT_EQ_INT(rc, 0);
    TT_ASSERT_TRUE(totals.total_calls == 0);
    TT_ASSERT_TRUE(totals.total_tokens_saved == 0);

    tt_database_close(&db);
}

/* ---- tt_savings_reset ---- */

TT_TEST(test_reset)
{
    tt_database_t db;
    TT_ASSERT_EQ_INT(open_test_db(&db, "test_reset"), 0);

    /* Add some data */
    tt_savings_record_t rec = tt_savings_calculate("tool", 4000, 400);
    tt_savings_record(&db, &rec);

    /* Reset */
    int rc = tt_savings_reset(&db);
    TT_ASSERT_EQ_INT(rc, 0);

    /* Verify empty */
    tt_savings_totals_t totals;
    rc = tt_savings_get_totals(&db, &totals);
    TT_ASSERT_EQ_INT(rc, 0);
    TT_ASSERT_TRUE(totals.total_calls == 0);

    tt_database_close(&db);
}

/* ---- tt_savings_raw_from_file_sizes ---- */

TT_TEST(test_raw_from_file_sizes)
{
    /* Create test files with known sizes */
    tt_test_write_file(s_tmpdir, "proj/a.txt", "hello");  /* 5 bytes */
    tt_test_write_file(s_tmpdir, "proj/b.txt", "world!"); /* 6 bytes */

    char *proj = tt_path_join(s_tmpdir, "proj");
    const char *files[] = {"a.txt", "b.txt"};
    int64_t total = tt_savings_raw_from_file_sizes(proj, files, 2);
    TT_ASSERT_TRUE(total == 11);

    /* Duplicate paths should be counted once */
    const char *dup_files[] = {"a.txt", "a.txt", "b.txt"};
    int64_t deduped = tt_savings_raw_from_file_sizes(proj, dup_files, 3);
    TT_ASSERT_TRUE(deduped == 11);

    free(proj);
}

TT_TEST(test_raw_from_file_sizes_empty)
{
    int64_t total = tt_savings_raw_from_file_sizes("/tmp", NULL, 0);
    TT_ASSERT_TRUE(total == 0);
}

/* ---- tt_savings_raw_from_file ---- */

TT_TEST(test_raw_from_file)
{
    tt_test_write_file(s_tmpdir, "single.txt", "test content");  /* 12 bytes */
    char *path = tt_path_join(s_tmpdir, "single.txt");
    int64_t sz = tt_savings_raw_from_file(path);
    TT_ASSERT_TRUE(sz == 12);
    free(path);
}

TT_TEST(test_raw_from_file_nonexistent)
{
    int64_t sz = tt_savings_raw_from_file("/nonexistent/file.txt");
    TT_ASSERT_TRUE(sz == 0);
}

TT_TEST(test_raw_from_file_null)
{
    int64_t sz = tt_savings_raw_from_file(NULL);
    TT_ASSERT_TRUE(sz == 0);
}

/* ---- tt_savings_track (integration) ---- */

TT_TEST(test_savings_track_records_to_db)
{
    tt_database_t db;
    TT_ASSERT_EQ_INT(open_test_db(&db, "test_track"), 0);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "msg", "short");

    /* raw_bytes >> response_bytes so savings > 0 */
    tt_savings_track(&db, "test_tool", 50000, result);

    /* _savings should NOT be embedded in result */
    cJSON *savings = cJSON_GetObjectItem(result, "_savings");
    TT_ASSERT_NULL(savings);

    /* But totals should be updated in DB */
    tt_savings_totals_t totals;
    int rc = tt_savings_get_totals(&db, &totals);
    TT_ASSERT_EQ_INT(rc, 0);
    TT_ASSERT_TRUE(totals.total_calls == 1);
    TT_ASSERT_TRUE(totals.total_tokens_saved > 0);

    cJSON_Delete(result);
    tt_database_close(&db);
}

TT_TEST(test_savings_track_no_savings_when_response_larger)
{
    tt_database_t db;
    TT_ASSERT_EQ_INT(open_test_db(&db, "test_track_no"), 0);

    /* raw_bytes = 10 which will be less than the serialized result */
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "data",
        "This is a long response that is bigger than 10 bytes of raw data");

    tt_savings_track(&db, "tool", 10, result);

    /* _savings should NOT be embedded (no savings) */
    cJSON *savings = cJSON_GetObjectItem(result, "_savings");
    TT_ASSERT_NULL(savings);

    cJSON_Delete(result);
    tt_database_close(&db);
}

TT_TEST(test_savings_track_null_guards)
{
    /* Should not crash with NULL args */
    tt_savings_track(NULL, "tool", 1000, NULL);

    tt_database_t db;
    memset(&db, 0, sizeof(db));
    tt_savings_track(&db, "tool", 1000, NULL);

    cJSON *result = cJSON_CreateObject();
    tt_savings_track(NULL, "tool", 1000, result);
    cJSON_Delete(result);
}

/* ---- tt_savings_get_per_tool ---- */

TT_TEST(test_per_tool_breakdown)
{
    tt_database_t db;
    TT_ASSERT_EQ_INT(open_test_db(&db, "test_per_tool"), 0);

    /* Record calls from different tools */
    tt_savings_record_t r1 = tt_savings_calculate("search_symbols", 8000, 1000);
    tt_savings_record(&db, &r1);

    tt_savings_record_t r2 = tt_savings_calculate("inspect_bundle", 20000, 3000);
    tt_savings_record(&db, &r2);

    /* Record a second call to search_symbols */
    tt_savings_record_t r3 = tt_savings_calculate("search_symbols", 6000, 800);
    tt_savings_record(&db, &r3);

    tt_savings_per_tool_t *items = NULL;
    int count = 0;
    int rc = tt_savings_get_per_tool(&db, &items, &count);
    TT_ASSERT_EQ_INT(rc, 0);
    TT_ASSERT_EQ_INT(count, 2);

    /* Sorted by tokens_saved DESC: inspect_bundle should be first */
    TT_ASSERT_EQ_STR(items[0].tool_name, "inspect_bundle");
    TT_ASSERT_TRUE(items[0].call_count == 1);
    TT_ASSERT_TRUE(items[0].tokens_saved == r2.tokens_saved);

    /* search_symbols second, with accumulated values */
    TT_ASSERT_EQ_STR(items[1].tool_name, "search_symbols");
    TT_ASSERT_TRUE(items[1].call_count == 2);
    TT_ASSERT_TRUE(items[1].raw_bytes == 14000);
    TT_ASSERT_TRUE(items[1].tokens_saved == r1.tokens_saved + r3.tokens_saved);

    tt_savings_free_per_tool(items, count);
    tt_database_close(&db);
}

TT_TEST(test_per_tool_empty)
{
    tt_database_t db;
    TT_ASSERT_EQ_INT(open_test_db(&db, "test_per_tool_empty"), 0);

    tt_savings_per_tool_t *items = NULL;
    int count = 0;
    int rc = tt_savings_get_per_tool(&db, &items, &count);
    TT_ASSERT_EQ_INT(rc, 0);
    TT_ASSERT_EQ_INT(count, 0);
    TT_ASSERT_NULL(items);

    tt_database_close(&db);
}

TT_TEST(test_reset_clears_per_tool)
{
    tt_database_t db;
    TT_ASSERT_EQ_INT(open_test_db(&db, "test_reset_pt"), 0);

    tt_savings_record_t rec = tt_savings_calculate("tool_a", 5000, 500);
    tt_savings_record(&db, &rec);

    int rc = tt_savings_reset(&db);
    TT_ASSERT_EQ_INT(rc, 0);

    tt_savings_per_tool_t *items = NULL;
    int count = 0;
    rc = tt_savings_get_per_tool(&db, &items, &count);
    TT_ASSERT_EQ_INT(rc, 0);
    TT_ASSERT_EQ_INT(count, 0);

    tt_database_close(&db);
}

/* ---- tt_savings_raw_from_index ---- */

TT_TEST(test_raw_from_index)
{
    tt_database_t db;
    TT_ASSERT_EQ_INT(open_test_db(&db, "test_raw_idx"), 0);

    /* Create schema + insert a file with known size */
    tt_schema_create(db.db);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db.db,
        "INSERT INTO files (path, language, size_bytes, hash, indexed_at) "
        "VALUES (?, 'c', ?, 'abc', datetime('now'))",
        -1, &stmt, NULL);
    TT_ASSERT_EQ_INT(rc, SQLITE_OK);

    sqlite3_bind_text(stmt, 1, "src/main.c", -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, 1234);
    rc = sqlite3_step(stmt);
    TT_ASSERT_EQ_INT(rc, SQLITE_DONE);
    sqlite3_finalize(stmt);

    int64_t total = tt_savings_raw_from_index(&db);
    TT_ASSERT_TRUE(total == 1234);

    tt_database_close(&db);
}

/* ---- Runner ---- */

void run_int_savings_tests(void)
{
    savings_setup();

    TT_RUN(test_calculate_positive_savings);
    TT_RUN(test_calculate_no_savings);
    TT_RUN(test_calculate_equal_bytes);
    TT_RUN(test_calculate_zero_raw);

    TT_RUN(test_ensure_tables_creates_tables);
    TT_RUN(test_record_and_get_totals);
    TT_RUN(test_get_totals_empty);
    TT_RUN(test_reset);

    TT_RUN(test_raw_from_file_sizes);
    TT_RUN(test_raw_from_file_sizes_empty);
    TT_RUN(test_raw_from_file);
    TT_RUN(test_raw_from_file_nonexistent);
    TT_RUN(test_raw_from_file_null);

    TT_RUN(test_savings_track_records_to_db);
    TT_RUN(test_savings_track_no_savings_when_response_larger);
    TT_RUN(test_savings_track_null_guards);

    TT_RUN(test_per_tool_breakdown);
    TT_RUN(test_per_tool_empty);
    TT_RUN(test_reset_clears_per_tool);

    TT_RUN(test_raw_from_index);

    savings_cleanup();
}
