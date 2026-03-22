/*
 * cmd_search.c -- search:symbols and search:text commands.
 */

#include "cmd_search.h"
#include "arena.h"
#include "json_output.h"
#include "output_fmt.h"
#include "error.h"
#include "platform.h"
#include "database.h"
#include "index_store.h"
#include "storage_paths.h"
#include "text_search.h"
#include "normalizer.h"
#include "symbol_kind.h"
#include "summarizer.h"
#include "str_util.h"
#include "hashmap.h"
#include "token_savings.h"
#include "symbol_scorer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* Parse comma-separated kind list, normalize each, return NULL-terminated array.
 * Returns NULL and sets *invalid_kind (caller-frees) on validation failure.
 * [caller-frees with free()] */
static const char **parse_kinds(const char *kind_str, int *out_count,
                                char **invalid_kind)
{
    *out_count = 0;
    if (invalid_kind) *invalid_kind = NULL;
    if (!kind_str || !kind_str[0])
        return NULL;

    int part_count = 0;
    char **parts = tt_str_split(kind_str, ',', &part_count);
    if (!parts || part_count == 0)
        return NULL;

    const char **kinds = calloc((size_t)(part_count + 1), sizeof(char *));
    if (!kinds)
    {
        tt_str_split_free(parts);
        return NULL;
    }

    int n = 0;
    for (int i = 0; i < part_count; i++)
    {
        char *trimmed = tt_str_trim(parts[i]);
        if (!trimmed[0])
            continue;
        if (!tt_kind_is_valid(trimmed))
        {
            if (invalid_kind) *invalid_kind = tt_strdup(trimmed);
            free(kinds);
            tt_str_split_free(parts);
            return NULL;
        }
        kinds[n++] = tt_normalize_kind(trimmed);
    }
    kinds[n] = NULL;
    *out_count = n;

    tt_str_split_free(parts);
    return kinds;
}

/* Resolve query from --search or positional[0], with warning on both */
static const char *resolve_query(tt_cli_opts_t *opts)
{
    if (opts->search && opts->search[0])
    {
        if (opts->positional_count > 0 && opts->positional[0][0])
        {
            fprintf(stderr, "Warning: both --search and positional query provided. Using --search.\n");
        }
        return opts->search;
    }
    if (opts->positional_count > 0)
        return opts->positional[0];
    return "";
}

/* Format a single symbol result as cJSON for search:symbols output.
 * normal mode: id, name, kind, file, line, [qname], [sig], [summary]
 * compact mode: id, l, [s], [d]  */
/* Detail level: compact < standard (default) < full */
typedef enum {
    DETAIL_COMPACT,
    DETAIL_STANDARD,
    DETAIL_FULL,
} detail_level_e;

static detail_level_e parse_detail(const char *detail, bool compact)
{
    if (detail && detail[0]) {
        if (strcmp(detail, "compact") == 0) return DETAIL_COMPACT;
        if (strcmp(detail, "full") == 0) return DETAIL_FULL;
    }
    if (compact) return DETAIL_COMPACT;
    return DETAIL_STANDARD;
}

static cJSON *format_symbol_result(const tt_symbol_result_t *r,
                                   detail_level_e level,
                                   bool no_sig, bool no_summary,
                                   const char *source)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj)
        return NULL;

    const tt_symbol_t *sym = &r->sym;

    cJSON_AddStringToObject(obj, "id", sym->id ? sym->id : "");

    if (level == DETAIL_COMPACT)
    {
        /* Compact: id, name, kind, file, line, byte_length */
        cJSON_AddStringToObject(obj, "name", sym->name ? sym->name : "");
        cJSON_AddStringToObject(obj, "kind", tt_kind_to_str(sym->kind));
        cJSON_AddStringToObject(obj, "file", sym->file ? sym->file : "");
        cJSON_AddNumberToObject(obj, "line", sym->line);
        if (sym->byte_length > 0)
            cJSON_AddNumberToObject(obj, "byte_length", sym->byte_length);
    }
    else
    {
        /* Standard and Full */
        cJSON_AddStringToObject(obj, "name", sym->name ? sym->name : "");
        cJSON_AddStringToObject(obj, "kind", tt_kind_to_str(sym->kind));
        cJSON_AddStringToObject(obj, "file", sym->file ? sym->file : "");
        cJSON_AddNumberToObject(obj, "line", sym->line);
        if (sym->byte_length > 0)
            cJSON_AddNumberToObject(obj, "byte_length", sym->byte_length);

        /* qname: only if != "" and != name */
        if (sym->qualified_name && sym->qualified_name[0] &&
            (!sym->name || strcmp(sym->qualified_name, sym->name) != 0))
        {
            cJSON_AddStringToObject(obj, "qname", sym->qualified_name);
        }

        /* sig */
        if (!no_sig && sym->signature && sym->signature[0])
        {
            cJSON_AddStringToObject(obj, "sig", sym->signature);
        }

        /* summary (omit tier3 fallback) */
        if (!no_summary && sym->summary && sym->summary[0] &&
            !tt_is_tier3_fallback(sym->summary, sym->kind, sym->name))
        {
            cJSON_AddStringToObject(obj, "summary", sym->summary);
        }

        /* Full: add end_line, docstring, source code */
        if (level == DETAIL_FULL)
        {
            if (sym->end_line > 0)
                cJSON_AddNumberToObject(obj, "end_line", sym->end_line);
            if (sym->docstring && sym->docstring[0])
                cJSON_AddStringToObject(obj, "docstring", sym->docstring);
            if (source)
                cJSON_AddStringToObject(obj, "source", source);
        }
    }

    return obj;
}

