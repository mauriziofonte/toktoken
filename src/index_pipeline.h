/*
 * index_pipeline.h -- Parallel indexing pipeline.
 *
 * Splits file list into K chunks, spawns K ctags worker threads + 1 writer
 * thread. Workers stream ctags output, parse JSON, build symbols, and push
 * batches to an MPSC queue. The writer drains the queue into SQLite.
 */

#ifndef TT_INDEX_PIPELINE_H
#define TT_INDEX_PIPELINE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>

#include "mpsc-queue.h"
#include "symbol.h"
#include "import_extractor.h"
#include "database.h"
#include "index_store.h"

/* Retrieve containing struct from mpsc_queue_node pointer */
#define tt_container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

/* ===== Batch: unit of work passed from workers to writer ===== */

typedef struct tt_symbol_batch
{
    struct mpsc_queue_node node; /* intrusive node — MUST be first field */
    tt_symbol_t *symbols;        /* [owns] array of symbols */
    int symbol_count;
    tt_import_t *imports;        /* [owns] array of imports, NULL if none */
    int import_count;
    char **file_paths;           /* [owns] relative paths in this batch */
    char **file_hashes;          /* [owns] content hashes (parallel to file_paths) */
    char **file_languages;       /* [owns] language strings */
    int64_t *file_sizes;         /* [owns] file sizes in bytes */
    int64_t *file_mtimes_sec;    /* [owns] mtime seconds */
    int64_t *file_mtimes_nsec;   /* [owns] mtime nanoseconds */
    int file_count;
    bool is_sentinel;            /* true = "worker done" signal, no data */
    bool has_error;              /* true = worker encountered an error */
    char *error_msg;             /* [owns] error description, NULL if no error */
} tt_symbol_batch_t;

/* ===== Progress counters (thread-safe) ===== */

typedef struct
{
    _Atomic(int64_t) files_indexed;
    _Atomic(int64_t) symbols_indexed;
    _Atomic(int64_t) bytes_processed;
    _Atomic(int64_t) errors;
    int64_t total_files; /* set before pipeline starts, immutable */
} tt_progress_t;

/* ===== Pipeline configuration ===== */

typedef struct
{
    const char *project_root;
    const char *ctags_path;
    const char **file_paths;     /* relative paths to index */
    const int64_t *file_sizes;   /* file sizes in bytes (parallel to file_paths), NULL ok */
    int file_count;
    int num_workers;             /* K: resolved cpu count / config */
    int ctags_timeout_sec;
    int max_file_size_bytes;
    tt_database_t *db;
    tt_index_store_t *store;
    bool incremental;            /* true = keep FTS triggers active (single-file reindex) */
} tt_pipeline_config_t;

/* ===== Pipeline result ===== */

typedef struct
{
    int files_indexed;
    int symbols_indexed;
    int imports_indexed;
    int errors_count;
    char **errors;               /* [owns] array of error messages */
} tt_pipeline_result_t;

/* ===== Opaque handle for async pipeline control ===== */

typedef struct tt_pipeline_handle tt_pipeline_handle_t;

/*
 * tt_pipeline_start -- Start the parallel indexing pipeline (non-blocking).
 *
 * Creates worker + writer threads. progress may be NULL.
 * Returns 0 on success, -1 on error.
 */
int tt_pipeline_start(const tt_pipeline_config_t *config,
                      tt_progress_t *progress,
                      tt_pipeline_handle_t **handle);

/*
 * tt_pipeline_finished -- Check if pipeline has finished.
 */
bool tt_pipeline_finished(tt_pipeline_handle_t *handle);

/*
 * tt_pipeline_join -- Block until pipeline finishes and collect results.
 *
 * Joins all threads, populates result, frees handle.
 * Does NOT rebuild indexes/FTS -- call tt_pipeline_finalize_schema() after
 * any post-pipeline work (summaries) that benefits from bare tables.
 */
int tt_pipeline_join(tt_pipeline_handle_t *handle,
                     tt_pipeline_result_t *result);

/*
 * tt_pipeline_finalize_schema -- Rebuild secondary indexes, FTS, and triggers.
 *
 * Call this after tt_pipeline_join() and any summary generation.
 */
void tt_pipeline_finalize_schema(tt_database_t *db);

/*
 * tt_pipeline_run -- Convenience: blocking run (start + join).
 */
int tt_pipeline_run(const tt_pipeline_config_t *config,
                    tt_pipeline_result_t *result);

/*
 * tt_pipeline_result_free -- Free result error messages.
 */
void tt_pipeline_result_free(tt_pipeline_result_t *result);

#endif /* TT_INDEX_PIPELINE_H */
