/*
 * index_store.h -- CRUD operations on the TokToken SQLite database.
 *
 * Provides prepared-statement-based access for files and symbols,
 * FTS5 search, change detection, and statistics.
 */

#ifndef TT_INDEX_STORE_H
#define TT_INDEX_STORE_H

#include "database.h"
#include "hashmap.h"
#include "symbol.h"

#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>

/* ---- Symbol result (symbol + search score) ---- */

typedef struct
{
    tt_symbol_t sym;
    double score;
    /* Debug score breakdown (populated when debug mode is active) */
    int dbg_name;
    int dbg_sig;
    int dbg_summary;
    int dbg_keyword;
    int dbg_docstring;
} tt_symbol_result_t;

void tt_symbol_result_free(tt_symbol_result_t *r);

/* ---- File record ---- */

typedef struct
{
    char *path;     /* [owns] */
    char *hash;     /* [owns] */
    char *language; /* [owns] */
    char *summary;  /* [owns] */
    int64_t size_bytes;
    char *indexed_at; /* [owns] */
} tt_file_record_t;

void tt_file_record_free(tt_file_record_t *r);

/* ---- Search results ---- */

typedef struct
{
    tt_symbol_result_t *results; /* [owns] */
    int count;
} tt_search_results_t;

void tt_search_results_free(tt_search_results_t *r);

/* ---- Name-count pair (used in stats) ---- */

typedef struct
{
    char *name;
    int count;
} tt_name_count_t;

/* ---- Statistics ---- */

typedef struct
{
    int files;
    int symbols;
    tt_name_count_t *languages; /* [owns] */
    int language_count;
    tt_name_count_t *kinds; /* [owns] */
    int kind_count;
    tt_name_count_t *dirs; /* [owns] */
    int dir_count;
} tt_stats_t;

void tt_stats_free(tt_stats_t *s);

/* ---- Index store (prepared statements) ---- */

typedef struct
{
    tt_database_t *db; /* [borrows] */
    sqlite3_stmt *insert_file;
    sqlite3_stmt *insert_sym;
    sqlite3_stmt *delete_file;
    sqlite3_stmt *delete_syms;
    sqlite3_stmt *get_file;
    sqlite3_stmt *get_sym;
    sqlite3_stmt *get_syms_file;
    sqlite3_stmt *set_meta;
    sqlite3_stmt *get_meta;
    sqlite3_stmt *update_sym_summary;
    sqlite3_stmt *update_file_summary;
} tt_index_store_t;

/*
 * tt_store_init -- Prepare all statements.
 *
 * The store borrows the database; the caller must keep it alive.
 * Returns 0 on success, -1 on error.
 */
int tt_store_init(tt_index_store_t *store, tt_database_t *db);

/*
 * tt_store_close -- Finalize all prepared statements.
 */
void tt_store_close(tt_index_store_t *store);

/* ---- Insert operations ---- */

int tt_store_insert_file(tt_index_store_t *store, const char *path, const char *hash,
                         const char *language, int64_t size_bytes,
                         int64_t mtime_sec, int64_t mtime_nsec,
                         const char *summary);

int tt_store_insert_symbol(tt_index_store_t *store, const tt_symbol_t *sym);

int tt_store_insert_symbols(tt_index_store_t *store, const tt_symbol_t *syms, int count);

/* ---- Query operations ---- */

int tt_store_search_symbols(tt_index_store_t *store, const char *query,
                            const char **kinds, int kind_count,
                            const char *language, const char *file_pattern,
                            int max_results, bool regex,
                            tt_search_results_t *out);

int tt_store_get_symbol(tt_index_store_t *store, const char *id, tt_symbol_result_t *out);

int tt_store_get_symbols_by_ids(tt_index_store_t *store, const char **ids, int count,
                                tt_symbol_result_t **out, int *out_count,
                                char ***errors, int *error_count);

int tt_store_get_symbols_by_file(tt_index_store_t *store, const char *file_path,
                                 tt_symbol_result_t **out, int *out_count);

int tt_store_get_file(tt_index_store_t *store, const char *path, tt_file_record_t *out);

int tt_store_get_file_hashes(tt_index_store_t *store, tt_hashmap_t **out);

int tt_store_get_all_files(tt_index_store_t *store, tt_file_record_t **out, int *count);

int tt_store_get_stats(tt_index_store_t *store, tt_stats_t *out);

/* ---- Delete and update operations ---- */

int tt_store_delete_symbols_by_file(tt_index_store_t *store, const char *file_path);
int tt_store_delete_file(tt_index_store_t *store, const char *file_path);
int tt_store_truncate(tt_index_store_t *store);

int tt_store_set_metadata(tt_index_store_t *store, const char *key, const char *value);

/*
 * tt_store_get_metadata -- Get a metadata value by key.
 *
 * [caller-frees] Returns NULL if not found.
 */
char *tt_store_get_metadata(tt_index_store_t *store, const char *key);

int tt_store_update_symbol_summary(tt_index_store_t *store, const char *id,
                                   const char *summary);
int tt_store_update_file_summary(tt_index_store_t *store, const char *path,
                                 const char *summary);

int tt_store_get_symbols_without_summary(tt_index_store_t *store, int limit,
                                         tt_symbol_result_t **out, int *count);

/* ---- Lightweight summary input (for docstring-only fetch) ---- */

typedef struct
{
    char *id;        /* [owns] symbol id */
    char *docstring; /* [owns] */
    char *kind;      /* [owns] canonical kind string */
    char *name;      /* [owns] */
} tt_summary_input_t;

