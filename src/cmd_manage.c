/*
 * cmd_manage.c -- Management commands: stats, projects:list, cache:clear, codebase:detect.
 */

#include "cmd_manage.h"
#include "json_output.h"
#include "error.h"
#include "platform.h"
#include "database.h"
#include "index_store.h"
#include "storage_paths.h"
#include "config.h"
#include "str_util.h"
#include "token_savings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <time.h>

/* ---- Shared helpers ---- */

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

/* ---- stats ---- */

cJSON *tt_cmd_stats_exec(tt_cli_opts_t *opts)
{
    tt_timer_start();

    char *project_path = tt_resolve_project_path(opts->path);
    if (!project_path)
    {
        tt_error_set("Failed to resolve project path");
        return NULL;
    }

    if (!tt_database_exists(project_path))
    {
        cJSON *err = make_error("no_index", "No index found.",
                                "Run \"toktoken index:create\" first.");
        free(project_path);
        return err;
    }

    tt_database_t db;
    if (tt_database_open(&db, project_path) < 0)
    {
        cJSON *err = make_error("db_open_failed", tt_error_get(), NULL);
        free(project_path);
        return err;
    }

    tt_index_store_t store;
    if (tt_store_init(&store, &db) < 0)
    {
        tt_database_close(&db);
        free(project_path);
        return NULL;
    }

    tt_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    if (tt_store_get_stats(&store, &stats) < 0)
    {
        tt_store_close(&store);
        tt_database_close(&db);
        free(project_path);
        return NULL;
    }

    /* Build JSON result */
    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "files", stats.files);
    cJSON_AddNumberToObject(result, "symbols", stats.symbols);

    /* languages: {name: count} */
    cJSON *langs = cJSON_CreateObject();
    for (int i = 0; i < stats.language_count; i++)
        cJSON_AddNumberToObject(langs, stats.languages[i].name, stats.languages[i].count);
    cJSON_AddItemToObject(result, "languages", langs);

    /* kinds: {name: count} */
    cJSON *kinds = cJSON_CreateObject();
    for (int i = 0; i < stats.kind_count; i++)
        cJSON_AddNumberToObject(kinds, stats.kinds[i].name, stats.kinds[i].count);
    cJSON_AddItemToObject(result, "kinds", kinds);

    /* dirs: {name: count} */
    cJSON *dirs = cJSON_CreateObject();
    for (int i = 0; i < stats.dir_count; i++)
        cJSON_AddNumberToObject(dirs, stats.dirs[i].name, stats.dirs[i].count);
    cJSON_AddItemToObject(result, "dirs", dirs);

    /* Staleness warning */
    char *indexed_at = tt_store_get_metadata(&store, "indexed_at");
    if (indexed_at && indexed_at[0])
    {
        tt_config_t config;
        tt_config_load(&config, project_path);

        struct tm tm_val;
        memset(&tm_val, 0, sizeof(tm_val));
        /* Parse ISO 8601 / RFC 3339: "2026-03-10T15:30:00+01:00" */
        if (sscanf(indexed_at, "%d-%d-%dT%d:%d:%d",
                   &tm_val.tm_year, &tm_val.tm_mon, &tm_val.tm_mday,
                   &tm_val.tm_hour, &tm_val.tm_min, &tm_val.tm_sec) >= 6)
        {
            tm_val.tm_year -= 1900;
            tm_val.tm_mon -= 1;
            tm_val.tm_isdst = -1;
            time_t indexed_time = mktime(&tm_val);
            if (indexed_time != (time_t)-1)
            {
                time_t now = time(NULL);
                int age_days = (int)((now - indexed_time) / 86400);
                if (age_days >= config.staleness_days)
                {
                    char warning[256];
                    snprintf(warning, sizeof(warning),
                             "Index is %d days old. Run \"toktoken index:update\" "
                             "or \"toktoken index:create\" to refresh.",
                             age_days);
                    cJSON_AddStringToObject(result, "staleness_warning", warning);
                }
            }
        }
        tt_config_free(&config);
    }
    free(indexed_at);

    /* Savings section */
    tt_savings_totals_t totals;
    if (tt_savings_get_totals(&db, &totals) == 0 && totals.total_calls > 0)
    {
        cJSON *savings_json = cJSON_CreateObject();
        cJSON_AddNumberToObject(savings_json, "total_calls",
                                (double)totals.total_calls);
        cJSON_AddNumberToObject(savings_json, "total_raw_bytes",
                                (double)totals.total_raw_bytes);
        cJSON_AddNumberToObject(savings_json, "total_response_bytes",
                                (double)totals.total_response_bytes);
        cJSON_AddNumberToObject(savings_json, "total_tokens_saved",
                                (double)totals.total_tokens_saved);

        /* Per-tool breakdown */
        tt_savings_per_tool_t *per_tool = NULL;
        int per_tool_count = 0;
        if (tt_savings_get_per_tool(&db, &per_tool, &per_tool_count) == 0
            && per_tool_count > 0)
        {
            cJSON *pt_arr = cJSON_CreateArray();
            for (int i = 0; i < per_tool_count; i++)
            {
                cJSON *entry = cJSON_CreateObject();
                cJSON_AddStringToObject(entry, "tool", per_tool[i].tool_name);
                cJSON_AddNumberToObject(entry, "calls",
                                        (double)per_tool[i].call_count);
                cJSON_AddNumberToObject(entry, "tokens_saved",
                                        (double)per_tool[i].tokens_saved);
                cJSON_AddItemToArray(pt_arr, entry);
            }
            cJSON_AddItemToObject(savings_json, "per_tool", pt_arr);
            tt_savings_free_per_tool(per_tool, per_tool_count);
        }

        cJSON_AddItemToObject(result, "savings", savings_json);
    }

    tt_stats_free(&stats);
    tt_store_close(&store);
    tt_database_close(&db);
    free(project_path);

    return result;
}

