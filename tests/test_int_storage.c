/*
 * test_int_storage.c -- Integration tests for the storage layer.
 *
 * Covers: schema, database, index_store (CRUD, FTS5, change detection,
 * file summaries, stats, update summaries, batch operations).
 */

#include "test_framework.h"
#include "test_helpers.h"
#include "storage_paths.h"
#include "schema.h"
#include "database.h"
#include "index_store.h"
#include "platform.h"
#include "error.h"
#include "str_util.h"
#include "hashmap.h"

#include <string.h>
#include <unistd.h>

/* Shared temp directory for this suite */
static char *s_tmpdir;

static void storage_setup(void)
{
    s_tmpdir = tt_test_tmpdir();
}

static void storage_cleanup(void)
{
    if (s_tmpdir) {
        tt_test_rmdir(s_tmpdir);
        free(s_tmpdir);
        s_tmpdir = NULL;
    }
}

/* ---- Schema tests ---- */

TT_TEST(test_schema_create_and_version)
{
    char *db_path = tt_path_join(s_tmpdir, "test_schema.sqlite");
    sqlite3 *db = NULL;
    int rc = sqlite3_open(db_path, &db);
    TT_ASSERT_EQ_INT(rc, SQLITE_OK);

    int ver = tt_schema_check_version(db);
    TT_ASSERT_EQ_INT(ver, 0);

    rc = tt_schema_create(db);
    TT_ASSERT_EQ_INT(rc, 0);

    ver = tt_schema_check_version(db);
    TT_ASSERT_EQ_INT(ver, 4);

    /* Idempotent */
    rc = tt_schema_create(db);
    TT_ASSERT_EQ_INT(rc, 0);

    sqlite3_close(db);
    free(db_path);
}

TT_TEST(test_schema_migrate)
{
    char *db_path = tt_path_join(s_tmpdir, "test_migrate.sqlite");
    sqlite3 *db = NULL;
    sqlite3_open(db_path, &db);

    int rc = tt_schema_migrate(db);
    TT_ASSERT_EQ_INT(rc, 0);

    int ver = tt_schema_check_version(db);
    TT_ASSERT_EQ_INT(ver, 4);

    sqlite3_close(db);
    free(db_path);
}

/* ---- Database tests ---- */

TT_TEST(test_database_open_close)
{
    tt_database_t db;
    int rc = tt_database_open(&db, s_tmpdir);
    TT_ASSERT_EQ_INT(rc, 0);
    TT_ASSERT_NOT_NULL(db.db);
    TT_ASSERT_NOT_NULL(db.path);
    TT_ASSERT_NOT_NULL(db.project_root);

    int64_t size = tt_database_size(&db);
    TT_ASSERT(size > 0, "db size > 0");

    tt_database_close(&db);
}

TT_TEST(test_database_exists)
{
    TT_ASSERT_TRUE(tt_database_exists(s_tmpdir));
    TT_ASSERT_FALSE(tt_database_exists("/nonexistent/path/xyz"));
}

TT_TEST(test_database_transactions)
{
    tt_database_t db;
    tt_database_open(&db, s_tmpdir);

    int rc = tt_database_begin(&db);
    TT_ASSERT_EQ_INT(rc, 0);
    TT_ASSERT_TRUE(db.in_transaction);

    /* Double begin = no-op */
    rc = tt_database_begin(&db);
    TT_ASSERT_EQ_INT(rc, 0);

    rc = tt_database_commit(&db);
    TT_ASSERT_EQ_INT(rc, 0);
    TT_ASSERT_FALSE(db.in_transaction);

    /* Rollback when not in transaction = no-op */
    rc = tt_database_rollback(&db);
    TT_ASSERT_EQ_INT(rc, 0);

    /* Begin + rollback */
    tt_database_begin(&db);
    rc = tt_database_rollback(&db);
    TT_ASSERT_EQ_INT(rc, 0);

    tt_database_close(&db);
}

/* ---- Index Store tests ---- */