/* Sort comparator helpers (receive pointers to tt_symbol_result_t*) */
static int cmp_by_name(const void *a, const void *b)
{
    const tt_symbol_result_t *ra = *(const tt_symbol_result_t *const *)a;
    const tt_symbol_result_t *rb = *(const tt_symbol_result_t *const *)b;
    return tt_strcasecmp(ra->sym.name ? ra->sym.name : "",
                         rb->sym.name ? rb->sym.name : "");
}

static int cmp_by_file(const void *a, const void *b)
{
    const tt_symbol_result_t *ra = *(const tt_symbol_result_t *const *)a;
    const tt_symbol_result_t *rb = *(const tt_symbol_result_t *const *)b;
    int c = strcmp(ra->sym.file ? ra->sym.file : "",
                   rb->sym.file ? rb->sym.file : "");
    if (c != 0)
        return c;
    return (ra->sym.line > rb->sym.line) - (ra->sym.line < rb->sym.line);
}

static int cmp_by_kind(const void *a, const void *b)
{
    const tt_symbol_result_t *ra = *(const tt_symbol_result_t *const *)a;
    const tt_symbol_result_t *rb = *(const tt_symbol_result_t *const *)b;
    int c = strcmp(tt_kind_to_str(ra->sym.kind), tt_kind_to_str(rb->sym.kind));
    if (c != 0)
        return c;
    return tt_strcasecmp(ra->sym.name ? ra->sym.name : "",
                         rb->sym.name ? rb->sym.name : "");
}

static int cmp_by_score_desc(const void *a, const void *b)
{
    const tt_symbol_result_t *ra = *(const tt_symbol_result_t *const *)a;
    const tt_symbol_result_t *rb = *(const tt_symbol_result_t *const *)b;
    if (rb->score > ra->score)
        return 1;
    if (rb->score < ra->score)
        return -1;
    return 0;
}

/* Callback to free file cache entries (file content strings) */
static int free_cache_entry(const char *key, void *value, void *userdata)
{
    (void)key;
    (void)userdata;
    free(value);
    return 0;
}

/* ---- search:symbols ---- */