int tt_cmd_stats(tt_cli_opts_t *opts)
{
    /* Table format */
    const char *format = opts->format ? opts->format : "json";
    if (strcmp(format, "table") == 0)
    {
        /* For table mode, still use _exec to get the data */
        cJSON *result = tt_cmd_stats_exec(opts);
        if (!result)
            return tt_output_error("internal_error", tt_error_get(), NULL);
        if (cJSON_GetObjectItemCaseSensitive(result, "error"))
        {
            tt_json_print(result);
            cJSON_Delete(result);
            return 1;
        }

        int files = cJSON_GetObjectItemCaseSensitive(result, "files")->valueint;
        int symbols = cJSON_GetObjectItemCaseSensitive(result, "symbols")->valueint;
        printf("Files:   %d\n", files);
        printf("Symbols: %d\n", symbols);
        printf("\n");

        printf("Languages:\n");
        cJSON *langs = cJSON_GetObjectItemCaseSensitive(result, "languages");
        cJSON *el;
        cJSON_ArrayForEach(el, langs)
            printf("  %s: %d\n", el->string, el->valueint);

        printf("\n");
        printf("Symbol kinds:\n");
        cJSON *kinds = cJSON_GetObjectItemCaseSensitive(result, "kinds");
        cJSON_ArrayForEach(el, kinds)
            printf("  %s: %d\n", el->string, el->valueint);

        cJSON_Delete(result);
        return 0;
    }

    cJSON *result = tt_cmd_stats_exec(opts);
    if (!result)
        return tt_output_error("internal_error", tt_error_get(), NULL);
    if (cJSON_GetObjectItemCaseSensitive(result, "error"))
    {
        tt_json_print(result);
        cJSON_Delete(result);
        return 1;
    }
    return tt_output_success(result);
}

/* ---- projects:list ---- */

typedef struct
{
    cJSON *json;
    char *indexed_at;
} proj_entry_t;

static int proj_entry_cmp_desc(const void *a, const void *b)
{
    const proj_entry_t *ea = (const proj_entry_t *)a;
    const proj_entry_t *eb = (const proj_entry_t *)b;
    const char *sa = ea->indexed_at ? ea->indexed_at : "";
    const char *sb = eb->indexed_at ? eb->indexed_at : "";
    return strcmp(sb, sa);
}

