/*
 * test_int_resolve.c -- DB-level tests for tt_store_resolve_imports().
 *
 * Tests import resolution in isolation: inserts files and imports directly
 * into SQLite, runs resolution, and verifies the resolved_file column.
 * No ctags, no pipeline -- pure DB logic.
 *
 * Covers all 3 resolution strategies:
 *   1. Direct exact match (C/C++ includes)
 *   2. Relative path resolution (JS/TS/Ruby ./ ../)
 *   3. Normalized suffix matching (PHP \ , Python . , Java . , Rust :: , Go /)
 */

#include "test_framework.h"
#include "test_helpers.h"

#include "schema.h"
#include "database.h"
#include "index_store.h"
#include "platform.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Shared state ---- */

static char *s_tmpdir;
static tt_database_t s_db;
static tt_index_store_t s_store;

/* ---- Helpers ---- */

static void insert_file(const char *path, const char *lang)
{
    sqlite3 *db = s_store.db->db;
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT OR IGNORE INTO files (path, language, hash, size_bytes, indexed_at) "
                      "VALUES (?, ?, 'deadbeef', 100, '2025-01-01T00:00:00Z')";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, lang, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

static int insert_import(const char *source, const char *target, const char *kind)
{
    sqlite3 *db = s_store.db->db;
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO imports (source_file, target_name, kind, resolved_file) "
                      "VALUES (?, ?, ?, '')";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, source, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, target, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, kind, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    int rowid = (int)sqlite3_last_insert_rowid(db);
    sqlite3_finalize(stmt);
    return rowid;
}

static char *get_resolved(int import_id)
{
    sqlite3 *db = s_store.db->db;
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT resolved_file FROM imports WHERE id = ?";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int(stmt, 1, import_id);
    char *result = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const char *val = (const char *)sqlite3_column_text(stmt, 0);
        result = val ? strdup(val) : strdup("");
    }
    sqlite3_finalize(stmt);
    return result;
}

static void reset_db(void)
{
    sqlite3 *db = s_store.db->db;
    sqlite3_exec(db, "DELETE FROM imports", NULL, NULL, NULL);
    sqlite3_exec(db, "DELETE FROM files", NULL, NULL, NULL);
}

/* ---- Setup / Teardown ---- */

static void resolve_setup(void)
{
    s_tmpdir = tt_test_tmpdir();
    if (!s_tmpdir)
    {
        fprintf(stderr, "  FATAL: tt_test_tmpdir() returned NULL\n");
        return;
    }
    memset(&s_db, 0, sizeof(s_db));
    if (tt_database_open(&s_db, s_tmpdir) < 0)
    {
        fprintf(stderr, "  FATAL: tt_database_open failed\n");
        return;
    }
    tt_schema_create(s_db.db);
    if (tt_store_init(&s_store, &s_db) < 0)
    {
        fprintf(stderr, "  FATAL: tt_store_init failed\n");
        return;
    }
}

static void resolve_cleanup(void)
{
    tt_store_close(&s_store);
    tt_database_close(&s_db);
    if (s_tmpdir)
    {
        tt_test_rmdir(s_tmpdir);
        free(s_tmpdir);
        s_tmpdir = NULL;
    }
}

/* ====================================================================
 * Strategy 1: Direct exact match
 * ==================================================================== */

TT_TEST(test_resolve_c_include_quoted)
{
    reset_db();
    insert_file("src/app.c", "c");
    insert_file("src/utils.h", "cpp");
    int id = insert_import("src/app.c", "src/utils.h", "include");
    tt_store_resolve_imports(&s_store);
    char *r = get_resolved(id);
    TT_ASSERT_EQ_STR(r, "src/utils.h");
    free(r);
}

TT_TEST(test_resolve_c_system_header)
{
    reset_db();
    insert_file("src/app.c", "c");
    int id = insert_import("src/app.c", "stdio.h", "include");
    tt_store_resolve_imports(&s_store);
    char *r = get_resolved(id);
    TT_ASSERT_EQ_STR(r, "");
    free(r);
}

TT_TEST(test_resolve_c_subdir_include)
{
    reset_db();
    insert_file("src/app.c", "c");
    insert_file("include/config.h", "cpp");
    int id = insert_import("src/app.c", "include/config.h", "include");
    tt_store_resolve_imports(&s_store);
    char *r = get_resolved(id);
    TT_ASSERT_EQ_STR(r, "include/config.h");
    free(r);
}

/* ====================================================================
 * Strategy 2: Relative path resolution (JS/TS/Ruby ./ ../)
 * ==================================================================== */

TT_TEST(test_resolve_js_relative_dot)
{
    reset_db();
    insert_file("src/app.ts", "typescript");
    insert_file("src/utils.ts", "typescript");
    int id = insert_import("src/app.ts", "./utils", "import");
    tt_store_resolve_imports(&s_store);
    char *r = get_resolved(id);
    TT_ASSERT_EQ_STR(r, "src/utils.ts");
    free(r);
}