TT_TEST(test_store_init_close)
{
    tt_database_t db;
    tt_database_open(&db, s_tmpdir);

    tt_index_store_t store;
    int rc = tt_store_init(&store, &db);
    TT_ASSERT_EQ_INT(rc, 0);
    TT_ASSERT_NOT_NULL(store.insert_file);
    TT_ASSERT_NOT_NULL(store.insert_sym);

    tt_store_close(&store);
    tt_database_close(&db);
}

TT_TEST(test_store_insert_and_get_file)
{
    tt_database_t db;
    tt_database_open(&db, s_tmpdir);
    tt_index_store_t store;
    tt_store_init(&store, &db);

    int rc = tt_store_insert_file(&store, "src/main.c", "abc123",
                                   "c", 1024, 0, 0, "main entry");
    TT_ASSERT_EQ_INT(rc, 0);

    tt_file_record_t file;
    rc = tt_store_get_file(&store, "src/main.c", &file);
    TT_ASSERT_EQ_INT(rc, 0);
    TT_ASSERT_EQ_STR(file.path, "src/main.c");
    TT_ASSERT_EQ_STR(file.hash, "abc123");
    TT_ASSERT_EQ_STR(file.language, "c");
    TT_ASSERT_EQ_STR(file.summary, "main entry");
    TT_ASSERT_EQ_INT((int)file.size_bytes, 1024);
    TT_ASSERT(file.indexed_at[0] != '\0', "indexed_at set");

    tt_file_record_free(&file);

    /* Not found */
    rc = tt_store_get_file(&store, "nonexistent.c", &file);
    TT_ASSERT_EQ_INT(rc, -1);

    tt_store_close(&store);
    tt_database_close(&db);
}

TT_TEST(test_store_insert_and_get_symbol)
{
    tt_database_t db;
    tt_database_open(&db, s_tmpdir);
    tt_index_store_t store;
    tt_store_init(&store, &db);

    tt_store_insert_file(&store, "src/auth.c", "def456", "c", 2048, 0, 0, "");

    tt_symbol_t sym;
    memset(&sym, 0, sizeof(sym));
    sym.id = tt_strdup("src/auth.c::login#function");
    sym.file = tt_strdup("src/auth.c");
    sym.name = tt_strdup("login");
    sym.qualified_name = tt_strdup("login");
    sym.kind = TT_KIND_FUNCTION;
    sym.language = tt_strdup("c");
    sym.signature = tt_strdup("int login(const char *user)");
    sym.docstring = tt_strdup("Authenticate user.");
    sym.summary = tt_strdup("");
    sym.decorators = tt_strdup("[]");
    sym.keywords = tt_strdup("[\"login\"]");
    sym.parent_id = NULL;
    sym.line = 10;
    sym.end_line = 25;
    sym.byte_offset = 200;
    sym.byte_length = 500;
    sym.content_hash = tt_strdup("hash123");

    int rc = tt_store_insert_symbol(&store, &sym);
    TT_ASSERT_EQ_INT(rc, 0);

    tt_symbol_result_t result;
    rc = tt_store_get_symbol(&store, "src/auth.c::login#function", &result);
    TT_ASSERT_EQ_INT(rc, 0);
    TT_ASSERT_EQ_STR(result.sym.name, "login");
    TT_ASSERT_TRUE(result.sym.kind == TT_KIND_FUNCTION);
    TT_ASSERT_EQ_INT(result.sym.line, 10);
    TT_ASSERT_EQ_INT(result.sym.end_line, 25);
    TT_ASSERT_EQ_STR(result.sym.signature, "int login(const char *user)");
    TT_ASSERT_NULL(result.sym.parent_id);

    tt_symbol_result_free(&result);
    tt_symbol_free(&sym);
    tt_store_close(&store);
    tt_database_close(&db);
}