cJSON *tt_cmd_projects_list_exec(tt_cli_opts_t *opts)
{
    (void)opts;
    tt_timer_start();

    char *projects_dir = tt_storage_projects_dir();
    if (!projects_dir || !tt_is_dir(projects_dir))
    {
        free(projects_dir);
        cJSON *result = cJSON_CreateObject();
        cJSON_AddItemToObject(result, "projects", cJSON_CreateArray());
        cJSON_AddNumberToObject(result, "n", 0);
        return result;
    }

    DIR *dp = opendir(projects_dir);
    if (!dp)
    {
        free(projects_dir);
        cJSON *result = cJSON_CreateObject();
        cJSON_AddItemToObject(result, "projects", cJSON_CreateArray());
        cJSON_AddNumberToObject(result, "n", 0);
        return result;
    }

    /* Collect project records into a sortable array */
    proj_entry_t *entries = NULL;
    int entry_count = 0;

    struct dirent *de;
    while ((de = readdir(dp)) != NULL)
    {
        if (de->d_name[0] == '.')
            continue;

        /* Build db path: projects_dir/hash/db.sqlite */
        char *dir_path = tt_path_join(projects_dir, de->d_name);
        if (!dir_path)
            continue;
        char *db_path = tt_path_join(dir_path, "db.sqlite");
        free(dir_path);
        if (!db_path)
            continue;

        if (!tt_file_exists(db_path))
        {
            free(db_path);
            continue;
        }

        /* Open the database directly to read metadata */
        sqlite3 *raw_db = NULL;
        if (sqlite3_open_v2(db_path, &raw_db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
        {
            free(db_path);
            continue;
        }

        /* Helper to read a metadata value */
        const char *meta_sql = "SELECT value FROM metadata WHERE key = ?";
        char *project_path = NULL;
        char *indexed_at = NULL;
        char *git_head = NULL;
        int file_count = 0;
        int symbol_count = 0;

        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(raw_db, meta_sql, -1, &stmt, NULL) == SQLITE_OK)
        {
            /* project_path */
            sqlite3_bind_text(stmt, 1, "project_path", -1, SQLITE_STATIC);
            if (sqlite3_step(stmt) == SQLITE_ROW)
                project_path = tt_strdup((const char *)sqlite3_column_text(stmt, 0));
            sqlite3_reset(stmt);

            /* indexed_at */
            sqlite3_bind_text(stmt, 1, "indexed_at", -1, SQLITE_STATIC);
            if (sqlite3_step(stmt) == SQLITE_ROW)
                indexed_at = tt_strdup((const char *)sqlite3_column_text(stmt, 0));
            sqlite3_reset(stmt);

            /* git_head */
            sqlite3_bind_text(stmt, 1, "git_head", -1, SQLITE_STATIC);
            if (sqlite3_step(stmt) == SQLITE_ROW)
                git_head = tt_strdup((const char *)sqlite3_column_text(stmt, 0));

            sqlite3_finalize(stmt);
        }

        /* Count files */
        stmt = NULL;
        if (sqlite3_prepare_v2(raw_db, "SELECT COUNT(*) FROM files", -1, &stmt, NULL) == SQLITE_OK)
        {
            if (sqlite3_step(stmt) == SQLITE_ROW)
                file_count = sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);
        }

        /* Count symbols */
        stmt = NULL;
        if (sqlite3_prepare_v2(raw_db, "SELECT COUNT(*) FROM symbols", -1, &stmt, NULL) == SQLITE_OK)
        {
            if (sqlite3_step(stmt) == SQLITE_ROW)
                symbol_count = sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);
        }

        sqlite3_close(raw_db);

        /* DB file size */
        int64_t db_size = tt_file_size(db_path);
        if (db_size < 0)
            db_size = 0;

        /* Build project JSON */
        cJSON *proj = cJSON_CreateObject();
        cJSON_AddStringToObject(proj, "path", (project_path && project_path[0]) ? project_path : "(unknown)");
        cJSON_AddStringToObject(proj, "hash", de->d_name);
        cJSON_AddNumberToObject(proj, "files", file_count);
        cJSON_AddNumberToObject(proj, "symbols", symbol_count);
        cJSON_AddNumberToObject(proj, "db_size", (double)db_size);
        cJSON_AddStringToObject(proj, "indexed_at", indexed_at ? indexed_at : "");

        /* git_head: first 8 chars, only if non-empty */
        if (git_head && git_head[0])
        {
            char short_head[9];
            size_t len = strlen(git_head);
            if (len > 8)
                len = 8;
            memcpy(short_head, git_head, len);
            short_head[len] = '\0';
            cJSON_AddStringToObject(proj, "git_head", short_head);
        }

        /* stale: project_path is non-empty but directory doesn't exist */
        if (project_path && project_path[0] && !tt_is_dir(project_path))
            cJSON_AddTrueToObject(proj, "stale");

        /* Add to entries array */
        proj_entry_t *new_entries = realloc(entries, (size_t)(entry_count + 1) * sizeof(proj_entry_t));
        if (new_entries)
        {
            entries = new_entries;
            entries[entry_count].json = proj;
            entries[entry_count].indexed_at = indexed_at; /* transfer ownership */
            entry_count++;
        }
        else
        {
            cJSON_Delete(proj);
            free(indexed_at);
        }

        free(project_path);
        free(git_head);
        free(db_path);
    }
    closedir(dp);
    free(projects_dir);

    /* Sort by indexed_at DESC */
    if (entry_count > 1)
    {
        qsort(entries, (size_t)entry_count, sizeof(proj_entry_t),
              proj_entry_cmp_desc);
    }

    /* Build output */
    cJSON *result = cJSON_CreateObject();
    cJSON *projects_arr = cJSON_CreateArray();
    for (int i = 0; i < entry_count; i++)
    {
        cJSON_AddItemToArray(projects_arr, entries[i].json);
        free(entries[i].indexed_at);
    }
    free(entries);

    cJSON_AddItemToObject(result, "projects", projects_arr);
    cJSON_AddNumberToObject(result, "n", entry_count);

    return result;
}