int tt_store_get_docstring_symbols(tt_index_store_t *store,
                                   tt_summary_input_t **out, int *count);
void tt_summary_input_free(tt_summary_input_t *items, int count);

/* ---- Fast change detection (stat-first, hash-on-change) ---- */

typedef struct
{
    char **changed;
    int changed_count;
    char **added;
    int added_count;
    char **deleted;
    int deleted_count;
    char **metadata_changed;
    int metadata_changed_count;
} tt_changes_fast_t;

void tt_changes_fast_free(tt_changes_fast_t *c);

int tt_store_detect_changes_fast(tt_index_store_t *store,
                                  const char *project_root,
                                  const char **discovered_paths,
                                  int path_count,
                                  tt_changes_fast_t *out);

int tt_store_update_file_metadata(tt_index_store_t *store,
                                   const char *path,
                                   int64_t mtime_sec, int64_t mtime_nsec,
                                   int64_t size_bytes);

/* ---- Import graph operations ---- */

#include "import_extractor.h"

int tt_store_insert_imports(tt_index_store_t *store, const tt_import_t *imps, int count);
int tt_store_delete_imports_for_file(tt_index_store_t *store, const char *file_path);

/*
 * tt_store_resolve_imports -- Resolve import specifiers to file paths.
 *
 * For each import record with resolved_file = '', attempts to match the
 * target_name (language-specific specifier) to a file path in the files table.
 * Uses separator normalization, extension probing, relative path resolution,
 * and case-insensitive suffix matching. Works across all languages.
 *
 * Call after all files and imports have been inserted (post-pipeline).
 * Returns 0 on success, -1 on error.
 */
int tt_store_resolve_imports(tt_index_store_t *store);

int tt_store_get_importers(tt_index_store_t *store, const char *file_path,
                           tt_import_t **out, int *out_count);
int tt_store_count_importers(tt_index_store_t *store, const char *file_path);
int tt_store_find_references(tt_index_store_t *store, const char *identifier,
                             tt_import_t **out, int *out_count);

/*
 * tt_store_get_imports_of -- Get files imported by a given file (forward direction).
 *
 * Returns target_name values from imports where source_file matches.
 * out_files: [caller-frees] array of strings.
 */
int tt_store_get_imports_of(tt_index_store_t *store, const char *file_path,
                             char ***out_files, int *out_count);

/* ---- Hierarchy nodes (inspect:hierarchy) ---- */

typedef struct
{
    char *id;             /* [owns] symbol id */
    char *file;           /* [owns] */
    char *name;           /* [owns] */
    char *qualified_name; /* [owns] */
    char *kind;           /* [owns] */
    char *parent_id;      /* [owns] nullable */
    int line;
    int end_line;
} tt_hierarchy_node_t;

void tt_hierarchy_node_free(tt_hierarchy_node_t *items, int count);

int tt_store_get_hierarchy(tt_index_store_t *store,
                            const char *file_path, const char *language,
                            int limit,
                            tt_hierarchy_node_t **out, int *out_count);

/* ---- Co-occurrence results (search:cooccurrence) ---- */

typedef struct
{
    char *file;    /* [owns] */
    char *name_a;  /* [owns] */
    char *kind_a;  /* [owns] */
    int line_a;
    char *name_b;  /* [owns] */
    char *kind_b;  /* [owns] */
    int line_b;
} tt_cooccurrence_t;

void tt_cooccurrence_free(tt_cooccurrence_t *items, int count);

int tt_store_search_cooccurrence(tt_index_store_t *store,
                                  const char *name_a, const char *name_b,
                                  const char *language, int limit,
                                  tt_cooccurrence_t **out, int *out_count);

/* ---- Dependency graph (inspect:dependencies) ---- */

typedef struct
{
    char *file; /* [owns] */
    int depth;
} tt_dependency_t;

void tt_dependency_free(tt_dependency_t *items, int count);

int tt_store_get_dependencies(tt_index_store_t *store,
                               const char *file_path, int max_depth,
                               tt_dependency_t **out, int *out_count);

/* ---- Caller results (find:callers) ---- */

typedef struct
{
    char *id;        /* [owns] */
    char *file;      /* [owns] */
    char *name;      /* [owns] */
    char *kind;      /* [owns] */
    int line;
    char *signature; /* [owns] */
} tt_caller_t;

void tt_caller_free(tt_caller_t *items, int count);

int tt_store_find_callers(tt_index_store_t *store,
                           const char *symbol_id, int limit,
                           tt_caller_t **out, int *out_count);

/* ---- Similar symbols (search:similar) ---- */

int tt_store_search_similar(tt_index_store_t *store,
                             const char *symbol_id, int limit,
                             tt_search_results_t *out);

/* ---- File summary generation ---- */

int tt_store_generate_file_summaries(tt_index_store_t *store);

/* ---- Centrality ---- */

/*
 * Compute file centrality scores from the import graph.
 * Stores log(1 + importer_count) into file_centrality table.
 * Call after indexing completes.
 */
int tt_store_compute_centrality(tt_index_store_t *store);

/*
 * Load file centrality scores into a hashmap.
 * Keys are file paths, values are (void *)(intptr_t)(score * 1000).
 * [caller-frees hashmap with tt_hashmap_free]
 */
tt_hashmap_t *tt_store_load_centrality(tt_index_store_t *store);

#endif /* TT_INDEX_STORE_H */