TT_TEST(test_store_batch_insert_symbols)
{
    tt_database_t db;
    tt_database_open(&db, s_tmpdir);
    tt_index_store_t store;
    tt_store_init(&store, &db);

    tt_store_insert_file(&store, "src/util.c", "util111", "c", 500, 0, 0, "");

    tt_symbol_t syms[3];
    memset(syms, 0, sizeof(syms));

    syms[0].id = tt_strdup("src/util.c::foo#function");
    syms[0].file = tt_strdup("src/util.c");
    syms[0].name = tt_strdup("foo");
    syms[0].qualified_name = tt_strdup("foo");
    syms[0].kind = TT_KIND_FUNCTION;
    syms[0].language = tt_strdup("c");
    syms[0].content_hash = tt_strdup("h1");
    syms[0].line = 1; syms[0].end_line = 5;

    syms[1].id = tt_strdup("src/util.c::bar#function");
    syms[1].file = tt_strdup("src/util.c");
    syms[1].name = tt_strdup("bar");
    syms[1].qualified_name = tt_strdup("bar");
    syms[1].kind = TT_KIND_FUNCTION;
    syms[1].language = tt_strdup("c");
    syms[1].content_hash = tt_strdup("h2");
    syms[1].line = 10; syms[1].end_line = 20;

    syms[2].id = tt_strdup("src/util.c::Util#class");
    syms[2].file = tt_strdup("src/util.c");
    syms[2].name = tt_strdup("Util");
    syms[2].qualified_name = tt_strdup("Util");
    syms[2].kind = TT_KIND_CLASS;
    syms[2].language = tt_strdup("c");
    syms[2].content_hash = tt_strdup("h3");
    syms[2].line = 30; syms[2].end_line = 50;

    int rc = tt_store_insert_symbols(&store, syms, 3);
    TT_ASSERT_EQ_INT(rc, 0);

    tt_symbol_result_t *out = NULL;
    int count = 0;
    rc = tt_store_get_symbols_by_file(&store, "src/util.c", &out, &count);
    TT_ASSERT_EQ_INT(rc, 0);
    TT_ASSERT_EQ_INT(count, 3);
    TT_ASSERT_EQ_INT(out[0].sym.line, 1);
    TT_ASSERT_EQ_INT(out[2].sym.line, 30);

    for (int i = 0; i < count; i++) tt_symbol_result_free(&out[i]);
    free(out);
    for (int i = 0; i < 3; i++) tt_symbol_free(&syms[i]);
    tt_store_close(&store);
    tt_database_close(&db);
}

TT_TEST(test_store_metadata)
{
    tt_database_t db;
    tt_database_open(&db, s_tmpdir);
    tt_index_store_t store;
    tt_store_init(&store, &db);

    int rc = tt_store_set_metadata(&store, "test_key", "test_value");
    TT_ASSERT_EQ_INT(rc, 0);

    char *val = tt_store_get_metadata(&store, "test_key");
    TT_ASSERT_NOT_NULL(val);
    TT_ASSERT_EQ_STR(val, "test_value");
    free(val);

    tt_store_set_metadata(&store, "test_key", "updated");
    val = tt_store_get_metadata(&store, "test_key");
    TT_ASSERT_EQ_STR(val, "updated");
    free(val);

    val = tt_store_get_metadata(&store, "nonexistent_key");
    TT_ASSERT_NULL(val);

    tt_store_close(&store);
    tt_database_close(&db);
}

TT_TEST(test_store_delete_file_cascade)
{
    tt_database_t db;
    tt_database_open(&db, s_tmpdir);
    tt_index_store_t store;
    tt_store_init(&store, &db);

    tt_store_insert_file(&store, "src/delete_me.c", "del1", "c", 100, 0, 0, "");

    tt_symbol_t sym;
    memset(&sym, 0, sizeof(sym));
    sym.id = tt_strdup("src/delete_me.c::func#function");
    sym.file = tt_strdup("src/delete_me.c");
    sym.name = tt_strdup("func");
    sym.qualified_name = tt_strdup("func");
    sym.kind = TT_KIND_FUNCTION;
    sym.language = tt_strdup("c");
    sym.content_hash = tt_strdup("dh1");
    sym.line = 1; sym.end_line = 5;
    tt_store_insert_symbol(&store, &sym);

    int rc = tt_store_delete_symbols_by_file(&store, "src/delete_me.c");
    TT_ASSERT_EQ_INT(rc, 0);

    rc = tt_store_delete_file(&store, "src/delete_me.c");
    TT_ASSERT_EQ_INT(rc, 0);

    tt_file_record_t file;
    rc = tt_store_get_file(&store, "src/delete_me.c", &file);
    TT_ASSERT_EQ_INT(rc, -1);

    tt_symbol_free(&sym);
    tt_store_close(&store);
    tt_database_close(&db);
}

