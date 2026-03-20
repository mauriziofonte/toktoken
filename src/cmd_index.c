/*
 * cmd_index.c -- index:create and index:update commands.
 */

#include "cmd_index.h"
#include "json_output.h"
#include "error.h"
#include "platform.h"
#include "database.h"
#include "schema.h"
#include "index_store.h"
#include "file_filter.h"
#include "index_pipeline.h"
#include "ctags_resolver.h"
#include "language_detector.h"
#include "summarizer.h"
#include "git_head.h"
#include "version.h"
#include "str_util.h"
#include "config.h"
#include "thread_pool.h"
#include "diagnostic.h"
#include "fast_hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Signal flag */
volatile sig_atomic_t tt_interrupted = 0;

/* ---- Defaults ---- */

#define DEFAULT_MAX_FILES 10000
#define DEFAULT_MAX_FILE_SIZE_KB 512
#define DEFAULT_CTAGS_TIMEOUT 120

/* ---- Helpers ---- */

static cJSON *make_error(const char *code, const char *message, const char *hint)
{
    cJSON *json = cJSON_CreateObject();
    if (!json)
        return NULL;
    cJSON_AddStringToObject(json, "error", code);
    cJSON_AddStringToObject(json, "message", message);
    if (hint && hint[0])
        cJSON_AddStringToObject(json, "hint", hint);
    return json;
}

/* Set all standard metadata after indexing */
static void set_metadata(tt_index_store_t *store, const char *project_path)
{
    const char *ts = tt_now_rfc3339();
    tt_store_set_metadata(store, "indexed_at", ts);
    tt_store_set_metadata(store, "project_path", project_path);
    tt_store_set_metadata(store, "toktoken_version", TT_VERSION);

    char ver_str[8];
    snprintf(ver_str, sizeof(ver_str), "%d", TT_SCHEMA_VERSION);
    tt_store_set_metadata(store, "schema_version", ver_str);

    char *head = tt_git_head(project_path);
    tt_store_set_metadata(store, "git_head", head ? head : "");
    free(head);
}

/*
 * Build a NULL-terminated extra_ignore array merging config + CLI patterns.
 * CLI patterns are appended after config patterns (both contribute).
 * [caller-frees] the returned array (not the strings, which are borrowed).
 */
static const char **build_extra_ignore(tt_cli_opts_t *opts, const tt_config_t *config)
{
    int total = (config ? config->extra_ignore_count : 0) + opts->ignore_count;
    if (total <= 0)
        return NULL;

    const char **arr = calloc((size_t)(total + 1), sizeof(char *));
    if (!arr)
        return NULL;

    int idx = 0;
    if (config)
    {
        for (int i = 0; i < config->extra_ignore_count; i++)
            arr[idx++] = config->extra_ignore_patterns[i];
    }
    for (int i = 0; i < opts->ignore_count; i++)
        arr[idx++] = opts->ignore_patterns[i];
    arr[idx] = NULL;
    return arr;
}

/* ---- index:create ---- */