int tt_cmd_projects_list(tt_cli_opts_t *opts)
{
    cJSON *result = tt_cmd_projects_list_exec(opts);
    if (!result)
        return tt_output_error("internal_error", tt_error_get(), NULL);
    return tt_output_success(result);
}

/* ---- cache:clear ---- */

/* Remove db.sqlite, WAL, SHM files, and the empty project directory */
static void cleanup_db_files(const char *db_path, const char *project_dir)
{
    tt_strbuf_t buf;
    tt_strbuf_init(&buf);

    tt_strbuf_append_str(&buf, db_path);
    tt_strbuf_append_str(&buf, "-wal");
    tt_remove_file(buf.data); /* ignore errors */

    tt_strbuf_reset(&buf);
    tt_strbuf_append_str(&buf, db_path);
    tt_strbuf_append_str(&buf, "-shm");
    tt_remove_file(buf.data); /* ignore errors */

    tt_strbuf_free(&buf);

    tt_remove_dir(project_dir); /* ignore errors (may not be empty) */
}

cJSON *tt_cmd_cache_clear_exec(tt_cli_opts_t *opts)
{
    tt_timer_start();

    if (opts->all)
    {
        /* ---- Purge all mode: remove entire base directory ---- */
        if (!opts->force)
        {
            return make_error("confirmation_required",
                              "This will delete ALL TokToken data (indexes, logs, config). "
                              "Pass --force to proceed.",
                              "Run: toktoken cache:clear --all --force");
        }

        char *base_dir = tt_storage_base_dir();
        if (!base_dir || !tt_is_dir(base_dir))
        {
            free(base_dir);
            cJSON *result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "purged", "(not found)");
            cJSON_AddNumberToObject(result, "freed_bytes", 0);
            return result;
        }

        /* Measure total size before removal */
        int64_t total_size = 0;
        DIR *dp = opendir(base_dir);
        if (dp)
        {
            struct dirent *de;
            while ((de = readdir(dp)) != NULL)
            {
                if (de->d_name[0] == '.')
                    continue;
                char *child = tt_path_join(base_dir, de->d_name);
                if (!child)
                    continue;
                if (tt_is_dir(child))
                {
                    DIR *sub = opendir(child);
                    if (sub)
                    {
                        struct dirent *se;
                        while ((se = readdir(sub)) != NULL)
                        {
                            if (se->d_name[0] == '.')
                                continue;
                            char *file = tt_path_join(child, se->d_name);
                            if (file)
                            {
                                int64_t sz = tt_file_size(file);
                                if (sz > 0)
                                    total_size += sz;
                                free(file);
                            }
                        }
                        closedir(sub);
                    }
                }
                else
                {
                    int64_t sz = tt_file_size(child);
                    if (sz > 0)
                        total_size += sz;
                }
                free(child);
            }
            closedir(dp);
        }

        if (tt_remove_dir_recursive(base_dir) < 0)
        {
            cJSON *err = make_error("purge_failed",
                                    "Failed to remove TokToken data directory.",
                                    base_dir);
            free(base_dir);
            return err;
        }

        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "purged", base_dir);
        cJSON_AddNumberToObject(result, "freed_bytes", (double)total_size);
        free(base_dir);
        return result;
    }

    /* ---- Single project mode ---- */
    char *project_path = tt_resolve_project_path(opts->path);
    if (!project_path)
    {
        tt_error_set("Failed to resolve project path");
        return NULL;
    }

    char *project_dir = tt_storage_project_dir(project_path);
    if (!project_dir)
    {
        free(project_path);
        return make_error("no_index", "No index found to clear.", NULL);
    }

    char *db_path = tt_path_join(project_dir, "db.sqlite");
    free(project_path);
    if (!db_path)
    {
        free(project_dir);
        return NULL;
    }

    if (!tt_file_exists(db_path))
    {
        free(db_path);
        free(project_dir);
        return make_error("no_index", "No index found to clear.", NULL);
    }

    int64_t size = tt_file_size(db_path);
    if (size < 0)
        size = 0;

    if (tt_remove_file(db_path) < 0)
    {
        free(db_path);
        free(project_dir);
        return make_error("delete_failed", "Failed to delete index database.", NULL);
    }

    cleanup_db_files(db_path, project_dir);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "deleted", db_path);
    cJSON_AddNumberToObject(result, "freed_bytes", (double)size);

    free(db_path);
    free(project_dir);
    return result;
}