TT_TEST(test_store_fts5_search)
{
    tt_database_t db;
    tt_database_open(&db, s_tmpdir);
    tt_index_store_t store;
    tt_store_init(&store, &db);

    tt_store_truncate(&store);

    tt_store_insert_file(&store, "src/auth.c", "a1", "c", 100, 0, 0, "");
    tt_store_insert_file(&store, "src/user.c", "u1", "c", 200, 0, 0, "");

    tt_symbol_t syms[4];
    memset(syms, 0, sizeof(syms));

    syms[0].id = tt_strdup("src/auth.c::authenticate#function");
    syms[0].file = tt_strdup("src/auth.c");
    syms[0].name = tt_strdup("authenticate");
    syms[0].qualified_name = tt_strdup("authenticate");
    syms[0].kind = TT_KIND_FUNCTION;
    syms[0].language = tt_strdup("c");
    syms[0].signature = tt_strdup("int authenticate(const char *user, const char *pass)");
    syms[0].content_hash = tt_strdup("ch1");
    syms[0].line = 1; syms[0].end_line = 20;

    syms[1].id = tt_strdup("src/auth.c::AuthManager#class");
    syms[1].file = tt_strdup("src/auth.c");
    syms[1].name = tt_strdup("AuthManager");
    syms[1].qualified_name = tt_strdup("AuthManager");
    syms[1].kind = TT_KIND_CLASS;
    syms[1].language = tt_strdup("c");
    syms[1].content_hash = tt_strdup("ch2");
    syms[1].line = 25; syms[1].end_line = 100;

    syms[2].id = tt_strdup("src/user.c::UserService#class");
    syms[2].file = tt_strdup("src/user.c");
    syms[2].name = tt_strdup("UserService");
    syms[2].qualified_name = tt_strdup("UserService");
    syms[2].kind = TT_KIND_CLASS;
    syms[2].language = tt_strdup("c");
    syms[2].content_hash = tt_strdup("ch3");
    syms[2].line = 1; syms[2].end_line = 50;

    syms[3].id = tt_strdup("src/user.c::getUser#method");
    syms[3].file = tt_strdup("src/user.c");
    syms[3].name = tt_strdup("getUser");
    syms[3].qualified_name = tt_strdup("UserService.getUser");
    syms[3].kind = TT_KIND_METHOD;
    syms[3].language = tt_strdup("c");
    syms[3].content_hash = tt_strdup("ch4");
    syms[3].line = 10; syms[3].end_line = 30;

    tt_database_begin(&db);
    for (int i = 0; i < 4; i++)
        tt_store_insert_symbol(&store, &syms[i]);
    tt_database_commit(&db);

    /* Search for "auth" */
    tt_search_results_t results;
    int rc = tt_store_search_symbols(&store, "auth", NULL, 0, NULL, NULL, 50, false, &results);
    TT_ASSERT_EQ_INT(rc, 0);
    TT_ASSERT(results.count >= 2, "should find auth symbols");

    bool found_authenticate = false;
    bool found_authmanager = false;
    for (int i = 0; i < results.count; i++) {
        if (strcmp(results.results[i].sym.name, "authenticate") == 0)
            found_authenticate = true;
        if (strcmp(results.results[i].sym.name, "AuthManager") == 0)
            found_authmanager = true;
    }
    TT_ASSERT_TRUE(found_authenticate);
    TT_ASSERT_TRUE(found_authmanager);
    tt_search_results_free(&results);

    /* Kind filter */
    const char *class_kind[] = { "class" };
    rc = tt_store_search_symbols(&store, "auth", class_kind, 1, NULL, NULL, 50, false, &results);
    TT_ASSERT_EQ_INT(rc, 0);
    for (int i = 0; i < results.count; i++)
        TT_ASSERT_TRUE(results.results[i].sym.kind == TT_KIND_CLASS);
    tt_search_results_free(&results);

    /* matchAll */
    rc = tt_store_search_symbols(&store, "", NULL, 0, NULL, NULL, 50, false, &results);
    TT_ASSERT_EQ_INT(rc, 0);
    TT_ASSERT_EQ_INT(results.count, 4);
    tt_search_results_free(&results);

    /* max_results */
    rc = tt_store_search_symbols(&store, "", NULL, 0, NULL, NULL, 2, false, &results);
    TT_ASSERT_EQ_INT(rc, 0);
    TT_ASSERT_EQ_INT(results.count, 2);
    tt_search_results_free(&results);

    for (int i = 0; i < 4; i++) tt_symbol_free(&syms[i]);
    tt_store_close(&store);
    tt_database_close(&db);
}