cJSON *tt_cmd_search_symbols_exec(tt_cli_opts_t *opts)
{
    tt_timer_start();

    /* Resolve project path */
    char *project_path = tt_resolve_project_path(opts->path);
    if (!project_path)
    {
        tt_error_set("Failed to resolve project path");
        return NULL;
    }

    /* Check DB exists */
    if (!tt_database_exists(project_path))
    {
        cJSON *err = make_error("no_index",
                                "No index found.",
                                "Run \"toktoken index:create\" first.");
        free(project_path);
        return err;
    }

    /* Resolve query */
    const char *query = resolve_query(opts);

    /* Parse kinds */
    int kind_count = 0;
    char *invalid_kind = NULL;
    const char **kinds = parse_kinds(opts->kind, &kind_count, &invalid_kind);
    if (invalid_kind)
    {
        cJSON *err = make_error("invalid_kind", "Unknown kind filter value.",
                                "Valid kinds: class, interface, trait, enum, function, "
                                "method, constant, property, variable, namespace, type, "
                                "directive, chapter, section, subsection");
        cJSON_AddStringToObject(err, "invalid_value", invalid_kind);
        free(invalid_kind);
        free(project_path);
        return err;
    }

    /* Cap --max to 200 (but uncap for count mode to get true total) */
    int max = opts->max > 0 ? opts->max : 50;
    if (max > 200)
        max = 200;
    if (opts->count)
        max = 0;

    /* Open database */
    tt_database_t db;
    memset(&db, 0, sizeof(db));
    if (tt_database_open(&db, project_path) < 0)
    {
        free((void *)kinds);
        cJSON *err = make_error("storage_error", "Failed to open database", NULL);
        free(project_path);
        return err;
    }

    /* Init store */
    tt_index_store_t store;
    if (tt_store_init(&store, &db) < 0)
    {
        free((void *)kinds);
        tt_database_close(&db);
        cJSON *err = make_error("storage_error", "Failed to prepare statements", NULL);
        free(project_path);
        return err;
    }

    /* Search */
    tt_search_results_t sr;
    memset(&sr, 0, sizeof(sr));
    if (tt_store_search_symbols(&store, query, kinds, kind_count,
                                opts->language, opts->file_glob,
                                max, opts->regex, &sr) < 0)
    {
        free((void *)kinds);
        tt_store_close(&store);
        tt_database_close(&db);
        cJSON *err = make_error("search_error", tt_error_get(), NULL);
        free(project_path);
        return err;
    }
    free((void *)kinds);

    /* Build scope map if --scope-imports-of or --scope-importers-of */
    tt_hashmap_t *scope_map = NULL;
    if (opts->scope_imports_of && opts->scope_imports_of[0])
    {
        if (opts->scope_importers_of && opts->scope_importers_of[0])
        {
            tt_search_results_free(&sr);
            tt_store_close(&store);
            tt_database_close(&db);
            free(project_path);
            return make_error("invalid_scope",
                              "Cannot use both --scope-imports-of and --scope-importers-of",
                              "Specify only one scope filter");
        }
        char **scope_files = NULL;
        int scope_count = 0;
        tt_store_get_imports_of(&store, opts->scope_imports_of,
                                &scope_files, &scope_count);
        scope_map = tt_hashmap_new(scope_count > 4 ? (size_t)scope_count * 2 : 8);
        /* Include the source file itself */
        tt_hashmap_set(scope_map, opts->scope_imports_of, (void *)1);
        for (int i = 0; i < scope_count; i++)
        {
            tt_hashmap_set(scope_map, scope_files[i], (void *)1);
            free(scope_files[i]);
        }
        free(scope_files);
    }
    else if (opts->scope_importers_of && opts->scope_importers_of[0])
    {
        tt_import_t *imps = NULL;
        int imp_count = 0;
        tt_store_get_importers(&store, opts->scope_importers_of,
                               &imps, &imp_count);
        scope_map = tt_hashmap_new(imp_count > 4 ? (size_t)imp_count * 2 : 8);
        /* Include the target file itself */
        tt_hashmap_set(scope_map, opts->scope_importers_of, (void *)1);
        for (int i = 0; i < imp_count; i++)
        {
            if (imps[i].from_file)
                tt_hashmap_set(scope_map, imps[i].from_file, (void *)1);
        }
        tt_import_array_free(imps, imp_count);
    }

    /* Build filtered result array (apply filter/exclude, unique, sort, limit) */
    /* Step 1: collect passing results into pointer array */
    tt_symbol_result_t **filtered = calloc(sr.count > 0 ? (size_t)sr.count : 1,
                                           sizeof(tt_symbol_result_t *));
    int fcount = 0;

    for (int i = 0; i < sr.count; i++)
    {
        if (!tt_matches_path_filters(sr.results[i].sym.file,
                                     opts->filter, opts->exclude))
            continue;
        if (scope_map && sr.results[i].sym.file &&
            !tt_hashmap_has(scope_map, sr.results[i].sym.file))
            continue;
        filtered[fcount++] = &sr.results[i];
    }

    /* Step 2: --unique (dedup by file:line:name) */
    if (opts->unique && fcount > 0)
    {
        tt_hashmap_t *seen = tt_hashmap_new((size_t)(fcount * 2));
        int new_count = 0;
        tt_strbuf_t keybuf;
        tt_strbuf_init(&keybuf);

        for (int i = 0; i < fcount; i++)
        {
            tt_strbuf_reset(&keybuf);
            tt_strbuf_appendf(&keybuf, "%s:%d:%s",
                              filtered[i]->sym.file ? filtered[i]->sym.file : "",
                              filtered[i]->sym.line,
                              filtered[i]->sym.name ? filtered[i]->sym.name : "");
            if (!tt_hashmap_has(seen, keybuf.data))
            {
                tt_hashmap_set(seen, keybuf.data, (void *)1);
                filtered[new_count++] = filtered[i];
            }
        }
        fcount = new_count;
        tt_strbuf_free(&keybuf);
        tt_hashmap_free(seen);
    }

    /* Step 3: sort */
    if (opts->sort && opts->sort[0])
    {
        int (*cmpfn)(const void *, const void *) = NULL;
        if (strcmp(opts->sort, "name") == 0)
            cmpfn = cmp_by_name;
        else if (strcmp(opts->sort, "file") == 0 || strcmp(opts->sort, "line") == 0)
            cmpfn = cmp_by_file;
        else if (strcmp(opts->sort, "kind") == 0)
            cmpfn = cmp_by_kind;
        else
            cmpfn = cmp_by_score_desc;

        if (fcount > 1)
            qsort(filtered, (size_t)fcount, sizeof(tt_symbol_result_t *), cmpfn);
    }

    /* Step 3b: token budget */
    int tokens_used = 0;
    if (opts->token_budget > 0 && fcount > 0)
    {
        int budget_bytes = opts->token_budget * 4;
        int accumulated = 0, budget_count = 0;
        for (int i = 0; i < fcount; i++)
        {
            int sym_bytes = filtered[i]->sym.byte_length > 0
                            ? filtered[i]->sym.byte_length : 100;
            if (accumulated + sym_bytes > budget_bytes && budget_count > 0)
                break;
            accumulated += sym_bytes;
            budget_count++;
        }
        fcount = budget_count;
        tokens_used = (accumulated + 3) / 4; /* ceil(bytes/4) */
    }

    /* Step 4: limit */
    tt_apply_limit(opts->limit, &fcount);

    /* Compute raw_bytes for savings tracking */
    int64_t raw_bytes = 0;
    {
        const char **sf = calloc(fcount > 0 ? (size_t)fcount : 1, sizeof(char *));
        for (int i = 0; i < fcount; i++)
            sf[i] = filtered[i]->sym.file;
        raw_bytes = tt_savings_raw_from_file_sizes(project_path, sf, fcount);
        free(sf);
    }

    /* Step 5: count mode */
    if (opts->count)
    {
        cJSON *result = NULL;
        if (opts->format && strcmp(opts->format, "table") == 0)
        {
            /* Table: just print the number */
            printf("%d\n", fcount);
            fflush(stdout);
            result = NULL; /* signal: already printed */
        }
        else
        {
            result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "q", query);
            cJSON_AddNumberToObject(result, "count", fcount);
        }

        if (result)
            tt_savings_track(&db, "search_symbols", raw_bytes, result);

        if (scope_map) tt_hashmap_free(scope_map);
        free(filtered);
        tt_search_results_free(&sr);
        tt_store_close(&store);
        tt_database_close(&db);
        free(project_path);
        return result;
    }

    /* Step 6: format results */
    cJSON *results_array = cJSON_CreateArray();
    detail_level_e detail = parse_detail(opts->detail, opts->compact);

    /* For detail=full, cache file contents to extract source code */
    tt_hashmap_t *file_cache = NULL;
    if (detail == DETAIL_FULL)
        file_cache = tt_hashmap_new(64);

    /* For debug mode, precompute query words */
    char *dbg_query_lower = NULL;
    char **dbg_query_words = NULL;
    int dbg_word_count = 0;
    if (opts->debug && query && query[0]) {
        dbg_query_lower = tt_str_tolower(query);
        dbg_query_words = tt_str_split_words(dbg_query_lower, &dbg_word_count);
    }

    for (int i = 0; i < fcount; i++)
    {
        /* Extract source code for full detail mode */
        const char *source = NULL;
        char *source_buf = NULL;
        if (detail == DETAIL_FULL &&
            filtered[i]->sym.byte_offset >= 0 &&
            filtered[i]->sym.byte_length > 0 &&
            filtered[i]->sym.file)
        {
            const char *file_content = tt_hashmap_get(file_cache, filtered[i]->sym.file);
            if (!file_content) {
                char *full = tt_path_join(project_path, filtered[i]->sym.file);
                if (full) {
                    size_t flen = 0;
                    char *data = tt_read_file(full, &flen);
                    free(full);
                    if (data) {
                        tt_hashmap_set(file_cache, filtered[i]->sym.file, data);
                        file_content = data;
                    }
                }
            }
            if (file_content) {
                int off = filtered[i]->sym.byte_offset;
                int len = filtered[i]->sym.byte_length;
                size_t flen = strlen(file_content);
                if (off >= 0 && (size_t)(off + len) <= flen) {
                    source_buf = malloc((size_t)len + 1);
                    if (source_buf) {
                        memcpy(source_buf, file_content + off, (size_t)len);
                        source_buf[len] = '\0';
                        source = source_buf;
                    }
                }
            }
        }

        cJSON *item = format_symbol_result(filtered[i], detail,
                                           opts->no_sig, opts->no_summary,
                                           source);
        free(source_buf);
        if (!item) continue;

        /* Debug mode: add per-field score breakdown */
        if (opts->debug && dbg_query_lower) {
            tt_score_breakdown_t bd;
            tt_score_symbol_debug(
                filtered[i]->sym.name,
                filtered[i]->sym.qualified_name,
                filtered[i]->sym.signature,
                filtered[i]->sym.summary,
                filtered[i]->sym.keywords,
                filtered[i]->sym.docstring,
                dbg_query_lower,
                (const char **)dbg_query_words, dbg_word_count,
                &bd);

            cJSON *dbg = cJSON_CreateObject();
            cJSON_AddNumberToObject(dbg, "name", bd.name_score);
            cJSON_AddNumberToObject(dbg, "sig", bd.signature_score);
            cJSON_AddNumberToObject(dbg, "summary", bd.summary_score);
            cJSON_AddNumberToObject(dbg, "keyword", bd.keyword_score);
            cJSON_AddNumberToObject(dbg, "docstring", bd.docstring_score);
            cJSON_AddNumberToObject(dbg, "total", bd.total);
            cJSON_AddItemToObject(item, "_score", dbg);
        }

        cJSON_AddItemToArray(results_array, item);
    }

    if (dbg_query_words) tt_str_split_free(dbg_query_words);
    free(dbg_query_lower);

    /* Build output based on format */
    cJSON *result = NULL;
    if (opts->format && strcmp(opts->format, "jsonl") == 0)
    {
        tt_output_jsonl(results_array);
        cJSON_Delete(results_array);
        result = NULL; /* already printed */
    }
    else if (opts->format && strcmp(opts->format, "table") == 0)
    {
        /* Table: kind(12), file(30), line(4), name(20), sig(40) */
        const char *columns[] = {"kind", "file", "line", "name", "sig"};
        const int min_widths[] = {12, 30, 4, 20, 40};
        int col_count = 5;

        tt_arena_t *tbl_arena = tt_arena_new(4096);
        const char ***rows = tt_arena_alloc(tbl_arena,
                                            sizeof(const char **) * (fcount > 0 ? (size_t)fcount : 1));
        if (rows) memset(rows, 0, sizeof(const char **) * (fcount > 0 ? (size_t)fcount : 1));

        for (int i = 0; i < fcount && rows; i++)
        {
            rows[i] = tt_arena_alloc(tbl_arena, 5 * sizeof(const char *));
            if (!rows[i]) break;
            memset(rows[i], 0, 5 * sizeof(const char *));
            rows[i][0] = tt_kind_to_str(filtered[i]->sym.kind);
            rows[i][1] = filtered[i]->sym.file ? filtered[i]->sym.file : "";
            char *lbuf = tt_arena_alloc(tbl_arena, 16);
            if (lbuf) snprintf(lbuf, 16, "%d", filtered[i]->sym.line);
            rows[i][2] = lbuf ? lbuf : "";
            rows[i][3] = filtered[i]->sym.name ? filtered[i]->sym.name : "";
            rows[i][4] = filtered[i]->sym.signature ? filtered[i]->sym.signature : "";
        }

        tt_render_table(columns, min_widths, col_count, rows, fcount,
                        opts->truncate_width);

        tt_arena_free(tbl_arena);
        cJSON_Delete(results_array);
        result = NULL; /* already printed */
    }
    else
    {
        /* JSON (default): {"q": query, "results": [...], "n": count} */
        result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "q", query);
        cJSON_AddItemToObject(result, "results", results_array);
        cJSON_AddNumberToObject(result, "n", fcount);

        if (opts->token_budget > 0)
        {
            cJSON_AddNumberToObject(result, "token_budget", opts->token_budget);
            cJSON_AddNumberToObject(result, "tokens_used", tokens_used);
            cJSON_AddNumberToObject(result, "tokens_remaining",
                                    opts->token_budget - tokens_used);
        }
    }

    if (result)
        tt_savings_track(&db, "search_symbols", raw_bytes, result);

    if (file_cache) {
        tt_hashmap_iter(file_cache, free_cache_entry, NULL);
        tt_hashmap_free(file_cache);
    }
    if (scope_map) tt_hashmap_free(scope_map);
    free(filtered);
    tt_search_results_free(&sr);
    tt_store_close(&store);
    tt_database_close(&db);
    free(project_path);

    return result;
}