int tt_cmd_cache_clear(tt_cli_opts_t *opts)
{
    cJSON *result = tt_cmd_cache_clear_exec(opts);
    if (!result)
        return tt_output_error("internal_error", tt_error_get(), NULL);
    if (cJSON_GetObjectItemCaseSensitive(result, "error"))
    {
        tt_json_print(result);
        cJSON_Delete(result);
        return 1;
    }
    return tt_output_success(result);
}

/* ---- codebase:detect ---- */

/* Manifest basenames -> ecosystem (21 entries) */
static const struct
{
    const char *file;
    const char *ecosystem;
} MANIFESTS[] = {
    {"composer.json", "php"},
    {"package.json", "node"},
    {"Cargo.toml", "rust"},
    {"go.mod", "go"},
    {"pyproject.toml", "python"},
    {"setup.py", "python"},
    {"setup.cfg", "python"},
    {"requirements.txt", "python"},
    {"Gemfile", "ruby"},
    {"build.gradle", "java"},
    {"build.gradle.kts", "kotlin"},
    {"pom.xml", "java"},
    {"CMakeLists.txt", "c/cpp"},
    {"Makefile", "c/cpp"},
    {"mix.exs", "elixir"},
    {"pubspec.yaml", "dart"},
    {"stack.yaml", "haskell"},
    {"cabal.project", "haskell"},
    {"build.sbt", "scala"},
    {"build.zig", "zig"},
    {"flake.nix", "nix"},
};
#define MANIFEST_COUNT (sizeof(MANIFESTS) / sizeof(MANIFESTS[0]))

/* Extension-based manifests (4 entries) */
static const struct
{
    const char *ext;
    const char *ecosystem;
} MANIFEST_EXTENSIONS[] = {
    {".sln", "dotnet"},
    {".csproj", "dotnet"},
    {".fsproj", "dotnet"},
    {".cabal", "haskell"},
};
#define MANIFEST_EXT_COUNT (sizeof(MANIFEST_EXTENSIONS) / sizeof(MANIFEST_EXTENSIONS[0]))

/* Source extensions for heuristic */
static const char *SOURCE_EXT_SET[] = {
    "php",
    "phtml",
    "inc",
    "js",
    "jsx",
    "mjs",
    "cjs",
    "ts",
    "tsx",
    "mts",
    "vue",
    "py",
    "pyw",
    "go",
    "rs",
    "java",
    "c",
    "h",
    "cpp",
    "cxx",
    "cc",
    "hpp",
    "hxx",
    "hh",
    "cs",
    "rb",
    "kt",
    "kts",
    "swift",
    "dart",
    "lua",
    "pl",
    "pm",
    "sh",
    "bash",
    "zsh",
    "sql",
    "r",
    "R",
    "scala",
    "ex",
    "exs",
    "erl",
    "hrl",
    "hs",
    "ml",
    "mli",
    "vim",
    "el",
    "clj",
    "cljs",
    "cljc",
    "groovy",
    "v",
    "sv",
    "nix",
    "gleam",
    "ejs",
};
#define SOURCE_EXT_COUNT (sizeof(SOURCE_EXT_SET) / sizeof(SOURCE_EXT_SET[0]))