TT_TEST(test_store_search_wildcard_star)
{
    tt_database_t db;
    tt_database_open(&db, s_tmpdir);
    tt_index_store_t store;
    tt_store_init(&store, &db);

    /* Search with "*" should fall back to match_all and return results */
    tt_search_results_t results;
    int rc = tt_store_search_symbols(&store, "*", NULL, 0, NULL, NULL, 50, false, &results);
    TT_ASSERT_EQ_INT(rc, 0);
    TT_ASSERT(results.count >= 1, "wildcard * should return results");
    tt_search_results_free(&results);

    tt_store_close(&store);
    tt_database_close(&db);
}

TT_TEST(test_store_search_regex_mode)
{
    tt_database_t db;
    tt_database_open(&db, s_tmpdir);
    tt_index_store_t store;
    tt_store_init(&store, &db);

    tt_search_results_t results;
    int rc = tt_store_search_symbols(&store, "user", NULL, 0, NULL, NULL, 50, true, &results);
    TT_ASSERT_EQ_INT(rc, 0);
    TT_ASSERT(results.count >= 1, "regex should find matches");
    tt_search_results_free(&results);

    tt_store_close(&store);
    tt_database_close(&db);
}

TT_TEST(test_store_get_symbols_by_ids)
{
    tt_database_t db;
    tt_database_open(&db, s_tmpdir);
    tt_index_store_t store;
    tt_store_init(&store, &db);

    const char *ids[] = {
        "src/auth.c::authenticate#function",
        "nonexistent::foo#function",
        "src/user.c::UserService#class"
    };

    tt_symbol_result_t *out = NULL;
    int out_count = 0;
    char **errors = NULL;
    int error_count = 0;

    int rc = tt_store_get_symbols_by_ids(&store, ids, 3, &out, &out_count, &errors, &error_count);
    TT_ASSERT_EQ_INT(rc, 0);
    TT_ASSERT_EQ_INT(out_count, 2);
    TT_ASSERT_EQ_INT(error_count, 1);

    for (int i = 0; i < out_count; i++) tt_symbol_result_free(&out[i]);
    free(out);
    for (int i = 0; i < error_count; i++) free(errors[i]);
    free(errors);

    tt_store_close(&store);
    tt_database_close(&db);
}

TT_TEST(test_store_file_hashes)
{
    tt_database_t db;
    tt_database_open(&db, s_tmpdir);
    tt_index_store_t store;
    tt_store_init(&store, &db);

    tt_hashmap_t *hashes = NULL;
    int rc = tt_store_get_file_hashes(&store, &hashes);
    TT_ASSERT_EQ_INT(rc, 0);
    TT_ASSERT_NOT_NULL(hashes);
    TT_ASSERT(tt_hashmap_count(hashes) > 0, "should have entries");

    tt_hashmap_free_with_values(hashes);
    tt_store_close(&store);
    tt_database_close(&db);
}