TT_TEST(test_resolve_js_relative_dotdot)
{
    reset_db();
    insert_file("src/pages/home.ts", "typescript");
    insert_file("src/utils.ts", "typescript");
    int id = insert_import("src/pages/home.ts", "../utils", "import");
    tt_store_resolve_imports(&s_store);
    char *r = get_resolved(id);
    TT_ASSERT_EQ_STR(r, "src/utils.ts");
    free(r);
}

TT_TEST(test_resolve_js_index_resolution)
{
    reset_db();
    insert_file("src/app.ts", "typescript");
    insert_file("src/components/index.ts", "typescript");
    int id = insert_import("src/app.ts", "./components", "import");
    tt_store_resolve_imports(&s_store);
    char *r = get_resolved(id);
    TT_ASSERT_EQ_STR(r, "src/components/index.ts");
    free(r);
}

TT_TEST(test_resolve_js_exact_with_ext)
{
    reset_db();
    insert_file("src/app.js", "javascript");
    insert_file("src/styles.css", "css");
    int id = insert_import("src/app.js", "./styles.css", "import");
    tt_store_resolve_imports(&s_store);
    char *r = get_resolved(id);
    TT_ASSERT_EQ_STR(r, "src/styles.css");
    free(r);
}

TT_TEST(test_resolve_ruby_relative)
{
    reset_db();
    insert_file("lib/main.rb", "ruby");
    insert_file("lib/helpers.rb", "ruby");
    int id = insert_import("lib/main.rb", "./helpers", "require_relative");
    tt_store_resolve_imports(&s_store);
    char *r = get_resolved(id);
    TT_ASSERT_EQ_STR(r, "lib/helpers.rb");
    free(r);
}

TT_TEST(test_resolve_ts_dotted_file)
{
    reset_db();
    insert_file("pages/index.ts", "typescript");
    insert_file("app/app.types.ts", "typescript");
    int id = insert_import("pages/index.ts", "../app/app.types", "import");
    tt_store_resolve_imports(&s_store);
    char *r = get_resolved(id);
    TT_ASSERT_EQ_STR(r, "app/app.types.ts");
    free(r);
}

/* ====================================================================
 * Strategy 3: Normalized suffix matching
 * ==================================================================== */

TT_TEST(test_resolve_php_namespace)
{
    reset_db();
    insert_file("src/routes.php", "php");
    insert_file("app/Http/Controller.php", "php");
    int id = insert_import("src/routes.php", "App\\Http\\Controller", "use");
    tt_store_resolve_imports(&s_store);
    char *r = get_resolved(id);
    TT_ASSERT_EQ_STR(r, "app/Http/Controller.php");
    free(r);
}

TT_TEST(test_resolve_php_deep_namespace)
{
    reset_db();
    insert_file("src/routes.php", "php");
    insert_file("app/Http/Middleware/Auth.php", "php");
    int id = insert_import("src/routes.php", "App\\Http\\Middleware\\Auth", "use");
    tt_store_resolve_imports(&s_store);
    char *r = get_resolved(id);
    TT_ASSERT_EQ_STR(r, "app/Http/Middleware/Auth.php");
    free(r);
}

TT_TEST(test_resolve_php_vendor_unresolvable)
{
    reset_db();
    insert_file("src/routes.php", "php");
    int id = insert_import("src/routes.php", "Illuminate\\Support\\Facades\\DB", "use");
    tt_store_resolve_imports(&s_store);
    char *r = get_resolved(id);
    TT_ASSERT_EQ_STR(r, "");
    free(r);
}

TT_TEST(test_resolve_php_case_insensitive)
{
    reset_db();
    insert_file("src/routes.php", "php");
    insert_file("app/Http/Controller.php", "php");
    int id = insert_import("src/routes.php", "app\\http\\controller", "use");
    tt_store_resolve_imports(&s_store);
    char *r = get_resolved(id);
    TT_ASSERT_EQ_STR(r, "app/Http/Controller.php");
    free(r);
}

TT_TEST(test_resolve_python_dot_module)
{
    reset_db();
    insert_file("src/main.py", "python");
    insert_file("src/services/auth.py", "python");
    int id = insert_import("src/main.py", "services.auth", "import");
    tt_store_resolve_imports(&s_store);
    char *r = get_resolved(id);
    TT_ASSERT_EQ_STR(r, "src/services/auth.py");
    free(r);
}

TT_TEST(test_resolve_python_init)
{
    reset_db();
    insert_file("src/main.py", "python");
    insert_file("src/models/__init__.py", "python");
    int id = insert_import("src/main.py", "models", "import");
    tt_store_resolve_imports(&s_store);
    char *r = get_resolved(id);
    TT_ASSERT_EQ_STR(r, "src/models/__init__.py");
    free(r);
}

TT_TEST(test_resolve_python_vendor)
{
    reset_db();
    insert_file("src/main.py", "python");
    int id = insert_import("src/main.py", "flask.views", "import");
    tt_store_resolve_imports(&s_store);
    char *r = get_resolved(id);
    TT_ASSERT_EQ_STR(r, "");
    free(r);
}