/* ---- search:text ---- */

cJSON *tt_cmd_search_text_exec(tt_cli_opts_t *opts)
{
    tt_timer_start();

    /* Resolve project path */
    char *project_path = tt_resolve_project_path(opts->path);
    if (!project_path)
    {
        tt_error_set("Failed to resolve project path");
        return NULL;
    }

    /* Check DB exists */
    if (!tt_database_exists(project_path))
    {
        cJSON *err = make_error("no_index",
                                "No index found.",
                                "Run \"toktoken index:create\" first.");
        free(project_path);
        return err;
    }

    /* Resolve query */
    const char *query_raw = resolve_query(opts);
    if (!query_raw || !query_raw[0])
    {
        cJSON *err = make_error("empty_query",
                                "Query is required.",
                                "Pass a search query as argument or via --search option.");
        free(project_path);
        return err;
    }

    /* Parse queries: split on | (literal mode only), trim, filter empty.
     * In regex mode, | is alternation — pass the whole query through. */
    int query_part_count = 0;
    char **query_parts = NULL;
    const char **queries = NULL;
    int actual_query_count = 0;

    if (opts->regex)
    {
        /* Regex mode: single query, no pipe splitting */
        queries = calloc(2, sizeof(char *));
        if (queries)
        {
            queries[0] = query_raw;
            actual_query_count = 1;
        }
    }
    else
    {
        query_parts = tt_str_split(query_raw, '|', &query_part_count);
        if (query_parts)
        {
            queries = calloc((size_t)(query_part_count + 1), sizeof(char *));
            for (int i = 0; i < query_part_count; i++)
            {
                char *trimmed = tt_str_trim(query_parts[i]);
                if (trimmed[0])
                    queries[actual_query_count++] = trimmed;
            }
        }
    }

    if (actual_query_count == 0)
    {
        free(queries);
        tt_str_split_free(query_parts);
        cJSON *err = make_error("empty_query",
                                "Query is required.",
                                "Pass a search query as argument or via --search option.");
        free(project_path);
        return err;
    }

    /* Open database */
    tt_database_t db;
    memset(&db, 0, sizeof(db));
    if (tt_database_open(&db, project_path) < 0)
    {
        free(queries);
        tt_str_split_free(query_parts);
        cJSON *err = make_error("storage_error", "Failed to open database", NULL);
        free(project_path);
        return err;
    }

    /* Init store */
    tt_index_store_t store;
    if (tt_store_init(&store, &db) < 0)
    {
        free(queries);
        tt_str_split_free(query_parts);
        tt_database_close(&db);
        cJSON *err = make_error("storage_error", "Failed to prepare statements", NULL);
        free(project_path);
        return err;
    }

    /* Get all indexed files */
    tt_file_record_t *all_files = NULL;
    int file_count = 0;
    if (tt_store_get_all_files(&store, &all_files, &file_count) < 0)
    {
        free(queries);
        tt_str_split_free(query_parts);
        tt_store_close(&store);
        tt_database_close(&db);
        cJSON *err = make_error("storage_error", "Failed to get file list", NULL);
        free(project_path);
        return err;
    }

    /* Filter files by --file glob and --filter/--exclude */
    const char **search_paths = calloc(file_count > 0 ? (size_t)file_count : 1,
                                       sizeof(char *));
    int search_path_count = 0;

    for (int i = 0; i < file_count; i++)
    {
        const char *path = all_files[i].path;

        /* --file glob filter */
        if (opts->file_glob && opts->file_glob[0])
        {
            if (!tt_fnmatch_ex(opts->file_glob, path,
                               TT_FNM_CASEFOLD | TT_FNM_PATHNAME))
                continue;
        }

        /* --filter / --exclude */
        if (!tt_matches_path_filters(path, opts->filter, opts->exclude))
            continue;

        search_paths[search_path_count++] = path;
    }

    /* Text search options (uncap for count mode to get true total) */
    int max_results = opts->max > 0 ? opts->max : 100;
    if (max_results > 500)
        max_results = 500;
    if (opts->count)
        max_results = 100000;

    int context_lines = opts->context;
    if (context_lines < 0)
        context_lines = 0;
    if (context_lines > 10)
        context_lines = 10;

    tt_text_search_opts_t search_opts = {
        .case_sensitive = opts->case_sensitive,
        .is_regex = opts->regex,
        .max_results = max_results,
        .context_lines = context_lines,
    };

    /* Execute text search */
    tt_text_results_t text_results;
    memset(&text_results, 0, sizeof(text_results));

    if (search_path_count > 0)
    {
        if (tt_text_search(project_path, search_paths, search_path_count,
                           queries, actual_query_count,
                           &search_opts, &text_results) < 0)
        {
            free(search_paths);
            for (int i = 0; i < file_count; i++)
                tt_file_record_free(&all_files[i]);
            free(all_files);
            free(queries);
            tt_str_split_free(query_parts);
            tt_store_close(&store);
            tt_database_close(&db);
            cJSON *err = make_error("search_error", tt_error_get(), NULL);
            free(project_path);
            return err;
        }
    }
    free(search_paths);

    /* Done with store (keep db open for savings tracking) */
    for (int i = 0; i < file_count; i++)
        tt_file_record_free(&all_files[i]);
    free(all_files);
    free(queries);
    tt_str_split_free(query_parts);
    tt_store_close(&store);

    /* Compute raw_bytes for savings tracking */
    int64_t raw_bytes = 0;
    {
        const char **sf = calloc(text_results.count > 0
                                     ? (size_t)text_results.count
                                     : 1,
                                 sizeof(char *));
        int sf_count = 0;
        for (int i = 0; i < text_results.count; i++)
            if (text_results.results[i].file)
                sf[sf_count++] = text_results.results[i].file;
        raw_bytes = tt_savings_raw_from_file_sizes(project_path, sf, sf_count);
        free(sf);
    }

    /* Apply --limit */
    int result_count = text_results.count;
    tt_apply_limit(opts->limit, &result_count);

    /* Count mode */
    if (opts->count)
    {
        cJSON *result = NULL;
        if (opts->format && strcmp(opts->format, "table") == 0)
        {
            printf("%d\n", result_count);
            fflush(stdout);
        }
        else
        {
            result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "q", query_raw);
            cJSON_AddNumberToObject(result, "count", result_count);
        }
        if (result)
            tt_savings_track(&db, "search_text", raw_bytes, result);
        tt_database_close(&db);
        tt_text_results_free(&text_results);
        free(project_path);
        return result;
    }

    /* Group-by file mode */
    if (opts->group_by && strcmp(opts->group_by, "file") == 0)
    {
        /* Collect unique files and their hit counts from results */
        /* We use arrays + hashmap for dedup */
        tt_hashmap_t *seen = tt_hashmap_new(128);
        const char **file_names = calloc(result_count > 0 ? (size_t)result_count : 1,
                                         sizeof(char *));
        int *counts = calloc(result_count > 0 ? (size_t)result_count : 1, sizeof(int));
        int group_count = 0;

        for (int i = 0; i < result_count; i++)
        {
            const char *f = text_results.results[i].file;
            if (!f)
                continue;

            void *idx_ptr = tt_hashmap_get(seen, f);
            if (idx_ptr)
            {
                /* Already seen, increment count */
                int idx = (int)(intptr_t)idx_ptr - 1; /* stored as 1-based */
                counts[idx]++;
            }
            else
            {
                /* New file */
                file_names[group_count] = f;
                counts[group_count] = 1;
                tt_hashmap_set(seen, f, (void *)(intptr_t)(group_count + 1));
                group_count++;
            }
        }
        tt_hashmap_free(seen);

        /* Sort by count DESC (simple insertion sort) */
        for (int i = 1; i < group_count; i++)
        {
            for (int j = i; j > 0 && counts[j] > counts[j - 1]; j--)
            {
                int tc = counts[j];
                counts[j] = counts[j - 1];
                counts[j - 1] = tc;
                const char *tf = file_names[j];
                file_names[j] = file_names[j - 1];
                file_names[j - 1] = tf;
            }
        }

        if (opts->format && strcmp(opts->format, "table") == 0)
        {
            const char *columns[] = {"hits", "file"};
            const int min_widths[] = {4, 40};

            tt_arena_t *tbl_arena = tt_arena_new(4096);
            size_t gcz = group_count > 0 ? (size_t)group_count : 1;
            const char ***rows = tt_arena_alloc(tbl_arena, sizeof(const char **) * gcz);
            if (rows) memset(rows, 0, sizeof(const char **) * gcz);

            for (int i = 0; i < group_count && rows; i++)
            {
                rows[i] = tt_arena_alloc(tbl_arena, 2 * sizeof(const char *));
                if (!rows[i]) break;
                memset(rows[i], 0, 2 * sizeof(const char *));
                char *cbuf = tt_arena_alloc(tbl_arena, 16);
                if (cbuf) snprintf(cbuf, 16, "%d", counts[i]);
                rows[i][0] = cbuf ? cbuf : "";
                rows[i][1] = file_names[i];
            }

            tt_render_table(columns, min_widths, 2, rows, group_count,
                            opts->truncate_width);

            tt_arena_free(tbl_arena);
            free(file_names);
            free(counts);
            tt_text_results_free(&text_results);
            tt_database_close(&db);
            free(project_path);
            return NULL; /* already printed */
        }

        /* JSON group-by */
        cJSON *groups_json = cJSON_CreateObject();
        int total_hits = 0;

        for (int i = 0; i < group_count; i++)
        {
            cJSON_AddNumberToObject(groups_json, file_names[i], counts[i]);
            total_hits += counts[i];
        }

        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "q", query_raw);
        cJSON_AddItemToObject(result, "groups", groups_json);
        cJSON_AddNumberToObject(result, "files", group_count);
        cJSON_AddNumberToObject(result, "total_hits", total_hits);

        free(file_names);
        free(counts);
        tt_text_results_free(&text_results);
        tt_savings_track(&db, "search_text", raw_bytes, result);
        tt_database_close(&db);
        free(project_path);
        return result;
    }

    /* Standard output */
    cJSON *results_array = cJSON_CreateArray();
    for (int i = 0; i < result_count; i++)
    {
        tt_text_result_t *tr = &text_results.results[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "f", tr->file ? tr->file : "");
        cJSON_AddNumberToObject(item, "l", tr->line);
        cJSON_AddStringToObject(item, "t", tr->text ? tr->text : "");

        if (tr->before && tr->before_count > 0)
        {
            cJSON *before = cJSON_CreateArray();
            for (int j = 0; j < tr->before_count; j++)
                cJSON_AddItemToArray(before, cJSON_CreateString(tr->before[j] ? tr->before[j] : ""));
            cJSON_AddItemToObject(item, "before", before);
        }
        if (tr->after && tr->after_count > 0)
        {
            cJSON *after = cJSON_CreateArray();
            for (int j = 0; j < tr->after_count; j++)
                cJSON_AddItemToArray(after, cJSON_CreateString(tr->after[j] ? tr->after[j] : ""));
            cJSON_AddItemToObject(item, "after", after);
        }

        cJSON_AddItemToArray(results_array, item);
    }

    cJSON *result = NULL;
    if (opts->format && strcmp(opts->format, "jsonl") == 0)
    {
        tt_output_jsonl(results_array);
        cJSON_Delete(results_array);
        result = NULL;
    }
    else if (opts->format && strcmp(opts->format, "table") == 0)
    {
        const char *columns[] = {"f", "l", "t"};
        const int min_widths[] = {40, 4, 60};

        tt_arena_t *tbl_arena = tt_arena_new(4096);
        size_t rcz = result_count > 0 ? (size_t)result_count : 1;
        const char ***rows = tt_arena_alloc(tbl_arena, sizeof(const char **) * rcz);
        if (rows) memset(rows, 0, sizeof(const char **) * rcz);

        for (int i = 0; i < result_count && rows; i++)
        {
            rows[i] = tt_arena_alloc(tbl_arena, 3 * sizeof(const char *));
            if (!rows[i]) break;
            memset(rows[i], 0, 3 * sizeof(const char *));
            rows[i][0] = text_results.results[i].file ? text_results.results[i].file : "";
            char *lbuf = tt_arena_alloc(tbl_arena, 16);
            if (lbuf) snprintf(lbuf, 16, "%d", text_results.results[i].line);
            rows[i][1] = lbuf ? lbuf : "";
            rows[i][2] = text_results.results[i].text ? text_results.results[i].text : "";
        }

        tt_render_table(columns, min_widths, 3, rows, result_count,
                        opts->truncate_width);

        tt_arena_free(tbl_arena);
        cJSON_Delete(results_array);
        result = NULL;
    }
    else
    {
        /* JSON default */
        result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "q", query_raw);
        cJSON_AddItemToObject(result, "results", results_array);
        cJSON_AddNumberToObject(result, "n", result_count);
    }

    if (result)
        tt_savings_track(&db, "search_text", raw_bytes, result);
    tt_database_close(&db);
    tt_text_results_free(&text_results);
    free(project_path);
    return result;
}