TT_TEST(test_store_file_summaries)
{
    tt_database_t db;
    tt_database_open(&db, s_tmpdir);
    tt_index_store_t store;
    tt_store_init(&store, &db);

    tt_store_truncate(&store);

    tt_store_insert_file(&store, "src/app.c", "ap1", "c", 1000, 0, 0, "");

    tt_symbol_t syms[4];
    memset(syms, 0, sizeof(syms));

    const char *names[] = { "init", "run", "stop", "App" };
    const char *kind_strs[] = { "function", "function", "function", "class" };
    tt_symbol_kind_e kind_enums[] = { TT_KIND_FUNCTION, TT_KIND_FUNCTION, TT_KIND_FUNCTION, TT_KIND_CLASS };

    for (int i = 0; i < 4; i++) {
        char id_buf[128];
        snprintf(id_buf, sizeof(id_buf), "src/app.c::%s#%s", names[i], kind_strs[i]);
        syms[i].id = tt_strdup(id_buf);
        syms[i].file = tt_strdup("src/app.c");
        syms[i].name = tt_strdup(names[i]);
        syms[i].qualified_name = tt_strdup(names[i]);
        syms[i].kind = kind_enums[i];
        syms[i].language = tt_strdup("c");
        syms[i].content_hash = tt_strdup("h");
        syms[i].line = (i + 1) * 10;
        syms[i].end_line = (i + 1) * 10 + 5;
    }

    tt_store_insert_symbols(&store, syms, 4);

    int rc = tt_store_generate_file_summaries(&store);
    TT_ASSERT_EQ_INT(rc, 0);

    tt_file_record_t file;
    rc = tt_store_get_file(&store, "src/app.c", &file);
    TT_ASSERT_EQ_INT(rc, 0);
    TT_ASSERT(file.summary[0] != '\0', "summary not empty");
    TT_ASSERT_STR_CONTAINS(file.summary, "3 functions");
    TT_ASSERT_STR_CONTAINS(file.summary, "1 class");

    tt_file_record_free(&file);
    for (int i = 0; i < 4; i++) tt_symbol_free(&syms[i]);
    tt_store_close(&store);
    tt_database_close(&db);
}

TT_TEST(test_store_stats)
{
    tt_database_t db;
    tt_database_open(&db, s_tmpdir);
    tt_index_store_t store;
    tt_store_init(&store, &db);

    tt_stats_t stats;
    int rc = tt_store_get_stats(&store, &stats);
    TT_ASSERT_EQ_INT(rc, 0);
    TT_ASSERT(stats.files >= 1, "has files");
    TT_ASSERT(stats.symbols >= 1, "has symbols");
    TT_ASSERT(stats.language_count >= 1, "has languages");
    TT_ASSERT(stats.kind_count >= 1, "has kinds");

    tt_stats_free(&stats);
    tt_store_close(&store);
    tt_database_close(&db);
}

TT_TEST(test_store_truncate)
{
    tt_database_t db;
    tt_database_open(&db, s_tmpdir);
    tt_index_store_t store;
    tt_store_init(&store, &db);

    int rc = tt_store_truncate(&store);
    TT_ASSERT_EQ_INT(rc, 0);

    tt_stats_t stats;
    tt_store_get_stats(&store, &stats);
    TT_ASSERT_EQ_INT(stats.files, 0);
    TT_ASSERT_EQ_INT(stats.symbols, 0);
    tt_stats_free(&stats);

    tt_store_close(&store);
    tt_database_close(&db);
}

TT_TEST(test_store_update_summaries)
{
    tt_database_t db;
    tt_database_open(&db, s_tmpdir);
    tt_index_store_t store;
    tt_store_init(&store, &db);

    tt_store_insert_file(&store, "src/test.c", "t1", "c", 50, 0, 0, "");

    tt_symbol_t sym;
    memset(&sym, 0, sizeof(sym));
    sym.id = tt_strdup("src/test.c::test_fn#function");
    sym.file = tt_strdup("src/test.c");
    sym.name = tt_strdup("test_fn");
    sym.qualified_name = tt_strdup("test_fn");
    sym.kind = TT_KIND_FUNCTION;
    sym.language = tt_strdup("c");
    sym.content_hash = tt_strdup("th1");
    sym.line = 1; sym.end_line = 10;

    tt_store_insert_symbol(&store, &sym);

    int rc = tt_store_update_symbol_summary(&store, sym.id, "Tests something.");
    TT_ASSERT_EQ_INT(rc, 0);

    tt_symbol_result_t result;
    tt_store_get_symbol(&store, sym.id, &result);
    TT_ASSERT_EQ_STR(result.sym.summary, "Tests something.");
    tt_symbol_result_free(&result);

    rc = tt_store_update_file_summary(&store, "src/test.c", "1 function");
    TT_ASSERT_EQ_INT(rc, 0);

    tt_file_record_t file;
    tt_store_get_file(&store, "src/test.c", &file);
    TT_ASSERT_EQ_STR(file.summary, "1 function");
    tt_file_record_free(&file);

    tt_symbol_free(&sym);
    tt_store_close(&store);
    tt_database_close(&db);
}