TT_TEST(test_resolve_java_package)
{
    reset_db();
    insert_file("src/Main.java", "java");
    insert_file("src/com/app/UserService.java", "java");
    int id = insert_import("src/Main.java", "com.app.UserService", "import");
    tt_store_resolve_imports(&s_store);
    char *r = get_resolved(id);
    TT_ASSERT_EQ_STR(r, "src/com/app/UserService.java");
    free(r);
}

TT_TEST(test_resolve_kotlin_import)
{
    reset_db();
    insert_file("src/App.kt", "kotlin");
    insert_file("src/data/Repository.kt", "kotlin");
    int id = insert_import("src/App.kt", "data.Repository", "import");
    tt_store_resolve_imports(&s_store);
    char *r = get_resolved(id);
    TT_ASSERT_EQ_STR(r, "src/data/Repository.kt");
    free(r);
}

TT_TEST(test_resolve_scala_import)
{
    reset_db();
    insert_file("src/Main.scala", "scala");
    insert_file("src/models/Actor.scala", "scala");
    int id = insert_import("src/Main.scala", "models.Actor", "import");
    tt_store_resolve_imports(&s_store);
    char *r = get_resolved(id);
    TT_ASSERT_EQ_STR(r, "src/models/Actor.scala");
    free(r);
}

TT_TEST(test_resolve_rust_mod)
{
    reset_db();
    insert_file("src/main.rs", "rust");
    insert_file("src/config.rs", "rust");
    int id = insert_import("src/main.rs", "config", "mod");
    tt_store_resolve_imports(&s_store);
    char *r = get_resolved(id);
    TT_ASSERT_EQ_STR(r, "src/config.rs");
    free(r);
}

TT_TEST(test_resolve_go_module_path)
{
    reset_db();
    insert_file("cmd/main.go", "go");
    insert_file("pkg/handler.go", "go");
    int id = insert_import("cmd/main.go", "pkg/handler", "import");
    tt_store_resolve_imports(&s_store);
    char *r = get_resolved(id);
    TT_ASSERT_EQ_STR(r, "pkg/handler.go");
    free(r);
}

TT_TEST(test_resolve_csharp_using)
{
    reset_db();
    insert_file("src/Program.cs", "csharp");
    insert_file("src/Data/Repository.cs", "csharp");
    int id = insert_import("src/Program.cs", "Data.Repository", "using");
    tt_store_resolve_imports(&s_store);
    char *r = get_resolved(id);
    TT_ASSERT_EQ_STR(r, "src/Data/Repository.cs");
    free(r);
}

TT_TEST(test_resolve_ambiguous_suffix)
{
    reset_db();
    insert_file("src/main.py", "python");
    insert_file("src/utils.py", "python");
    insert_file("lib/utils.py", "python");
    int id = insert_import("src/main.py", "utils", "import");
    tt_store_resolve_imports(&s_store);
    char *r = get_resolved(id);
    /* Same-directory resolution prefers src/utils.py (same dir as src/main.py)
     * over the ambiguous suffix match. This matches Python's actual behavior. */
    TT_ASSERT_EQ_STR(r, "src/utils.py");
    free(r);
}

TT_TEST(test_resolve_haskell_qualified)
{
    reset_db();
    insert_file("src/Main.hs", "haskell");
    insert_file("src/Data/Utils.hs", "haskell");
    int id = insert_import("src/Main.hs", "Data.Utils", "import");
    tt_store_resolve_imports(&s_store);
    char *r = get_resolved(id);
    TT_ASSERT_EQ_STR(r, "src/Data/Utils.hs");
    free(r);
}

/* ---- Suite runner ---- */

void run_int_resolve_tests(void)
{
    resolve_setup();

    /* Strategy 1: Direct exact match */
    TT_RUN(test_resolve_c_include_quoted);
    TT_RUN(test_resolve_c_system_header);
    TT_RUN(test_resolve_c_subdir_include);

    /* Strategy 2: Relative path resolution */
    TT_RUN(test_resolve_js_relative_dot);
    TT_RUN(test_resolve_js_relative_dotdot);
    TT_RUN(test_resolve_js_index_resolution);
    TT_RUN(test_resolve_js_exact_with_ext);
    TT_RUN(test_resolve_ruby_relative);
    TT_RUN(test_resolve_ts_dotted_file);

    /* Strategy 3: Normalized suffix matching */
    TT_RUN(test_resolve_php_namespace);
    TT_RUN(test_resolve_php_deep_namespace);
    TT_RUN(test_resolve_php_vendor_unresolvable);
    TT_RUN(test_resolve_php_case_insensitive);
    TT_RUN(test_resolve_python_dot_module);
    TT_RUN(test_resolve_python_init);
    TT_RUN(test_resolve_python_vendor);
    TT_RUN(test_resolve_java_package);
    TT_RUN(test_resolve_kotlin_import);
    TT_RUN(test_resolve_scala_import);
    TT_RUN(test_resolve_rust_mod);
    TT_RUN(test_resolve_go_module_path);
    TT_RUN(test_resolve_csharp_using);
    TT_RUN(test_resolve_ambiguous_suffix);
    TT_RUN(test_resolve_haskell_qualified);

    resolve_cleanup();
}