cJSON *tt_cmd_index_create_exec(tt_cli_opts_t *opts)
{
    tt_timer_start();

    /* Enable diagnostic mode if requested */
    if (opts->diagnostic)
    {
        tt_diag_init();
        tt_diag_enable();
    }

    /* Resolve project path: --path flag takes priority, then positional[0] */
    const char *raw_path = opts->path;
    if (!raw_path && opts->positional_count > 0)
        raw_path = opts->positional[0];
    char *project_path = tt_resolve_project_path(raw_path);
    if (!project_path)
    {
        tt_error_set("Failed to resolve project path");
        return NULL;
    }

    /* Verify directory */
    if (!tt_is_dir(project_path))
    {
        cJSON *err = make_error("invalid_path", "Directory not found", NULL);
        free(project_path);
        return err;
    }

    /* Load configuration */
    tt_config_t config;
    tt_config_load(&config, project_path);

    /* Apply extra extensions from config/env */
    if (config.extra_ext_count > 0)
    {
        tt_lang_set_extra_extensions((const char **)config.extra_ext_keys,
                                     (const char **)config.extra_ext_languages,
                                     config.extra_ext_count);
    }

    /* Resolve ctags binary */
    char *ctags_path = tt_ctags_resolve();
    if (!ctags_path)
    {
        cJSON *err = make_error("ctags_failed",
                                "Universal Ctags not found",
                                "Ensure universal-ctags is available.");
        tt_lang_clear_extra_extensions();
        tt_config_free(&config);
        free(project_path);
        return err;
    }

    /* Build extra ignore patterns (config + CLI merged) */
    const char **extra_ignore = build_extra_ignore(opts, &config);

    /* Emit sysinfo after config is loaded but before work begins */
    {
        int diag_K = config.workers > 0 ? config.workers : tt_cpu_count();
        TT_DIAG("init", "sysinfo",
                "\"cpus\":%d,\"workers\":%d,\"max_files\":%d,"
                "\"max_file_size_kb\":%d,\"smart_filter\":%s,"
                "\"batch_sz\":%d,\"bp_thresh\":%d",
                tt_cpu_count(), diag_K,
                opts->max_files > 0 ? opts->max_files : config.max_files,
                config.max_file_size_kb,
                (config.smart_filter && !opts->full) ? "true" : "false",
                1024, 100000);
        TT_DIAG_MEM();
    }

    /* Phase 1: Discover file paths (lightweight, no hashing) */
    TT_DIAG("discovery", "start", "\"path\":\"%s\"", project_path);
    uint64_t t_discovery_start = tt_monotonic_ms();

    tt_file_filter_t ff;
    if (tt_file_filter_init(&ff, config.max_file_size_kb, extra_ignore,
                            config.smart_filter && !opts->full) < 0)
    {
        free(extra_ignore);
        free(ctags_path);
        tt_config_free(&config);
        free(project_path);
        tt_error_set("Failed to initialize file filter");
        return NULL;
    }

    tt_discovered_paths_t discovered;
    memset(&discovered, 0, sizeof(discovered));
    if (tt_discover_paths(project_path, &ff, &discovered) < 0)
    {
        tt_file_filter_free(&ff);
        free(extra_ignore);
        free(ctags_path);
        tt_config_free(&config);
        free(project_path);
        tt_error_set("File discovery failed");
        return NULL;
    }
    tt_file_filter_free(&ff);
    free(extra_ignore);

    /* Apply max_files limit (CLI --max-files overrides config) */
    int max_files = opts->max_files > 0 ? opts->max_files : config.max_files;
    int skipped = 0;
    if (discovered.count > max_files)
    {
        skipped = discovered.count - max_files;
        for (int i = max_files; i < discovered.count; i++)
            free(discovered.paths[i]);
        discovered.count = max_files;
    }

    /* No files? */
    if (discovered.count == 0)
    {
        tt_discovered_paths_free(&discovered);
        free(ctags_path);
        tt_config_free(&config);
        cJSON *err = make_error("no_files",
                                "No source files found in the project",
                                "Check that the directory contains source files and is not entirely excluded.");
        free(project_path);
        return err;
    }

    uint64_t t_discovery_done = tt_monotonic_ms();
    TT_DIAG("discovery", "done",
            "\"files\":%d,\"skipped\":%d,\"dur_ms\":%llu",
            discovered.count, skipped,
            (unsigned long long)(t_discovery_done - t_discovery_start));

    /* Open database */
    tt_database_t db;
    memset(&db, 0, sizeof(db));
    if (tt_database_open(&db, project_path) < 0)
    {
        cJSON *err = make_error("storage_error",
                                "Failed to open database", NULL);
        tt_discovered_paths_free(&discovered);
        free(ctags_path);
        tt_config_free(&config);
        free(project_path);
        return err;
    }

    /* Init store */
    tt_index_store_t store;
    if (tt_store_init(&store, &db) < 0)
    {
        cJSON *err = make_error("storage_error",
                                "Failed to prepare statements", NULL);
        tt_database_close(&db);
        tt_discovered_paths_free(&discovered);
        free(ctags_path);
        tt_config_free(&config);
        free(project_path);
        return err;
    }

    /* Truncate existing data */
    tt_store_truncate(&store);

    /* Bulk indexing PRAGMAs: larger cache + in-memory temp store */
    sqlite3_exec(db.db, "PRAGMA cache_size=-32000", NULL, NULL, NULL);
    sqlite3_exec(db.db, "PRAGMA temp_store=MEMORY", NULL, NULL, NULL);

    /* Phase 2+3: Parallel indexing pipeline with progress polling */
    int K = config.workers > 0 ? config.workers : tt_cpu_count();
    int timeout = config.ctags_timeout_seconds;

    tt_pipeline_config_t pcfg = {0};
    pcfg.project_root = project_path;
    pcfg.ctags_path = ctags_path;
    pcfg.file_paths = (const char **)discovered.paths;
    pcfg.file_sizes = discovered.sizes;
    pcfg.file_count = discovered.count;
    pcfg.num_workers = K;
    pcfg.ctags_timeout_sec = timeout;
    pcfg.max_file_size_bytes = config.max_file_size_kb * 1024;
    pcfg.db = &db;
    pcfg.store = &store;

    tt_progress_t progress = {0};
    progress.total_files = discovered.count;

    TT_DIAG("pipeline", "start",
            "\"workers\":%d,\"files\":%d", K, discovered.count);

    tt_pipeline_handle_t *handle = NULL;
    if (tt_pipeline_start(&pcfg, &progress, &handle) < 0)
    {
        cJSON *err = make_error("pipeline_error",
                                "Failed to start indexing pipeline", NULL);
        tt_store_close(&store);
        tt_database_close(&db);
        tt_discovered_paths_free(&discovered);
        free(ctags_path);
        tt_config_free(&config);
        free(project_path);
        return err;
    }

    /* Progress polling */
    int diag_poll_count = 0;
    while (!tt_pipeline_finished(handle))
    {
        int64_t done = atomic_load(&progress.files_indexed);
        if (opts->progress_cb)
            opts->progress_cb(opts->progress_ctx, done, (int64_t)discovered.count);
        else
            tt_progress("Indexing... %d/%d files\n", (int)done, discovered.count);

        /* Memory snapshot every ~5s (every 10th poll at 500ms interval) */
        if (++diag_poll_count % 10 == 0)
            TT_DIAG_MEM();

        usleep(500000);
    }

    tt_pipeline_result_t presult;
    if (tt_pipeline_join(handle, &presult) < 0)
    {
        cJSON *err = make_error("pipeline_error",
                                "Pipeline join failed", NULL);
        tt_store_close(&store);
        tt_database_close(&db);
        tt_discovered_paths_free(&discovered);
        free(ctags_path);
        tt_config_free(&config);
        free(project_path);
        return err;
    }

    free(ctags_path);
    TT_DIAG_MEM(); /* Capture post-pipeline RSS before memory is freed */
    uint64_t t_pipeline_done = tt_monotonic_ms();
    TT_DIAG("pipeline", "done",
            "\"files\":%d,\"syms\":%d,\"errors\":%d,\"dur_ms\":%llu",
            presult.files_indexed, presult.symbols_indexed,
            presult.errors_count,
            (unsigned long long)(t_pipeline_done - t_discovery_done));

    /* Phase 4: Summaries run BEFORE index/FTS rebuild (bare tables = fast) */
    uint64_t t_summaries_start = tt_monotonic_ms();
    TT_DIAG("summary", "start", NULL);
    tt_apply_sync_summaries(&db);
    uint64_t t_summaries_done = tt_monotonic_ms();

    tt_store_generate_file_summaries(&store);
    uint64_t t_file_summaries_done = tt_monotonic_ms();
    TT_DIAG("summary", "done",
            "\"sym_ms\":%llu,\"file_ms\":%llu",
            (unsigned long long)(t_summaries_done - t_summaries_start),
            (unsigned long long)(t_file_summaries_done - t_summaries_done));

    /* Rebuild secondary indexes, FTS, and triggers */
    {
        uint64_t t0 = tt_monotonic_ms();
        TT_DIAG("schema", "start", NULL);
        tt_schema_create_secondary_indexes(db.db);
        uint64_t t1 = tt_monotonic_ms();
        TT_DIAG("schema", "secondary_idx",
                "\"dur_ms\":%llu", (unsigned long long)(t1 - t0));

        tt_schema_rebuild_fts(db.db);
        uint64_t t2 = tt_monotonic_ms();
        TT_DIAG("schema", "fts_rebuild",
                "\"dur_ms\":%llu", (unsigned long long)(t2 - t1));

        tt_schema_create_fts_triggers(db.db);
        uint64_t t3 = tt_monotonic_ms();
        TT_DIAG("schema", "fts_triggers",
                "\"dur_ms\":%llu", (unsigned long long)(t3 - t2));

        tt_store_resolve_imports(&store);
        uint64_t t4 = tt_monotonic_ms();
        TT_DIAG("schema", "resolve_imports",
                "\"dur_ms\":%llu", (unsigned long long)(t4 - t3));

        tt_store_compute_centrality(&store);
        uint64_t t5 = tt_monotonic_ms();
        TT_DIAG("schema", "centrality",
                "\"dur_ms\":%llu", (unsigned long long)(t5 - t4));

        TT_DIAG("schema", "done",
                "\"dur_ms\":%llu", (unsigned long long)(t5 - t0));
    }

    set_metadata(&store, project_path);

    /* WAL checkpoint */
    sqlite3_exec(db.db, "PRAGMA wal_checkpoint(TRUNCATE)", NULL, NULL, NULL);

    /* Get stats for output */
    tt_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    tt_store_get_stats(&store, &stats);

    /* Build result JSON */
    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "files", stats.files);
    cJSON_AddNumberToObject(result, "symbols", stats.symbols);

    cJSON *langs = cJSON_CreateObject();
    for (int i = 0; i < stats.language_count; i++)
    {
        cJSON_AddNumberToObject(langs, stats.languages[i].name,
                                stats.languages[i].count);
    }
    cJSON_AddItemToObject(result, "languages", langs);

    cJSON_AddNumberToObject(result, "duration_seconds", tt_timer_elapsed_sec());

    if (skipped > 0)
    {
        cJSON_AddNumberToObject(result, "skipped", skipped);
    }

    if (presult.errors_count > 0)
    {
        cJSON *errors = cJSON_CreateArray();
        for (int i = 0; i < presult.errors_count; i++)
            cJSON_AddItemToArray(errors, cJSON_CreateString(presult.errors[i]));
        cJSON_AddItemToObject(result, "warnings", errors);
    }

    /* Timing breakdown (milliseconds) */
    {
        uint64_t t_start = t_discovery_start;
        cJSON *timing = cJSON_CreateObject();
        cJSON_AddNumberToObject(timing, "discovery_ms",
                                (double)(t_discovery_done - t_start));
        cJSON_AddNumberToObject(timing, "pipeline_ms",
                                (double)(t_pipeline_done - t_discovery_done));
        cJSON_AddNumberToObject(timing, "summaries_ms",
                                (double)(t_summaries_done - t_pipeline_done));
        cJSON_AddNumberToObject(timing, "file_summaries_ms",
                                (double)(t_file_summaries_done - t_summaries_done));
        cJSON_AddItemToObject(result, "timing", timing);
    }

    /* Capture RSS after schema rebuild, before reporting peak */
    TT_DIAG_MEM();

    /* Final diagnostic summary */
    TT_DIAG("done", "summary",
            "\"dur_s\":%.2f,\"peak_rss_kb\":%llu,"
            "\"files\":%d,\"syms\":%d,\"errors\":%d",
            tt_timer_elapsed_sec(),
            (unsigned long long)tt_diag_peak_rss_kb(),
            stats.files, (int)stats.symbols, presult.errors_count);

    /* Cleanup */
    tt_pipeline_result_free(&presult);
    tt_stats_free(&stats);
    tt_store_close(&store);
    tt_database_close(&db);
    tt_discovered_paths_free(&discovered);
    tt_config_free(&config);
    free(project_path);

    return result;
}

