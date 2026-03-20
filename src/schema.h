/*
 * schema.h -- SQLite schema management for TokToken.
 *
 * Creates the v1 schema (metadata, files, symbols, imports, FTS5, triggers,
 * indexes) and handles future migrations.
 */

#ifndef TT_SCHEMA_H
#define TT_SCHEMA_H

#include <sqlite3.h>

#define TT_SCHEMA_VERSION 4

/*
 * tt_schema_create -- Create all tables, indexes, triggers from scratch.
 *
 * Idempotent: uses CREATE TABLE IF NOT EXISTS.
 * Returns 0 on success, -1 on error.
 */
int tt_schema_create(sqlite3 *db);

/*
 * tt_schema_check_version -- Return current schema version.
 *
 * Returns the version number or 0 if the metadata table does not exist
 * or has no schema_version row.
 */
int tt_schema_check_version(sqlite3 *db);

/*
 * tt_schema_migrate -- Migrate from current version to latest.
 *
 * Applies all necessary migrations sequentially.
 * Returns 0 on success, -1 on error.
 */
int tt_schema_migrate(sqlite3 *db);

/* Drop FTS5 sync triggers -- call before bulk indexing. */
int tt_schema_drop_fts_triggers(sqlite3 *db);

/* Recreate FTS5 sync triggers -- call after bulk indexing. */
int tt_schema_create_fts_triggers(sqlite3 *db);

/* Drop all secondary indexes on symbols table -- call before bulk indexing. */
int tt_schema_drop_secondary_indexes(sqlite3 *db);

/* Recreate all secondary indexes -- call after bulk indexing. */
int tt_schema_create_secondary_indexes(sqlite3 *db);

/* Rebuild FTS5 from symbols content table -- call after bulk indexing. */
int tt_schema_rebuild_fts(sqlite3 *db);

#endif /* TT_SCHEMA_H */