TT_TEST(test_store_symbols_without_summary)
{
    tt_database_t db;
    tt_database_open(&db, s_tmpdir);
    tt_index_store_t store;
    tt_store_init(&store, &db);

    tt_store_insert_file(&store, "src/nosummary.c", "ns1", "c", 30, 0, 0, "");

    tt_symbol_t sym;
    memset(&sym, 0, sizeof(sym));
    sym.id = tt_strdup("src/nosummary.c::no_doc#function");
    sym.file = tt_strdup("src/nosummary.c");
    sym.name = tt_strdup("no_doc");
    sym.qualified_name = tt_strdup("no_doc");
    sym.kind = TT_KIND_FUNCTION;
    sym.language = tt_strdup("c");
    sym.summary = tt_strdup("");
    sym.content_hash = tt_strdup("nsh1");
    sym.line = 1; sym.end_line = 5;

    tt_store_insert_symbol(&store, &sym);

    tt_symbol_result_t *out = NULL;
    int count = 0;
    int rc = tt_store_get_symbols_without_summary(&store, 100, &out, &count);
    TT_ASSERT_EQ_INT(rc, 0);
    TT_ASSERT(count >= 1, "at least 1");

    bool found = false;
    for (int i = 0; i < count; i++) {
        if (strcmp(out[i].sym.id, "src/nosummary.c::no_doc#function") == 0)
            found = true;
        tt_symbol_result_free(&out[i]);
    }
    free(out);
    TT_ASSERT_TRUE(found);

    tt_symbol_free(&sym);
    tt_store_close(&store);
    tt_database_close(&db);
}

TT_TEST(test_store_get_all_files)
{
    tt_database_t db;
    tt_database_open(&db, s_tmpdir);
    tt_index_store_t store;
    tt_store_init(&store, &db);

    tt_file_record_t *files = NULL;
    int count = 0;
    int rc = tt_store_get_all_files(&store, &files, &count);
    TT_ASSERT_EQ_INT(rc, 0);
    TT_ASSERT(count >= 1, "has files");

    for (int i = 0; i < count; i++) tt_file_record_free(&files[i]);
    free(files);

    tt_store_close(&store);
    tt_database_close(&db);
}

TT_TEST(test_store_schema_version_metadata)
{
    tt_database_t db;
    tt_database_open(&db, s_tmpdir);
    tt_index_store_t store;
    tt_store_init(&store, &db);

    char *ver = tt_store_get_metadata(&store, "schema_version");
    TT_ASSERT_NOT_NULL(ver);
    TT_ASSERT_EQ_STR(ver, "4");
    free(ver);

    tt_store_close(&store);
    tt_database_close(&db);
}

void run_int_storage_tests(void)
{
    storage_setup();

    TT_RUN(test_schema_create_and_version);
    TT_RUN(test_schema_migrate);
    TT_RUN(test_database_open_close);
    TT_RUN(test_database_exists);
    TT_RUN(test_database_transactions);
    TT_RUN(test_store_init_close);
    TT_RUN(test_store_insert_and_get_file);
    TT_RUN(test_store_insert_and_get_symbol);
    TT_RUN(test_store_batch_insert_symbols);
    TT_RUN(test_store_metadata);
    TT_RUN(test_store_schema_version_metadata);
    TT_RUN(test_store_delete_file_cascade);
    TT_RUN(test_store_fts5_search);
    TT_RUN(test_store_search_wildcard_star);
    TT_RUN(test_store_search_regex_mode);
    TT_RUN(test_store_get_symbols_by_ids);
    TT_RUN(test_store_file_hashes);

    TT_RUN(test_store_file_summaries);
    TT_RUN(test_store_stats);
    TT_RUN(test_store_update_summaries);
    TT_RUN(test_store_symbols_without_summary);
    TT_RUN(test_store_get_all_files);
    TT_RUN(test_store_truncate);

    storage_cleanup();
}