/* ---- index:update ---- */

cJSON *tt_cmd_index_update_exec(tt_cli_opts_t *opts)
{
    tt_timer_start();

    /* Enable diagnostic mode if requested */
    if (opts->diagnostic)
    {
        tt_diag_init();
        tt_diag_enable();
    }

    /* Resolve project path: --path flag takes priority, then positional[0] */
    const char *raw_path = opts->path;
    if (!raw_path && opts->positional_count > 0)
        raw_path = opts->positional[0];
    char *project_path = tt_resolve_project_path(raw_path);
    if (!project_path)
    {
        tt_error_set("Failed to resolve project path");
        return NULL;
    }

    /* Check DB exists */
    if (!tt_database_exists(project_path))
    {
        cJSON *err = make_error("no_index",
                                "No index found for this project.",
                                "Run \"toktoken index:create\" first.");
        free(project_path);
        return err;
    }

    /* Open database */
    tt_database_t db;
    memset(&db, 0, sizeof(db));
    if (tt_database_open(&db, project_path) < 0)
    {
        cJSON *err = make_error("storage_error",
                                "Failed to open database", NULL);
        free(project_path);
        return err;
    }

    /* Check schema compatibility */
    int schema_ver = tt_schema_check_version(db.db);
    if (schema_ver != TT_SCHEMA_VERSION)
    {
        tt_index_store_t tmp_store;
        if (tt_store_init(&tmp_store, &db) == 0)
        {
            char *stored_ver = tt_store_get_metadata(&tmp_store, "toktoken_version");
            tt_store_close(&tmp_store);

            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Index was created by toktoken %s (schema v%d), "
                     "current is %s (schema v%d).",
                     stored_ver ? stored_ver : "unknown", schema_ver,
                     TT_VERSION, TT_SCHEMA_VERSION);
            free(stored_ver);

            tt_database_close(&db);
            cJSON *err = make_error("schema_mismatch", msg,
                                    "Run \"toktoken cache:clear\" then \"toktoken index:create\" to rebuild.");
            free(project_path);
            return err;
        }
        tt_database_close(&db);
        cJSON *err = make_error("schema_mismatch",
                                "Schema version mismatch",
                                "Run \"toktoken cache:clear\" then \"toktoken index:create\" to rebuild.");
        free(project_path);
        return err;
    }

    /* Load configuration */
    tt_config_t config;
    tt_config_load(&config, project_path);

    /* Apply extra extensions from config/env */
    if (config.extra_ext_count > 0)
    {
        tt_lang_set_extra_extensions((const char **)config.extra_ext_keys,
                                     (const char **)config.extra_ext_languages,
                                     config.extra_ext_count);
    }

    /* Resolve ctags binary */
    char *ctags_path = tt_ctags_resolve();
    if (!ctags_path)
    {
        tt_lang_clear_extra_extensions();
        tt_config_free(&config);
        tt_database_close(&db);
        cJSON *err = make_error("ctags_failed",
                                "Universal Ctags not found",
                                "Ensure universal-ctags is available.");
        free(project_path);
        return err;
    }

    /* Build extra ignore patterns (config + CLI merged) */
    const char **extra_ignore = build_extra_ignore(opts, &config);

    /* Emit sysinfo for update path */
    {
        int diag_K = config.workers > 0 ? config.workers : tt_cpu_count();
        TT_DIAG("init", "sysinfo",
                "\"cpus\":%d,\"workers\":%d,\"max_files\":%d,"
                "\"max_file_size_kb\":%d,\"smart_filter\":%s",
                tt_cpu_count(), diag_K,
                opts->max_files > 0 ? opts->max_files : config.max_files,
                config.max_file_size_kb,
                (config.smart_filter && !opts->full) ? "true" : "false");
        TT_DIAG_MEM();
    }

    /* Phase 1: Discover file paths (lightweight) */
    TT_DIAG("discovery", "start", "\"path\":\"%s\"", project_path);
    uint64_t t_upd_discovery_start = tt_monotonic_ms();

    tt_file_filter_t ff;
    if (tt_file_filter_init(&ff, config.max_file_size_kb, extra_ignore,
                            config.smart_filter && !opts->full) < 0)
    {
        free(extra_ignore);
        free(ctags_path);
        tt_lang_clear_extra_extensions();
        tt_config_free(&config);
        tt_database_close(&db);
        free(project_path);
        tt_error_set("Failed to initialize file filter");
        return NULL;
    }

    tt_discovered_paths_t discovered;
    memset(&discovered, 0, sizeof(discovered));
    if (tt_discover_paths(project_path, &ff, &discovered) < 0)
    {
        tt_file_filter_free(&ff);
        free(extra_ignore);
        free(ctags_path);
        tt_config_free(&config);
        tt_database_close(&db);
        free(project_path);
        tt_error_set("File discovery failed");
        return NULL;
    }
    tt_file_filter_free(&ff);
    free(extra_ignore);

    /* Apply max_files limit */
    int max_files = opts->max_files > 0 ? opts->max_files : config.max_files;
    if (discovered.count > max_files)
    {
        for (int i = max_files; i < discovered.count; i++)
            free(discovered.paths[i]);
        discovered.count = max_files;
    }

    uint64_t t_upd_discovery_done = tt_monotonic_ms();
    TT_DIAG("discovery", "done",
            "\"files\":%d,\"dur_ms\":%llu",
            discovered.count,
            (unsigned long long)(t_upd_discovery_done - t_upd_discovery_start));

    /* Init store */
    tt_index_store_t store;
    if (tt_store_init(&store, &db) < 0)
    {
        tt_discovered_paths_free(&discovered);
        free(ctags_path);
        tt_config_free(&config);
        tt_database_close(&db);
        free(project_path);
        tt_error_set("Failed to prepare statements");
        return NULL;
    }

    /* Phase 1b: Fast change detection (stat-based, no content hashing) */
    TT_DIAG("changes", "start", NULL);
    uint64_t t_changes_start = tt_monotonic_ms();

    tt_changes_fast_t changes;
    memset(&changes, 0, sizeof(changes));
    if (tt_store_detect_changes_fast(&store, project_path,
                                     (const char **)discovered.paths,
                                     discovered.count, &changes) < 0)
    {
        tt_store_close(&store);
        tt_discovered_paths_free(&discovered);
        free(ctags_path);
        tt_config_free(&config);
        tt_database_close(&db);
        free(project_path);
        tt_error_set("Failed to detect changes");
        return NULL;
    }

    int files_to_reindex_count = changes.changed_count + changes.added_count;
    TT_DIAG("changes", "done",
            "\"changed\":%d,\"added\":%d,\"deleted\":%d,"
            "\"metadata\":%d,\"dur_ms\":%llu",
            changes.changed_count, changes.added_count,
            changes.deleted_count, changes.metadata_changed_count,
            (unsigned long long)(tt_monotonic_ms() - t_changes_start));

    /* If nothing changed, update timestamps and return immediately */
    if (files_to_reindex_count == 0 && changes.deleted_count == 0)
    {
        /* Update metadata-only changes (mtime drift without content change) */
        for (int i = 0; i < changes.metadata_changed_count; i++)
        {
            char *full = tt_path_join(project_path, changes.metadata_changed[i]);
            if (full)
            {
                struct stat st;
                if (stat(full, &st) == 0)
                {
                    tt_store_update_file_metadata(&store,
                                                  changes.metadata_changed[i],
                                                  TT_ST_MTIME_SEC(st),
                                                  TT_ST_MTIME_NSEC(st),
                                                  (int64_t)st.st_size);
                }
                free(full);
            }
        }

        set_metadata(&store, project_path);

        cJSON *result = cJSON_CreateObject();
        cJSON_AddNumberToObject(result, "changed", 0);
        cJSON_AddNumberToObject(result, "added", 0);
        cJSON_AddNumberToObject(result, "deleted", 0);
        cJSON_AddNumberToObject(result, "duration_seconds", tt_timer_elapsed_sec());

        tt_changes_fast_free(&changes);
        tt_store_close(&store);
        tt_discovered_paths_free(&discovered);
        free(ctags_path);
        tt_config_free(&config);
        tt_database_close(&db);
        free(project_path);
        return result;
    }

    /* Delete removed files */
    if (tt_database_begin(&db) < 0)
    {
        cJSON *err = make_error("storage_error",
                                "Failed to begin transaction", NULL);
        tt_changes_fast_free(&changes);
        tt_store_close(&store);
        tt_discovered_paths_free(&discovered);
        free(ctags_path);
        tt_config_free(&config);
        tt_database_close(&db);
        free(project_path);
        return err;
    }

    for (int i = 0; i < changes.deleted_count; i++)
    {
        if (tt_interrupted)
            goto interrupted;
        tt_store_delete_symbols_by_file(&store, changes.deleted[i]);
        tt_store_delete_file(&store, changes.deleted[i]);
        tt_store_delete_imports_for_file(&store, changes.deleted[i]);
    }

    /* Delete changed files (will be re-indexed by pipeline) */
    for (int i = 0; i < changes.changed_count; i++)
    {
        if (tt_interrupted)
            goto interrupted;
        tt_store_delete_symbols_by_file(&store, changes.changed[i]);
        tt_store_delete_file(&store, changes.changed[i]);
        tt_store_delete_imports_for_file(&store, changes.changed[i]);
    }

    /* Update metadata-only changes */
    for (int i = 0; i < changes.metadata_changed_count; i++)
    {
        char *full = tt_path_join(project_path, changes.metadata_changed[i]);
        if (full)
        {
            struct stat st;
            if (stat(full, &st) == 0)
            {
                tt_store_update_file_metadata(&store,
                                              changes.metadata_changed[i],
                                              TT_ST_MTIME_SEC(st),
                                              TT_ST_MTIME_NSEC(st),
                                              (int64_t)st.st_size);
            }
            free(full);
        }
    }

    tt_database_commit(&db);

    /* Build files-to-reindex list (changed + added) */
    const char **files_to_reindex = NULL;
    if (files_to_reindex_count > 0)
    {
        files_to_reindex = calloc((size_t)files_to_reindex_count, sizeof(char *));
        int idx = 0;
        for (int i = 0; i < changes.changed_count; i++)
            files_to_reindex[idx++] = changes.changed[i];
        for (int i = 0; i < changes.added_count; i++)
            files_to_reindex[idx++] = changes.added[i];
    }

    /* Decide incremental vs bulk mode.
     * Incremental: keep FTS triggers active, update in O(changed_symbols).
     * Bulk: drop triggers/indexes, rebuild from scratch in O(total_symbols).
     * Threshold: incremental when affected files < 10% of total indexed. */
    int total_affected = files_to_reindex_count + changes.deleted_count;
    bool use_incremental = (discovered.count > 0 &&
                            total_affected <= discovered.count / 10);
    bool imports_changed = false;

    /* Bulk indexing PRAGMAs: larger cache + in-memory temp store */
    sqlite3_exec(db.db, "PRAGMA cache_size=-32000", NULL, NULL, NULL);
    sqlite3_exec(db.db, "PRAGMA temp_store=MEMORY", NULL, NULL, NULL);

    /* Phase 2+3: Parallel indexing pipeline */
    if (files_to_reindex_count > 0)
    {
        int K = config.workers > 0 ? config.workers : tt_cpu_count();
        int timeout = config.ctags_timeout_seconds;

        tt_pipeline_config_t pcfg = {0};
        pcfg.project_root = project_path;
        pcfg.ctags_path = ctags_path;
        pcfg.file_paths = files_to_reindex;
        pcfg.file_count = files_to_reindex_count;
        pcfg.num_workers = K;
        pcfg.ctags_timeout_sec = timeout;
        pcfg.max_file_size_bytes = config.max_file_size_kb * 1024;
        pcfg.db = &db;
        pcfg.store = &store;
        pcfg.incremental = use_incremental;

        tt_progress("Updating %d changed/new files...\n", files_to_reindex_count);

        TT_DIAG("pipeline", "start",
                "\"workers\":%d,\"files\":%d", K, files_to_reindex_count);

        tt_progress_t progress = {0};
        progress.total_files = files_to_reindex_count;

        tt_pipeline_handle_t *handle = NULL;
        if (tt_pipeline_start(&pcfg, &progress, &handle) < 0)
        {
            cJSON *err = make_error("pipeline_error",
                                    "Failed to start indexing pipeline", NULL);
            free(files_to_reindex);
            tt_changes_fast_free(&changes);
            tt_store_close(&store);
            tt_discovered_paths_free(&discovered);
            free(ctags_path);
            tt_config_free(&config);
            tt_database_close(&db);
            free(project_path);
            return err;
        }

        /* Progress polling */
        int diag_upd_poll = 0;
        while (!tt_pipeline_finished(handle))
        {
            int64_t done = atomic_load(&progress.files_indexed);
            if (opts->progress_cb)
                opts->progress_cb(opts->progress_ctx, done, (int64_t)files_to_reindex_count);
            else
                tt_progress("Updating... %d/%d files\n",
                            (int)done, files_to_reindex_count);

            if (++diag_upd_poll % 10 == 0)
                TT_DIAG_MEM();

            usleep(500000);
        }

        tt_pipeline_result_t presult;
        tt_pipeline_join(handle, &presult);
        TT_DIAG_MEM(); /* Capture post-pipeline RSS */

        TT_DIAG("pipeline", "done",
                "\"files\":%d,\"syms\":%d,\"errors\":%d,\"incremental\":%s",
                presult.files_indexed, presult.symbols_indexed,
                presult.errors_count, use_incremental ? "true" : "false");

        imports_changed = (presult.imports_indexed > 0 ||
                           changes.deleted_count > 0);
        tt_pipeline_result_free(&presult);
    }
    else
    {
        /* No files to reindex, but deletions may have removed imports */
        imports_changed = (changes.deleted_count > 0);
    }
    free(files_to_reindex);
    free(ctags_path);

    /* Phase 4: Summaries — only for newly inserted symbols (summary = '') */
    TT_DIAG("summary", "start", NULL);
    uint64_t t_upd_sum_start = tt_monotonic_ms();
    tt_apply_sync_summaries(&db);
    tt_store_generate_file_summaries(&store);
    TT_DIAG("summary", "done",
            "\"dur_ms\":%llu",
            (unsigned long long)(tt_monotonic_ms() - t_upd_sum_start));

    /* Schema maintenance: skip expensive rebuilds in incremental mode */
    {
        uint64_t t0 = tt_monotonic_ms();
        TT_DIAG("schema", "start", NULL);

        if (use_incremental)
        {
            /* Incremental: indexes/triggers were never dropped, FTS was
             * maintained via triggers. Only recompute centrality if the
             * import graph changed. */
            TT_DIAG("schema", "secondary_idx", "\"dur_ms\":0,\"skipped\":true");
            TT_DIAG("schema", "fts_rebuild", "\"dur_ms\":0,\"skipped\":true");
            TT_DIAG("schema", "fts_triggers", "\"dur_ms\":0,\"skipped\":true");

            if (imports_changed)
            {
                tt_store_resolve_imports(&store);
                uint64_t t1 = tt_monotonic_ms();
                TT_DIAG("schema", "resolve_imports",
                        "\"dur_ms\":%llu", (unsigned long long)(t1 - t0));

                tt_store_compute_centrality(&store);
                uint64_t t2 = tt_monotonic_ms();
                TT_DIAG("schema", "centrality",
                        "\"dur_ms\":%llu", (unsigned long long)(t2 - t1));
            }
            else
            {
                TT_DIAG("schema", "centrality",
                        "\"dur_ms\":0,\"skipped\":true");
            }
        }
        else
        {
            /* Bulk mode: full rebuild (same as index:create) */
            tt_schema_create_secondary_indexes(db.db);
            uint64_t t1 = tt_monotonic_ms();
            TT_DIAG("schema", "secondary_idx",
                    "\"dur_ms\":%llu", (unsigned long long)(t1 - t0));

            tt_schema_rebuild_fts(db.db);
            uint64_t t2 = tt_monotonic_ms();
            TT_DIAG("schema", "fts_rebuild",
                    "\"dur_ms\":%llu", (unsigned long long)(t2 - t1));

            tt_schema_create_fts_triggers(db.db);
            uint64_t t3 = tt_monotonic_ms();
            TT_DIAG("schema", "fts_triggers",
                    "\"dur_ms\":%llu", (unsigned long long)(t3 - t2));

            tt_store_resolve_imports(&store);
            uint64_t t4 = tt_monotonic_ms();
            TT_DIAG("schema", "resolve_imports",
                    "\"dur_ms\":%llu", (unsigned long long)(t4 - t3));

            tt_store_compute_centrality(&store);
            uint64_t t5 = tt_monotonic_ms();
            TT_DIAG("schema", "centrality",
                    "\"dur_ms\":%llu", (unsigned long long)(t5 - t4));
        }

        TT_DIAG("schema", "done",
                "\"dur_ms\":%llu,\"incremental\":%s",
                (unsigned long long)(tt_monotonic_ms() - t0),
                use_incremental ? "true" : "false");
    }

    set_metadata(&store, project_path);

    /* WAL checkpoint */
    sqlite3_exec(db.db, "PRAGMA wal_checkpoint(TRUNCATE)", NULL, NULL, NULL);

    /* Build result */
    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "changed", changes.changed_count);
    cJSON_AddNumberToObject(result, "added", changes.added_count);
    cJSON_AddNumberToObject(result, "deleted", changes.deleted_count);
    cJSON_AddNumberToObject(result, "duration_seconds", tt_timer_elapsed_sec());

    /* Capture RSS after schema work, before reporting peak */
    TT_DIAG_MEM();

    /* Final diagnostic summary */
    TT_DIAG("done", "summary",
            "\"dur_s\":%.2f,\"peak_rss_kb\":%llu,"
            "\"changed\":%d,\"added\":%d,\"deleted\":%d",
            tt_timer_elapsed_sec(),
            (unsigned long long)tt_diag_peak_rss_kb(),
            changes.changed_count, changes.added_count, changes.deleted_count);

    /* Cleanup */
    tt_changes_fast_free(&changes);
    tt_store_close(&store);
    tt_discovered_paths_free(&discovered);
    tt_config_free(&config);
    tt_database_close(&db);
    free(project_path);

    return result;

