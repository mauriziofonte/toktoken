/*
 * schema.c -- SQLite schema management for TokToken.
 */

#include "schema.h"
#include "error.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ---- DDL strings ---- */

static const char *DDL_TABLES =
    "CREATE TABLE IF NOT EXISTS metadata ("
    "    key TEXT PRIMARY KEY,"
    "    value TEXT NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS files ("
    "    path TEXT PRIMARY KEY,"
    "    hash TEXT NOT NULL,"
    "    language TEXT NOT NULL,"
    "    summary TEXT DEFAULT '',"
    "    size_bytes INTEGER NOT NULL,"
    "    mtime_sec INTEGER NOT NULL DEFAULT 0,"
    "    mtime_nsec INTEGER NOT NULL DEFAULT 0,"
    "    indexed_at TEXT NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS symbols ("
    "    id TEXT PRIMARY KEY,"
    "    file TEXT NOT NULL REFERENCES files(path) ON DELETE CASCADE,"
    "    name TEXT NOT NULL,"
    "    qualified_name TEXT NOT NULL,"
    "    kind TEXT NOT NULL,"
    "    language TEXT NOT NULL,"
    "    signature TEXT DEFAULT '',"
    "    docstring TEXT DEFAULT '',"
    "    summary TEXT DEFAULT '',"
    "    decorators TEXT DEFAULT '[]',"
    "    keywords TEXT DEFAULT '[]',"
    "    parent_id TEXT,"
    "    line INTEGER NOT NULL,"
    "    end_line INTEGER NOT NULL,"
    "    byte_offset INTEGER NOT NULL,"
    "    byte_length INTEGER NOT NULL,"
    "    content_hash TEXT NOT NULL"
    ");";

static const char *DDL_IMPORTS =
    "CREATE TABLE IF NOT EXISTS imports ("
    "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "    source_file TEXT NOT NULL REFERENCES files(path) ON DELETE CASCADE,"
    "    target_name TEXT NOT NULL,"
    "    resolved_file TEXT NOT NULL DEFAULT '',"
    "    kind TEXT NOT NULL DEFAULT 'module',"
    "    import_type TEXT NOT NULL DEFAULT 'import'"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_imports_source ON imports(source_file);"
    "CREATE INDEX IF NOT EXISTS idx_imports_target ON imports(target_name);"
    "CREATE INDEX IF NOT EXISTS idx_imports_resolved ON imports(resolved_file);"
    "CREATE INDEX IF NOT EXISTS idx_imports_kind ON imports(kind);";

static const char *DDL_CENTRALITY =
    "CREATE TABLE IF NOT EXISTS file_centrality ("
    "    file TEXT PRIMARY KEY,"
    "    score REAL NOT NULL DEFAULT 0.0"
    ");";

static const char *DDL_METADATA_INIT =
    "INSERT OR IGNORE INTO metadata (key, value) VALUES ('schema_version', '4');"
    "INSERT OR IGNORE INTO metadata (key, value) VALUES ('indexed_at', '');"
    "INSERT OR IGNORE INTO metadata (key, value) VALUES ('git_head', '');";

static const char *DDL_INDEXES =
    "CREATE INDEX IF NOT EXISTS idx_symbols_file_line ON symbols(file, line);"
    "CREATE INDEX IF NOT EXISTS idx_symbols_kind_lang ON symbols(kind, language);"
    "CREATE INDEX IF NOT EXISTS idx_symbols_parent ON symbols(parent_id);"
    "CREATE INDEX IF NOT EXISTS idx_files_language ON files(language);";

static const char *DDL_FTS5 =
    "CREATE VIRTUAL TABLE IF NOT EXISTS symbols_fts USING fts5("
    "    name, qualified_name, signature, summary,"
    "    content=symbols, content_rowid=rowid"
    ");";

static const char *DDL_TRIGGERS =
    "CREATE TRIGGER IF NOT EXISTS symbols_ai AFTER INSERT ON symbols BEGIN"
    "    INSERT INTO symbols_fts(rowid, name, qualified_name, signature, summary)"
    "    VALUES (new.rowid, new.name, new.qualified_name, new.signature, new.summary);"
    "END;"
    "CREATE TRIGGER IF NOT EXISTS symbols_ad AFTER DELETE ON symbols BEGIN"
    "    INSERT INTO symbols_fts(symbols_fts, rowid, name, qualified_name, signature, summary)"
    "    VALUES ('delete', old.rowid, old.name, old.qualified_name, old.signature, old.summary);"
    "END;"
    "CREATE TRIGGER IF NOT EXISTS symbols_au AFTER UPDATE ON symbols BEGIN"
    "    INSERT INTO symbols_fts(symbols_fts, rowid, name, qualified_name, signature, summary)"
    "    VALUES ('delete', old.rowid, old.name, old.qualified_name, old.signature, old.summary);"
    "    INSERT INTO symbols_fts(rowid, name, qualified_name, signature, summary)"
    "    VALUES (new.rowid, new.name, new.qualified_name, new.signature, new.summary);"
    "END;";