/* ---- CLI wrappers ---- */

int tt_cmd_search_symbols(tt_cli_opts_t *opts)
{
    cJSON *result = tt_cmd_search_symbols_exec(opts);
    if (!result)
    {
        /* NULL can mean: already printed (count table, jsonl), or internal error */
        const char *err = tt_error_get();
        if (err && err[0])
            return tt_output_error("internal_error", err, NULL);
        return 0;
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

int tt_cmd_search_text(tt_cli_opts_t *opts)
{
    cJSON *result = tt_cmd_search_text_exec(opts);
    if (!result)
    {
        const char *err = tt_error_get();
        if (err && err[0])
            return tt_output_error("internal_error", err, NULL);
        return 0;
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

/* ---- search:cooccurrence ---- */

cJSON *tt_cmd_search_cooccurrence_exec(tt_cli_opts_t *opts)
{
    const char *query = resolve_query(opts);
    if (!query || !query[0])
    {
        return make_error("missing_argument",
                           "Usage: search:cooccurrence <name_a>,<name_b>",
                           "Specify two comma-separated symbol names");
    }

    /* Split query on comma -> name_a, name_b */
    char *query_dup = tt_strdup(query);
    char *comma = strchr(query_dup, ',');
    if (!comma)
    {
        free(query_dup);
        return make_error("invalid_argument",
                           "Expected two comma-separated names",
                           "Example: search:cooccurrence Logger,Database");
    }
    *comma = '\0';
    const char *name_a = query_dup;
    const char *name_b = comma + 1;

    char *project_path = tt_resolve_project_path(opts->path);
    if (!project_path)
    {
        free(query_dup);
        tt_error_set("Failed to resolve project path");
        return NULL;
    }

    tt_database_t db;
    memset(&db, 0, sizeof(db));
    if (tt_database_open(&db, project_path) < 0)
    {
        free(query_dup);
        free(project_path);
        return make_error("storage_error", "Failed to open database", NULL);
    }

    tt_index_store_t store;
    if (tt_store_init(&store, &db) < 0)
    {
        tt_database_close(&db);
        free(query_dup);
        free(project_path);
        return make_error("storage_error", "Failed to prepare statements", NULL);
    }

    int limit = opts->limit > 0 ? opts->limit : 50;
    tt_cooccurrence_t *items = NULL;
    int count = 0;
    tt_store_search_cooccurrence(&store, name_a, name_b,
                                  opts->language, limit, &items, &count);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "name_a", name_a);
    cJSON_AddStringToObject(result, "name_b", name_b);
    cJSON_AddNumberToObject(result, "n", count);
    cJSON *arr = cJSON_AddArrayToObject(result, "results");
    for (int i = 0; i < count; i++)
    {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "file", items[i].file);
        cJSON *a = cJSON_AddObjectToObject(obj, "a");
        cJSON_AddStringToObject(a, "name", items[i].name_a);
        cJSON_AddStringToObject(a, "kind", items[i].kind_a);
        cJSON_AddNumberToObject(a, "line", items[i].line_a);
        cJSON *b = cJSON_AddObjectToObject(obj, "b");
        cJSON_AddStringToObject(b, "name", items[i].name_b);
        cJSON_AddStringToObject(b, "kind", items[i].kind_b);
        cJSON_AddNumberToObject(b, "line", items[i].line_b);
        cJSON_AddItemToArray(arr, obj);
    }

    tt_cooccurrence_free(items, count);
    tt_store_close(&store);
    tt_database_close(&db);
    free(query_dup);
    free(project_path);
    return result;
}

int tt_cmd_search_cooccurrence(tt_cli_opts_t *opts)
{
    cJSON *result = tt_cmd_search_cooccurrence_exec(opts);
    if (!result)
    {
        const char *err = tt_error_get();
        if (err && err[0])
            return tt_output_error("internal_error", err, NULL);
        return 0;
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

/* ---- search:similar ---- */

cJSON *tt_cmd_search_similar_exec(tt_cli_opts_t *opts)
{
    const char *query = resolve_query(opts);
    if (!query || !query[0])
    {
        return make_error("missing_argument",
                           "Usage: search:similar <symbol-id>",
                           "Specify a symbol ID to find similar symbols");
    }

    char *project_path = tt_resolve_project_path(opts->path);
    if (!project_path)
    {
        tt_error_set("Failed to resolve project path");
        return NULL;
    }

    tt_database_t db;
    memset(&db, 0, sizeof(db));
    if (tt_database_open(&db, project_path) < 0)
    {
        free(project_path);
        return make_error("storage_error", "Failed to open database", NULL);
    }

    tt_index_store_t store;
    if (tt_store_init(&store, &db) < 0)
    {
        tt_database_close(&db);
        free(project_path);
        return make_error("storage_error", "Failed to prepare statements", NULL);
    }

    int limit = opts->limit > 0 ? opts->limit : 20;
    tt_search_results_t sr;
    tt_store_search_similar(&store, query, limit, &sr);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "q", query);
    cJSON_AddNumberToObject(result, "n", sr.count);
    cJSON *arr = cJSON_AddArrayToObject(result, "results");
    for (int i = 0; i < sr.count; i++)
    {
        cJSON *obj = cJSON_CreateObject();
        tt_symbol_result_t *r = &sr.results[i];
        cJSON_AddStringToObject(obj, "id", r->sym.id);
        cJSON_AddStringToObject(obj, "name", r->sym.name);
        cJSON_AddStringToObject(obj, "kind", tt_kind_to_str(r->sym.kind));
        cJSON_AddStringToObject(obj, "file", r->sym.file);
        cJSON_AddNumberToObject(obj, "line", r->sym.line);
        if (r->sym.signature && r->sym.signature[0])
            cJSON_AddStringToObject(obj, "sig", r->sym.signature);
        if (r->sym.summary && r->sym.summary[0])
            cJSON_AddStringToObject(obj, "summary", r->sym.summary);
        cJSON_AddItemToArray(arr, obj);
    }

    tt_search_results_free(&sr);
    tt_store_close(&store);
    tt_database_close(&db);
    free(project_path);
    return result;
}

int tt_cmd_search_similar(tt_cli_opts_t *opts)
{
    cJSON *result = tt_cmd_search_similar_exec(opts);
    if (!result)
    {
        const char *err = tt_error_get();
        if (err && err[0])
            return tt_output_error("internal_error", err, NULL);
        return 0;
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