interrupted:
    tt_database_rollback(&db);
    tt_changes_fast_free(&changes);
    tt_store_close(&store);
    tt_discovered_paths_free(&discovered);
    free(ctags_path);
    tt_config_free(&config);
    tt_database_close(&db);
    free(project_path);
    tt_error_set("Interrupted");
    return NULL;
}

/* ---- CLI wrappers ---- */

int tt_cmd_index_create(tt_cli_opts_t *opts)
{
    cJSON *result = tt_cmd_index_create_exec(opts);
    if (!result)
    {
        return tt_output_error("internal_error", tt_error_get(), NULL);
    }
    if (cJSON_GetObjectItem(result, "error"))
    {
        tt_json_print(result);
        cJSON_Delete(result);
        return 1;
    }
    tt_json_print(result);
    cJSON_Delete(result);
    return 0;
}

int tt_cmd_index_update(tt_cli_opts_t *opts)
{
    cJSON *result = tt_cmd_index_update_exec(opts);
    if (!result)
    {
        return tt_output_error("internal_error", tt_error_get(), NULL);
    }
    if (cJSON_GetObjectItem(result, "error"))
    {
        tt_json_print(result);
        cJSON_Delete(result);
        return 1;
    }
    tt_json_print(result);
    cJSON_Delete(result);
    return 0;
}