/* Directories to skip during recursive scan (case-insensitive) */
static const char *SKIP_DIRS[] = {
    "vendor",
    "node_modules",
    ".git",
    ".svn",
    ".hg",
    "__pycache__",
    ".tox",
    ".venv",
    "venv",
    "dist",
    "build",
    "target",
    ".idea",
    ".vscode",
};
#define SKIP_DIR_COUNT (sizeof(SKIP_DIRS) / sizeof(SKIP_DIRS[0]))

#define HEURISTIC_SOURCE_THRESHOLD 5
#define MANIFEST_SCAN_DEPTH 2

/* Check if a directory name should be skipped */
static bool is_skip_dir(const char *name)
{
    for (size_t i = 0; i < SKIP_DIR_COUNT; i++)
    {
        if (tt_strcasecmp(name, SKIP_DIRS[i]) == 0)
            return true;
    }
    return false;
}

/* Check if extension is a known source extension */
static bool is_source_ext(const char *ext)
{
    for (size_t i = 0; i < SOURCE_EXT_COUNT; i++)
    {
        if (strcmp(ext, SOURCE_EXT_SET[i]) == 0)
            return true;
    }
    return false;
}

/* Add ecosystem to hashmap (set semantics) */
static void add_ecosystem(tt_hashmap_t *set, const char *ecosystem)
{
    if (!tt_hashmap_has(set, ecosystem))
        tt_hashmap_set(set, ecosystem, (void *)1);
}

/* Scan a single directory for manifest files */
static void scan_manifests_in_dir(const char *dir, tt_hashmap_t *ecosystems)
{
    /* Fixed-name manifests: direct stat */
    for (size_t i = 0; i < MANIFEST_COUNT; i++)
    {
        char *full = tt_path_join(dir, MANIFESTS[i].file);
        if (full)
        {
            if (tt_file_exists(full))
                add_ecosystem(ecosystems, MANIFESTS[i].ecosystem);
            free(full);
        }
    }

    /* Extension-based manifests: scandir */
    DIR *dp = opendir(dir);
    if (!dp)
        return;

    struct dirent *de;
    while ((de = readdir(dp)) != NULL)
    {
        if (de->d_name[0] == '.')
            continue;
        for (size_t i = 0; i < MANIFEST_EXT_COUNT; i++)
        {
            if (tt_str_ends_with(de->d_name, MANIFEST_EXTENSIONS[i].ext))
            {
                char *full = tt_path_join(dir, de->d_name);
                if (full)
                {
                    if (!tt_is_dir(full))
                        add_ecosystem(ecosystems, MANIFEST_EXTENSIONS[i].ecosystem);
                    free(full);
                }
            }
        }
    }
    closedir(dp);
}

/* Recursive manifest scan with depth limit and early exit */
static void scan_manifests_recursive(const char *dir, tt_hashmap_t *ecosystems, int depth)
{
    DIR *dp = opendir(dir);
    if (!dp)
        return;

    struct dirent *de;
    while ((de = readdir(dp)) != NULL)
    {
        if (de->d_name[0] == '.')
            continue;
        if (is_skip_dir(de->d_name))
            continue;

        char *subdir = tt_path_join(dir, de->d_name);
        if (!subdir)
            continue;

        if (!tt_is_dir(subdir))
        {
            free(subdir);
            continue;
        }

        scan_manifests_in_dir(subdir, ecosystems);
        if (tt_hashmap_count(ecosystems) > 0)
        {
            free(subdir);
            closedir(dp);
            return;
        }

        if (depth < MANIFEST_SCAN_DEPTH)
        {
            scan_manifests_recursive(subdir, ecosystems, depth + 1);
            if (tt_hashmap_count(ecosystems) > 0)
            {
                free(subdir);
                closedir(dp);
                return;
            }
        }

        free(subdir);
    }
    closedir(dp);
}