/* Execute a multi-statement SQL string. Returns 0 on success. */
static int exec_sql(sqlite3 *db, const char *sql)
{
    char *errmsg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK)
    {
        tt_error_set("schema: SQL exec failed: %s", errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
        return -1;
    }
    return 0;
}

int tt_schema_create(sqlite3 *db)
{
    if (exec_sql(db, DDL_TABLES) < 0)
        return -1;
    if (exec_sql(db, DDL_IMPORTS) < 0)
        return -1;
    if (exec_sql(db, DDL_CENTRALITY) < 0)
        return -1;
    if (exec_sql(db, DDL_METADATA_INIT) < 0)
        return -1;
    if (exec_sql(db, DDL_INDEXES) < 0)
        return -1;
    if (exec_sql(db, DDL_FTS5) < 0)
        return -1;
    if (exec_sql(db, DDL_TRIGGERS) < 0)
        return -1;
    return 0;
}

int tt_schema_check_version(sqlite3 *db)
{
    /* Check if metadata table exists */
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "SELECT value FROM metadata WHERE key = 'schema_version'",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        /* Table doesn't exist */
        return 0;
    }

    int version = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const char *val = (const char *)sqlite3_column_text(stmt, 0);
        if (val)
            version = atoi(val);
    }
    sqlite3_finalize(stmt);
    return version;
}

int tt_schema_drop_fts_triggers(sqlite3 *db)
{
    return exec_sql(db,
        "DROP TRIGGER IF EXISTS symbols_ai;"
        "DROP TRIGGER IF EXISTS symbols_ad;"
        "DROP TRIGGER IF EXISTS symbols_au;");
}

int tt_schema_create_fts_triggers(sqlite3 *db)
{
    return exec_sql(db, DDL_TRIGGERS);
}

int tt_schema_drop_secondary_indexes(sqlite3 *db)
{
    return exec_sql(db,
        "DROP INDEX IF EXISTS idx_symbols_name;"
        "DROP INDEX IF EXISTS idx_symbols_kind;"
        "DROP INDEX IF EXISTS idx_symbols_file_line;"
        "DROP INDEX IF EXISTS idx_symbols_language;"
        "DROP INDEX IF EXISTS idx_symbols_qualified;"
        "DROP INDEX IF EXISTS idx_symbols_kind_lang;"
        "DROP INDEX IF EXISTS idx_symbols_parent;"
        "DROP INDEX IF EXISTS idx_files_language;");
}

int tt_schema_create_secondary_indexes(sqlite3 *db)
{
    return exec_sql(db, DDL_INDEXES);
}

int tt_schema_rebuild_fts(sqlite3 *db)
{
    return exec_sql(db,
        "INSERT INTO symbols_fts(symbols_fts) VALUES('rebuild');");
}

int tt_schema_migrate(sqlite3 *db)
{
    int version = tt_schema_check_version(db);

    if (version < 1)
    {
        /* No schema at all: create from scratch */
        return tt_schema_create(db);
    }

    if (version < 3)
    {
        /* v2 -> v3: Remove 4 redundant indexes, add parent_id index */
        if (exec_sql(db,
                "DROP INDEX IF EXISTS idx_symbols_name;"
                "DROP INDEX IF EXISTS idx_symbols_kind;"
                "DROP INDEX IF EXISTS idx_symbols_language;"
                "DROP INDEX IF EXISTS idx_symbols_qualified;"
                "CREATE INDEX IF NOT EXISTS idx_symbols_parent ON symbols(parent_id);"
                "INSERT OR REPLACE INTO metadata (key, value) VALUES ('schema_version', '3');") < 0)
            return -1;
    }

    if (version < 4)
    {
        /* v3 -> v4: Add file_centrality table + resolved_file column */
        if (exec_sql(db,
                "CREATE TABLE IF NOT EXISTS file_centrality ("
                "    file TEXT PRIMARY KEY,"
                "    score REAL NOT NULL DEFAULT 0.0"
                ");"
                "ALTER TABLE imports ADD COLUMN resolved_file TEXT NOT NULL DEFAULT '';"
                "CREATE INDEX IF NOT EXISTS idx_imports_resolved ON imports(resolved_file);"
                "INSERT OR REPLACE INTO metadata (key, value) VALUES ('schema_version', '4');") < 0)
            return -1;
    }

    return 0;
}