/* ---- index:file -- single-file reindex ---- */

cJSON *tt_cmd_index_file_exec(tt_cli_opts_t *opts)
{
    tt_timer_start();

    const char *file_arg = NULL;
    if (opts->positional_count > 0)
        file_arg = opts->positional[0];
    else if (opts->search && opts->search[0])
        file_arg = opts->search;

    if (!file_arg || !file_arg[0])
        return make_error("missing_argument",
                          "Usage: index:file <filepath>",
                          "Specify a file path to reindex");

    char *project_path = tt_resolve_project_path(opts->path);
    if (!project_path)
    {
        tt_error_set("Failed to resolve project path");
        return NULL;
    }

    /* Resolve file to relative path within project */
    size_t root_len = strlen(project_path);
    const char *rel_path = file_arg;

    /* If absolute path, strip project root prefix */
    if (file_arg[0] == '/' && strlen(file_arg) > root_len &&
        memcmp(file_arg, project_path, root_len) == 0 &&
        file_arg[root_len] == '/')
    {
        rel_path = file_arg + root_len + 1;
    }

    /* Build absolute path for stat/hash */
    char abs_path[4096];
    if (file_arg[0] == '/')
        snprintf(abs_path, sizeof(abs_path), "%s", file_arg);
    else
        snprintf(abs_path, sizeof(abs_path), "%s/%s", project_path, rel_path);

    /* Check file exists */
    struct stat st;
    if (stat(abs_path, &st) != 0)
    {
        free(project_path);
        return make_error("file_not_found",
                          "File does not exist",
                          abs_path);
    }

    /* Open database (must already exist) */
    if (!tt_database_exists(project_path))
    {
        free(project_path);
        return make_error("no_index",
                          "No index found. Run index:create first.",
                          NULL);
    }

    tt_database_t db;
    memset(&db, 0, sizeof(db));
    if (tt_database_open(&db, project_path) < 0)
    {
        free(project_path);
        return make_error("storage_error",
                          "Failed to open database.",
                          NULL);
    }

    tt_index_store_t store;
    if (tt_store_init(&store, &db) < 0)
    {
        tt_database_close(&db);
        free(project_path);
        return make_error("storage_error", "Failed to prepare statements", NULL);
    }

    /* Hash check: unchanged? */
    char *new_hash = tt_fast_hash_file(abs_path);
    if (!new_hash)
    {
        tt_store_close(&store);
        tt_database_close(&db);
        free(project_path);
        return make_error("hash_error", "Failed to hash file", abs_path);
    }

    tt_file_record_t old_rec;
    memset(&old_rec, 0, sizeof(old_rec));
    int has_old = (tt_store_get_file(&store, rel_path, &old_rec) == 0);
    if (has_old && old_rec.hash && strcmp(old_rec.hash, new_hash) == 0)
    {
        /* Unchanged */
        cJSON *result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "changed", false);
        cJSON_AddStringToObject(result, "file", rel_path);
        cJSON_AddNumberToObject(result, "duration_seconds",
                                tt_timer_elapsed_sec());

        tt_file_record_free(&old_rec);
        free(new_hash);
        tt_store_close(&store);
        tt_database_close(&db);
        free(project_path);
        return result;
    }
    tt_file_record_free(&old_rec);
    free(new_hash);

    /* Delete old data for this file (cascades to symbols + imports) */
    if (has_old)
    {
        tt_database_begin(&db);
        tt_store_delete_file(&store, rel_path);
        tt_database_commit(&db);
    }

    /* Find ctags */
    char *ctags_path = tt_ctags_resolve();
    if (!ctags_path)
    {
        tt_store_close(&store);
        tt_database_close(&db);
        free(project_path);
        return make_error("ctags_not_found",
                          "Universal Ctags not found",
                          "Install universal-ctags or ensure it is in PATH");
    }

    /* Run pipeline in incremental mode: keeps FTS triggers active so the
     * index is updated in O(symbols_in_file) instead of O(total_symbols). */
    tt_pipeline_config_t pcfg = {0};
    pcfg.project_root = project_path;
    pcfg.ctags_path = ctags_path;
    pcfg.file_paths = &rel_path;
    pcfg.file_count = 1;
    pcfg.num_workers = 1;
    pcfg.ctags_timeout_sec = 30;
    pcfg.max_file_size_bytes = 10 * 1024 * 1024; /* 10MB */
    pcfg.db = &db;
    pcfg.store = &store;
    pcfg.incremental = true;

    tt_pipeline_result_t presult;
    if (tt_pipeline_run(&pcfg, &presult) < 0)
    {
        free(ctags_path);
        tt_store_close(&store);
        tt_database_close(&db);
        free(project_path);
        return make_error("pipeline_error",
                          "Failed to index file", NULL);
    }

    int syms = presult.symbols_indexed;
    int imps = presult.imports_indexed;
    tt_pipeline_result_free(&presult);

    free(ctags_path);

    /* Resolve import specifiers to file paths */
    if (imps > 0)
        tt_store_resolve_imports(&store);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "changed", true);
    cJSON_AddStringToObject(result, "file", rel_path);
    cJSON_AddNumberToObject(result, "symbols", syms);
    cJSON_AddNumberToObject(result, "imports", imps);
    cJSON_AddNumberToObject(result, "duration_seconds",
                            tt_timer_elapsed_sec());

    tt_store_close(&store);
    tt_database_close(&db);
    free(project_path);
    return result;
}

int tt_cmd_index_file(tt_cli_opts_t *opts)
{
    cJSON *result = tt_cmd_index_file_exec(opts);
    if (!result)
    {
        return tt_output_error("internal_error", tt_error_get(), NULL);
    }
    if (cJSON_GetObjectItem(result, "error"))
    {
        tt_json_print(result);
        cJSON_Delete(result);
        return 1;
    }
    tt_json_print(result);
    cJSON_Delete(result);
    return 0;
}