/* Count source files recursively with early exit at threshold */
static int count_source_files(const char *dir, int count)
{
    DIR *dp = opendir(dir);
    if (!dp)
        return count;

    struct dirent *de;
    while ((de = readdir(dp)) != NULL)
    {
        if (de->d_name[0] == '.')
            continue;

        char *full = tt_path_join(dir, de->d_name);
        if (!full)
            continue;

        if (tt_is_dir(full))
        {
            if (!is_skip_dir(de->d_name))
            {
                count = count_source_files(full, count);
                if (count >= HEURISTIC_SOURCE_THRESHOLD)
                {
                    free(full);
                    closedir(dp);
                    return count;
                }
            }
            free(full);
            continue;
        }
        free(full);

        /* Extract extension */
        const char *dot = strrchr(de->d_name, '.');
        if (dot)
        {
            const char *ext = dot + 1;
            if (is_source_ext(ext))
            {
                count++;
                if (count >= HEURISTIC_SOURCE_THRESHOLD)
                {
                    closedir(dp);
                    return count;
                }
            }
        }
    }
    closedir(dp);
    return count;
}

/* Callback for hashmap iteration: collect ecosystem names */
static int collect_ecosystems_cb(const char *key, void *value, void *userdata)
{
    (void)value;
    cJSON *arr = (cJSON *)userdata;
    cJSON_AddItemToArray(arr, cJSON_CreateString(key));
    return 0;
}

cJSON *tt_cmd_codebase_detect_exec(tt_cli_opts_t *opts, int *out_exit_code)
{
    char *path = tt_resolve_project_path(opts->path);
    if (!path)
    {
        tt_error_set("Failed to resolve project path");
        if (out_exit_code)
            *out_exit_code = 1;
        return NULL;
    }

    if (!tt_is_dir(path))
    {
        cJSON *err = make_error("invalid_path", "Directory not found", NULL);
        /* Add the path to the message */
        cJSON_DeleteItemFromObject(err, "message");
        char msg[512];
        snprintf(msg, sizeof(msg), "Directory not found: %s", path);
        cJSON_AddStringToObject(err, "message", msg);
        free(path);
        if (out_exit_code)
            *out_exit_code = 1;
        return err;
    }

    tt_hashmap_t *ecosystems = tt_hashmap_new(16);
    const char *detection = "none";

    /* Phase 1: Root-level manifest check */
    scan_manifests_in_dir(path, ecosystems);
    if (tt_hashmap_count(ecosystems) > 0)
    {
        detection = "manifest-root";
    }
    else
    {
        /* Phase 2: Depth-limited recursive scan */
        scan_manifests_recursive(path, ecosystems, 1);
        if (tt_hashmap_count(ecosystems) > 0)
        {
            detection = "manifest-subdir";
        }
        else
        {
            /* Phase 3: Heuristic (.git + source count) */
            char *git_dir = tt_path_join(path, ".git");
            if (git_dir)
            {
                if (tt_is_dir(git_dir))
                {
                    int count = count_source_files(path, 0);
                    if (count >= HEURISTIC_SOURCE_THRESHOLD)
                        detection = "heuristic";
                }
                free(git_dir);
            }
        }
    }

    bool is_codebase = (strcmp(detection, "none") != 0);
    bool has_index = tt_database_exists(path);

    /* Build output */
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "is_codebase", is_codebase);

    /* ecosystems: array of unique names */
    cJSON *eco_arr = cJSON_CreateArray();
    tt_hashmap_iter(ecosystems, collect_ecosystems_cb, eco_arr);
    cJSON_AddItemToObject(result, "ecosystems", eco_arr);

    cJSON_AddBoolToObject(result, "has_index", has_index);
    cJSON_AddStringToObject(result, "detection", detection);

    /* action */
    if (is_codebase && !has_index)
        cJSON_AddStringToObject(result, "action", "index:create");
    else if (is_codebase && has_index)
        cJSON_AddStringToObject(result, "action", "ready");
    else
        cJSON_AddStringToObject(result, "action", "skip");

    tt_hashmap_free(ecosystems);
    free(path);

    if (out_exit_code)
        *out_exit_code = is_codebase ? 0 : 1;

    return result;
}

int tt_cmd_codebase_detect(tt_cli_opts_t *opts)
{
    int exit_code = 0;
    cJSON *result = tt_cmd_codebase_detect_exec(opts, &exit_code);
    if (!result)
        return tt_output_error("internal_error", tt_error_get(), NULL);
    if (cJSON_GetObjectItemCaseSensitive(result, "error"))
    {
        tt_json_print(result);
        cJSON_Delete(result);
        return exit_code;
    }
    tt_json_print(result);
    cJSON_Delete(result);
    return exit_code;
}
