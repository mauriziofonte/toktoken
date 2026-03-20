/*
 * cmd_inspect.c -- inspect:outline, inspect:symbol, inspect:file, inspect:tree.
 */

#include "cmd_inspect.h"
#include "arena.h"
#include "json_output.h"
#include "output_fmt.h"
#include "error.h"
#include "platform.h"
#include "database.h"
#include "index_store.h"
#include "normalizer.h"
#include "symbol_kind.h"
#include "summarizer.h"
#include "str_util.h"
#include "hashmap.h"
#include "fast_hash.h"
#include "token_savings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

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

/* Normalize path in-place: backslash -> forward slash */
static char *normalize_path(const char *input)
{
    if (!input || input[0] == '\0')
        return NULL;

    /* Reject path traversal and absolute paths */
    if (strstr(input, "..") != NULL)
        return NULL;
    if (input[0] == '/' || input[0] == '\\')
        return NULL;
#ifdef TT_PLATFORM_WINDOWS
    if (input[0] && input[1] == ':')
        return NULL;
#endif

    char *copy = tt_strdup(input);
    if (copy)
        tt_path_normalize_sep(copy);
    return copy;
}

/* Parse comma-separated kind list, normalize each.
 * Returns NULL and sets *invalid_kind (caller-frees) on validation failure. */
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

/* Check if a kind string matches any in the filter list */
static bool kind_matches(const char *kind_str, const char **kinds, int kind_count)
{
    if (!kinds || kind_count <= 0)
        return true;
    for (int i = 0; i < kind_count; i++)
    {
        if (tt_strcasecmp(kind_str, kinds[i]) == 0)
            return true;
    }
    return false;
}

/* ================================================================
 * inspect:outline
 * ================================================================ */

/* Tree node for outline hierarchy */
typedef struct outline_node
{
    int sym_index; /* index into symbols array */
    struct outline_node **children;
    int child_count;
    int child_cap;
    bool is_root;
} outline_node_t;

static void outline_node_add_child(outline_node_t *parent, outline_node_t *child)
{
    if (parent->child_count >= parent->child_cap)
    {
        int new_cap = parent->child_cap ? parent->child_cap * 2 : 4;
        outline_node_t **tmp = realloc(parent->children,
                                       (size_t)new_cap * sizeof(outline_node_t *));
        if (!tmp) return;
        parent->children = tmp;
        parent->child_cap = new_cap;
    }
    parent->children[parent->child_count++] = child;
}

static void outline_nodes_free(outline_node_t *nodes, int count)
{
    for (int i = 0; i < count; i++)
        free(nodes[i].children);
    free(nodes);
}

static cJSON *build_outline_json(outline_node_t *node,
                                 const tt_symbol_result_t *syms,
                                 bool compact, bool no_sig, bool no_summary)
{
    const tt_symbol_t *sym = &syms[node->sym_index].sym;
    cJSON *obj = cJSON_CreateObject();

    if (compact)
    {
        cJSON_AddStringToObject(obj, "k", tt_kind_to_str(sym->kind));
        cJSON_AddStringToObject(obj, "n", sym->name ? sym->name : "");
        cJSON_AddNumberToObject(obj, "l", sym->line);
        if (sym->end_line > sym->line)
            cJSON_AddNumberToObject(obj, "e", sym->end_line);
        if (!no_sig && sym->signature && sym->signature[0])
            cJSON_AddStringToObject(obj, "s", sym->signature);
        if (!no_summary && sym->summary && sym->summary[0] &&
            !tt_is_tier3_fallback(sym->summary, sym->kind, sym->name))
            cJSON_AddStringToObject(obj, "d", sym->summary);
    }
    else
    {
        cJSON_AddStringToObject(obj, "kind", tt_kind_to_str(sym->kind));
        cJSON_AddStringToObject(obj, "name", sym->name ? sym->name : "");
        cJSON_AddNumberToObject(obj, "line", sym->line);
        if (sym->end_line > sym->line)
            cJSON_AddNumberToObject(obj, "end", sym->end_line);
        if (!no_sig && sym->signature && sym->signature[0])
            cJSON_AddStringToObject(obj, "sig", sym->signature);
        if (!no_summary && sym->summary && sym->summary[0] &&
            !tt_is_tier3_fallback(sym->summary, sym->kind, sym->name))
            cJSON_AddStringToObject(obj, "summary", sym->summary);
    }

    /* Add children recursively */
    if (node->child_count > 0)
    {
        const char *children_key = compact ? "c" : "children";
        cJSON *children = cJSON_CreateArray();
        for (int i = 0; i < node->child_count; i++)
        {
            cJSON *child = build_outline_json(node->children[i], syms,
                                              compact, no_sig, no_summary);
            if (child)
                cJSON_AddItemToArray(children, child);
        }
        cJSON_AddItemToObject(obj, children_key, children);
    }

    return obj;
}

cJSON *tt_cmd_inspect_outline_exec(tt_cli_opts_t *opts)
{
    /* File path is required */
    if (opts->positional_count < 1 || !opts->positional[0][0])
    {
        return make_error("missing_argument",
                          "File path is required.",
                          "Usage: toktoken inspect:outline <file>");
    }

    /* Resolve project path */
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

    /* Normalize file path */
    char *file_path = normalize_path(opts->positional[0]);
    if (!file_path)
    {
        cJSON *err = make_error("invalid_path",
                                "Invalid file path (absolute paths and path traversal are not allowed).",
                                NULL);
        free(project_path);
        return err;
    }

    /* Open database */
    tt_database_t db;
    memset(&db, 0, sizeof(db));
    if (tt_database_open(&db, project_path) < 0)
    {
        free(file_path);
        cJSON *err = make_error("storage_error", "Failed to open database", NULL);
        free(project_path);
        return err;
    }

    tt_index_store_t store;
    if (tt_store_init(&store, &db) < 0)
    {
        free(file_path);
        tt_database_close(&db);
        cJSON *err = make_error("storage_error", "Failed to prepare statements", NULL);
        free(project_path);
        return err;
    }

    /* Check file exists in index */
    tt_file_record_t file_rec;
    memset(&file_rec, 0, sizeof(file_rec));
    if (tt_store_get_file(&store, file_path, &file_rec) < 0)
    {
        char msg[512];
        snprintf(msg, sizeof(msg), "File not in index: %s", file_path);
        free(file_path);
        tt_store_close(&store);
        tt_database_close(&db);
        cJSON *err = make_error("file_not_found", msg,
                                "Run \"toktoken index:update\" to sync.");
        free(project_path);
        return err;
    }

    /* Get symbols for file (ordered by line ASC) */
    tt_symbol_result_t *syms = NULL;
    int sym_count = 0;
    tt_store_get_symbols_by_file(&store, file_path, &syms, &sym_count);

    /* Parse kind filter */
    int kind_count = 0;
    char *invalid_kind = NULL;
    const char **kinds = parse_kinds(opts->kind, &kind_count, &invalid_kind);
    if (invalid_kind)
    {
        free(syms);
        free(file_path);
        tt_store_close(&store);
        tt_database_close(&db);
        cJSON *err = make_error("invalid_kind", "Unknown kind filter value.",
                                "Valid kinds: class, interface, trait, enum, function, "
                                "method, constant, property, variable, namespace, type, directive");
        cJSON_AddStringToObject(err, "invalid_value", invalid_kind);
        free(invalid_kind);
        free(project_path);
        return err;
    }

    /* Filter by kind */
    tt_symbol_result_t **filtered = calloc(sym_count > 0 ? (size_t)sym_count : 1,
                                           sizeof(tt_symbol_result_t *));
    int fcount = 0;
    for (int i = 0; i < sym_count; i++)
    {
        if (kind_matches(tt_kind_to_str(syms[i].sym.kind), kinds, kind_count))
            filtered[fcount++] = &syms[i];
    }
    free((void *)kinds);

    /* Apply limit */
    tt_apply_limit(opts->limit, &fcount);

    /* Format output */
    cJSON *result = NULL;

    if (opts->format && strcmp(opts->format, "table") == 0)
    {
        /* Flat table */
        int col_count = 3;
        const char *base_cols[] = {"kind", "line", "name", "sig", "summary"};
        const int base_widths[] = {12, 4, 25, 40, 30};

        if (!opts->no_sig)
            col_count++;
        if (!opts->no_summary)
            col_count++;

        tt_arena_t *tbl_arena = tt_arena_new(4096);

        const char **columns = tt_arena_alloc(tbl_arena, (size_t)col_count * sizeof(char *));
        int *min_widths = tt_arena_alloc(tbl_arena, (size_t)col_count * sizeof(int));
        if (columns) memset(columns, 0, (size_t)col_count * sizeof(char *));
        if (min_widths) memset(min_widths, 0, (size_t)col_count * sizeof(int));

        int ci = 0;
        if (columns && min_widths)
        {
            columns[ci] = base_cols[0];
            min_widths[ci] = base_widths[0];
            ci++;
            columns[ci] = base_cols[1];
            min_widths[ci] = base_widths[1];
            ci++;
            columns[ci] = base_cols[2];
            min_widths[ci] = base_widths[2];
            ci++;
            if (!opts->no_sig)
            {
                columns[ci] = base_cols[3];
                min_widths[ci] = base_widths[3];
                ci++;
            }
            if (!opts->no_summary)
            {
                columns[ci] = base_cols[4];
                min_widths[ci] = base_widths[4];
                ci++;
            }
        }

        size_t fcz = fcount > 0 ? (size_t)fcount : 1;
        const char ***rows = tt_arena_alloc(tbl_arena, sizeof(const char **) * fcz);
        if (rows) memset(rows, 0, sizeof(const char **) * fcz);

        for (int i = 0; i < fcount && rows && columns; i++)
        {
            rows[i] = tt_arena_alloc(tbl_arena, (size_t)col_count * sizeof(const char *));
            if (!rows[i]) break;
            memset(rows[i], 0, (size_t)col_count * sizeof(const char *));
            int c = 0;
            rows[i][c++] = tt_kind_to_str(filtered[i]->sym.kind);
            char *lbuf = tt_arena_alloc(tbl_arena, 16);
            if (lbuf) snprintf(lbuf, 16, "%d", filtered[i]->sym.line);
            rows[i][c++] = lbuf ? lbuf : "";
            rows[i][c++] = filtered[i]->sym.name ? filtered[i]->sym.name : "";
            if (!opts->no_sig)
                rows[i][c++] = filtered[i]->sym.signature ? filtered[i]->sym.signature : "";
            if (!opts->no_summary)
            {
                const char *sum = filtered[i]->sym.summary;
                if (sum && sum[0] &&
                    !tt_is_tier3_fallback(sum, filtered[i]->sym.kind,
                                          filtered[i]->sym.name))
                    rows[i][c++] = sum;
                else
                    rows[i][c++] = "";
            }
        }

        tt_render_table(columns, min_widths, col_count, rows, fcount,
                        opts->truncate_width);

        tt_arena_free(tbl_arena);
        result = NULL; /* already printed */
    }
    else if (opts->format && strcmp(opts->format, "jsonl") == 0)
    {
        /* Flat JSONL */
        cJSON *arr = cJSON_CreateArray();
        for (int i = 0; i < fcount; i++)
        {
            const tt_symbol_t *sym = &filtered[i]->sym;
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "kind", tt_kind_to_str(sym->kind));
            cJSON_AddStringToObject(item, "name", sym->name ? sym->name : "");
            cJSON_AddNumberToObject(item, "line", sym->line);
            if (!opts->no_sig && sym->signature && sym->signature[0])
                cJSON_AddStringToObject(item, "sig", sym->signature);
            if (!opts->no_summary && sym->summary && sym->summary[0] &&
                !tt_is_tier3_fallback(sym->summary, sym->kind, sym->name))
                cJSON_AddStringToObject(item, "summary", sym->summary);
            cJSON_AddItemToArray(arr, item);
        }
        tt_output_jsonl(arr);
        cJSON_Delete(arr);
        result = NULL;
    }
    else
    {
        /* JSON (hierarchical) */
        /* Build ID -> index map for parent resolution */
        tt_hashmap_t *by_id = tt_hashmap_new(sym_count > 0 ? (size_t)(sym_count * 2) : 16);

        /* Create nodes for filtered symbols */
        outline_node_t *nodes = calloc(fcount > 0 ? (size_t)fcount : 1,
                                       sizeof(outline_node_t));
        /* We need to map filtered indices by their original sym pointers */
        for (int i = 0; i < fcount; i++)
        {
            nodes[i].sym_index = (int)(filtered[i] - syms);
            if (filtered[i]->sym.id)
                tt_hashmap_set(by_id, filtered[i]->sym.id, &nodes[i]);
        }

        /* Pass 1: build parent-child relationships */
        for (int i = 0; i < fcount; i++)
        {
            const char *pid = filtered[i]->sym.parent_id;
            outline_node_t *parent_node = NULL;
            if (pid && pid[0])
                parent_node = tt_hashmap_get(by_id, pid);

            if (parent_node)
                outline_node_add_child(parent_node, &nodes[i]);
            else
                nodes[i].is_root = true;
        }

        /* Pass 2: render root nodes (all children now attached) */
        cJSON *roots = cJSON_CreateArray();
        for (int i = 0; i < fcount; i++)
        {
            if (nodes[i].is_root)
            {
                cJSON *node_json = build_outline_json(&nodes[i], syms,
                                                      opts->compact,
                                                      opts->no_sig,
                                                      opts->no_summary);
                if (node_json)
                    cJSON_AddItemToArray(roots, node_json);
            }
        }

        result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "file", file_path);
        if (file_rec.language && file_rec.language[0])
            cJSON_AddStringToObject(result, "lang", file_rec.language);
        cJSON_AddItemToObject(result, "symbols", roots);

        outline_nodes_free(nodes, fcount);
        tt_hashmap_free(by_id);
    }

    /* Track savings */
    if (result)
    {
        char *full = tt_path_join(project_path, file_path);
        int64_t raw_bytes = tt_savings_raw_from_file(full);
        free(full);
        tt_savings_track(&db, "inspect_outline", raw_bytes, result);
    }

    free(filtered);
    for (int i = 0; i < sym_count; i++)
        tt_symbol_result_free(&syms[i]);
    free(syms);
    tt_file_record_free(&file_rec);
    free(file_path);
    tt_store_close(&store);
    tt_database_close(&db);
    free(project_path);

    return result;
}

/* ================================================================
 * inspect:symbol
 * ================================================================ */

cJSON *tt_cmd_inspect_symbol_exec(tt_cli_opts_t *opts, int *out_exit_code)
{
    *out_exit_code = 0;

    if (opts->positional_count < 1)
    {
        *out_exit_code = 1;
        return make_error("missing_argument",
                          "Symbol ID is required.",
                          "Usage: toktoken inspect:symbol <id> [id2 ...]");
    }

    /* Resolve project path */
    char *project_path = tt_resolve_project_path(opts->path);
    if (!project_path)
    {
        *out_exit_code = 1;
        tt_error_set("Failed to resolve project path");
        return NULL;
    }

    if (!tt_database_exists(project_path))
    {
        *out_exit_code = 1;
        cJSON *err = make_error("no_index", "No index found.",
                                "Run \"toktoken index:create\" first.");
        free(project_path);
        return err;
    }

    /* Open database */
    tt_database_t db;
    memset(&db, 0, sizeof(db));
    if (tt_database_open(&db, project_path) < 0)
    {
        *out_exit_code = 1;
        cJSON *err = make_error("storage_error", "Failed to open database", NULL);
        free(project_path);
        return err;
    }

    tt_index_store_t store;
    if (tt_store_init(&store, &db) < 0)
    {
        *out_exit_code = 1;
        tt_database_close(&db);
        cJSON *err = make_error("storage_error", "Failed to prepare statements", NULL);
        free(project_path);
        return err;
    }

    /* Batch fetch symbols */
    tt_symbol_result_t *found_syms = NULL;
    int found_count = 0;
    char **errors = NULL;
    int error_count = 0;

    tt_store_get_symbols_by_ids(&store, opts->positional, opts->positional_count,
                                &found_syms, &found_count, &errors, &error_count);

    /* Build file content cache (read each unique file once) */
    tt_hashmap_t *file_cache = tt_hashmap_new(32);
    cJSON *symbols_arr = cJSON_CreateArray();
    cJSON *errors_arr = cJSON_CreateArray();

    /* Process found symbols in order of input IDs */
    int context_lines = opts->context;
    if (context_lines < 0)
        context_lines = 0;
    if (context_lines > 50)
        context_lines = 50;

    for (int i = 0; i < found_count; i++)
    {
        tt_symbol_t *sym = &found_syms[i].sym;
        cJSON *item = cJSON_CreateObject();

        /* Build full path */
        char *full_path = tt_path_join(project_path, sym->file);

        /* Read file content (cached) */
        char *content = NULL;
        size_t content_len = 0;
        bool file_ok = false;

        if (!full_path || !tt_file_exists(full_path))
        {
            /* File not on disk */
            cJSON *err_item = cJSON_CreateObject();
            cJSON_AddStringToObject(err_item, "id", sym->id ? sym->id : "");
            cJSON_AddStringToObject(err_item, "error", "Source file not found on disk");
            cJSON_AddItemToArray(errors_arr, err_item);
            cJSON_Delete(item);
            free(full_path);
            continue;
        }

        /* Check cache */
        content = tt_hashmap_get(file_cache, sym->file);
        if (!content)
        {
            content = tt_read_file(full_path, &content_len);
            if (!content)
            {
                cJSON *err_item = cJSON_CreateObject();
                cJSON_AddStringToObject(err_item, "id", sym->id ? sym->id : "");
                cJSON_AddStringToObject(err_item, "error", "Cannot read source file");
                cJSON_AddItemToArray(errors_arr, err_item);
                cJSON_Delete(item);
                free(full_path);
                continue;
            }
            tt_hashmap_set(file_cache, sym->file, content);
        }
        else
        {
            content_len = strlen(content);
        }
        file_ok = true;
        free(full_path);

        /* Extract source via byte offset/length */
        char *source = NULL;
        if (file_ok && sym->byte_offset >= 0 &&
            sym->byte_length > 0 &&
            (size_t)(sym->byte_offset + sym->byte_length) <= content_len)
        {
            source = tt_strndup(content + sym->byte_offset, (size_t)sym->byte_length);
        }
        else if (file_ok)
        {
            cJSON *err_item = cJSON_CreateObject();
            cJSON_AddStringToObject(err_item, "id", sym->id ? sym->id : "");
            cJSON_AddStringToObject(err_item, "error", "Failed to read source bytes");
            cJSON_AddItemToArray(errors_arr, err_item);
            cJSON_Delete(item);
            continue;
        }

        /* Verify content hash if requested */
        bool verified = false;
        if (opts->verify && source)
        {
            /* SHA-256 of source bytes */
            char *hash = tt_fast_hash_hex(source, strlen(source));
            if (hash && sym->content_hash)
            {
                verified = (strcmp(hash, sym->content_hash) == 0);
            }
            free(hash);
        }

        /* Context: if --context > 0, replace source with context-expanded lines */
        if (context_lines > 0 && content)
        {
            /* Split content into lines */
            int line_count = 0;
            char **lines = tt_str_split(content, '\n', &line_count);
            if (lines && line_count > 0)
            {
                int start = sym->line - 1 - context_lines;
                if (start < 0)
                    start = 0;
                int end = (sym->end_line > sym->line ? sym->end_line : sym->line) - 1 + context_lines;
                if (end >= line_count)
                    end = line_count - 1;

                tt_strbuf_t sb;
                tt_strbuf_init(&sb);
                for (int l = start; l <= end; l++)
                {
                    if (l > start)
                        tt_strbuf_append_char(&sb, '\n');
                    tt_strbuf_append_str(&sb, lines[l]);
                }
                free(source);
                source = tt_strbuf_detach(&sb);
                tt_str_split_free(lines);
            }
        }

        /* Build result JSON */
        if (opts->compact)
        {
            cJSON_AddNumberToObject(item, "l", sym->line);
            if (sym->end_line > sym->line)
                cJSON_AddNumberToObject(item, "e", sym->end_line);
            if (!opts->no_sig && sym->signature && sym->signature[0])
                cJSON_AddStringToObject(item, "s", sym->signature);
            if (!opts->no_summary && sym->summary && sym->summary[0] &&
                !tt_is_tier3_fallback(sym->summary, sym->kind, sym->name))
                cJSON_AddStringToObject(item, "d", sym->summary);
            cJSON_AddStringToObject(item, "src", source ? source : "");
            if (opts->verify)
                cJSON_AddBoolToObject(item, "verified", verified);
        }
        else
        {
            cJSON_AddStringToObject(item, "name", sym->name ? sym->name : "");
            cJSON_AddStringToObject(item, "kind", tt_kind_to_str(sym->kind));
            cJSON_AddStringToObject(item, "file", sym->file ? sym->file : "");
            cJSON_AddNumberToObject(item, "line", sym->line);
            if (sym->end_line > sym->line)
                cJSON_AddNumberToObject(item, "end", sym->end_line);
            if (!opts->no_sig && sym->signature && sym->signature[0])
                cJSON_AddStringToObject(item, "sig", sym->signature);
            if (!opts->no_summary && sym->summary && sym->summary[0] &&
                !tt_is_tier3_fallback(sym->summary, sym->kind, sym->name))
                cJSON_AddStringToObject(item, "summary", sym->summary);
            cJSON_AddStringToObject(item, "source", source ? source : "");
            if (opts->verify)
                cJSON_AddBoolToObject(item, "verified", verified);
        }

        cJSON_AddItemToArray(symbols_arr, item);
        free(source);
    }

    /* Add "not found" errors from the store */
    for (int i = 0; i < error_count; i++)
    {
        cJSON *err_item = cJSON_CreateObject();
        /* errors[i] format is "id" from the store */
        cJSON_AddStringToObject(err_item, "id", errors[i] ? errors[i] : "");
        cJSON_AddStringToObject(err_item, "error", "Symbol not found");
        cJSON_AddItemToArray(errors_arr, err_item);
    }

    int success_count = cJSON_GetArraySize(symbols_arr);
    int err_total = cJSON_GetArraySize(errors_arr);

    /* Determine exit code */
    if (success_count > 0 && err_total == 0)
        *out_exit_code = 0;
    else if (success_count == 0)
        *out_exit_code = 1;
    else
        *out_exit_code = 2; /* partial success */

    /* Build output */
    cJSON *result = NULL;
    bool single_success = (opts->positional_count == 1 && success_count == 1 && err_total == 0);

    if (single_success)
    {
        /* Flat output (no wrapper) */
        result = cJSON_DetachItemFromArray(symbols_arr, 0);
        cJSON_Delete(symbols_arr);
        cJSON_Delete(errors_arr);
    }
    else
    {
        /* Batch output */
        result = cJSON_CreateObject();
        cJSON_AddItemToObject(result, "symbols", symbols_arr);
        if (err_total > 0)
            cJSON_AddItemToObject(result, "errors", errors_arr);
        else
            cJSON_Delete(errors_arr);
    }

    /* Track savings */
    if (result)
    {
        const char **sf = calloc(found_count > 0 ? (size_t)found_count : 1,
                                 sizeof(char *));
        for (int i = 0; i < found_count; i++)
            sf[i] = found_syms[i].sym.file;
        int64_t raw_bytes = tt_savings_raw_from_file_sizes(project_path,
                                                           sf, found_count);
        free(sf);
        tt_savings_track(&db, "inspect_symbol", raw_bytes, result);
    }

    /* Cleanup */
    tt_hashmap_free_with_values(file_cache);

    for (int i = 0; i < found_count; i++)
        tt_symbol_result_free(&found_syms[i]);
    free(found_syms);
    for (int i = 0; i < error_count; i++)
        free(errors[i]);
    free(errors);

    tt_store_close(&store);
    tt_database_close(&db);
    free(project_path);

    return result;
}

/* ================================================================
 * inspect:file
 * ================================================================ */

cJSON *tt_cmd_inspect_file_exec(tt_cli_opts_t *opts)
{
    if (opts->positional_count < 1 || !opts->positional[0][0])
    {
        return make_error("missing_argument",
                          "File path is required.",
                          "Usage: toktoken inspect:file <file>");
    }

    /* Resolve project path */
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

    /* Normalize file path */
    char *file_path = normalize_path(opts->positional[0]);
    if (!file_path)
    {
        cJSON *err = make_error("invalid_path",
                                "Invalid file path (absolute paths and path traversal are not allowed).",
                                NULL);
        free(project_path);
        return err;
    }

    /* Open database */
    tt_database_t db;
    memset(&db, 0, sizeof(db));
    if (tt_database_open(&db, project_path) < 0)
    {
        free(file_path);
        cJSON *err = make_error("storage_error", "Failed to open database", NULL);
        free(project_path);
        return err;
    }

    tt_index_store_t store;
    if (tt_store_init(&store, &db) < 0)
    {
        free(file_path);
        tt_database_close(&db);
        cJSON *err = make_error("storage_error", "Failed to prepare statements", NULL);
        free(project_path);
        return err;
    }

    /* Check file in index */
    tt_file_record_t file_rec;
    memset(&file_rec, 0, sizeof(file_rec));
    if (tt_store_get_file(&store, file_path, &file_rec) < 0)
    {
        char msg[512];
        snprintf(msg, sizeof(msg), "File not in index: %s", file_path);
        free(file_path);
        tt_store_close(&store);
        tt_database_close(&db);
        cJSON *err = make_error("file_not_found", msg,
                                "Run \"toktoken index:update\" to sync.");
        free(project_path);
        return err;
    }

    /* Check file exists on disk */
    char *full_path = tt_path_join(project_path, file_path);
    if (!full_path || !tt_file_exists(full_path))
    {
        char msg[512];
        snprintf(msg, sizeof(msg), "File no longer exists on disk: %s", file_path);
        free(full_path);
        free(file_path);
        tt_file_record_free(&file_rec);
        tt_store_close(&store);
        tt_database_close(&db);
        cJSON *err = make_error("file_missing", msg, NULL);
        free(project_path);
        return err;
    }

    /* Read file */
    size_t content_len = 0;
    char *content = tt_read_file(full_path, &content_len);
    free(full_path);

    if (!content)
    {
        char msg[512];
        snprintf(msg, sizeof(msg), "Cannot read file: %s", file_path);
        free(file_path);
        tt_file_record_free(&file_rec);
        tt_store_close(&store);
        tt_database_close(&db);
        cJSON *err = make_error("read_error", msg, NULL);
        free(project_path);
        return err;
    }

    /* Split into lines */
    int total_lines = 0;
    char **all_lines = tt_str_split(content, '\n', &total_lines);
    free(content);

    /* Parse --lines range */
    int start_line = 1;
    int end_line = total_lines;

    if (opts->lines && opts->lines[0])
    {
        int part_count = 0;
        char **parts = tt_str_split(opts->lines, '-', &part_count);
        if (parts && part_count >= 1)
        {
            start_line = atoi(parts[0]);
            if (start_line < 1)
                start_line = 1;

            if (part_count >= 2 && parts[1][0])
            {
                end_line = atoi(parts[1]);
                if (end_line > total_lines)
                    end_line = total_lines;
            }
            else
            {
                end_line = total_lines;
            }
        }
        tt_str_split_free(parts);

        if (start_line > end_line)
        {
            char msg[128];
            snprintf(msg, sizeof(msg), "Invalid line range: %s", opts->lines);
            tt_str_split_free(all_lines);
            free(file_path);
            tt_file_record_free(&file_rec);
            tt_store_close(&store);
            tt_database_close(&db);
            cJSON *err = make_error("invalid_range", msg, NULL);
            free(project_path);
            return err;
        }
    }

    /* Build content from lines[start-1..end-1] */
    tt_strbuf_t sb;
    tt_strbuf_init(&sb);
    for (int i = start_line - 1; i < end_line && i < total_lines; i++)
    {
        if (i > start_line - 1)
            tt_strbuf_append_char(&sb, '\n');
        tt_strbuf_append_str(&sb, all_lines[i] ? all_lines[i] : "");
    }
    char *slice = tt_strbuf_detach(&sb);

    tt_str_split_free(all_lines);

    /* Build result */
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "file", file_path);
    if (file_rec.language && file_rec.language[0])
        cJSON_AddStringToObject(result, "lang", file_rec.language);
    cJSON_AddNumberToObject(result, "total_lines", total_lines);

    cJSON *range = cJSON_CreateArray();
    cJSON_AddItemToArray(range, cJSON_CreateNumber(start_line));
    cJSON_AddItemToArray(range, cJSON_CreateNumber(end_line));
    cJSON_AddItemToObject(result, "range", range);

    cJSON_AddStringToObject(result, "content", slice ? slice : "");

    /* Track savings */
    if (result)
    {
        char *fp = tt_path_join(project_path, file_path);
        int64_t raw_bytes = tt_savings_raw_from_file(fp);
        free(fp);
        tt_savings_track(&db, "inspect_file", raw_bytes, result);
    }

    free(slice);
    free(file_path);
    tt_file_record_free(&file_rec);
    tt_store_close(&store);
    tt_database_close(&db);
    free(project_path);

    return result;
}

/* ================================================================
 * inspect:tree
 * ================================================================ */

/* Tree node for directory hierarchy */
typedef struct tree_node
{
    char *name; /* [owns] */
    struct tree_node **children;
    int child_count;
    int child_cap;
    bool is_leaf; /* true = file, false = directory */
} tree_node_t;

static tree_node_t *tree_node_new(const char *name, bool is_leaf)
{
    tree_node_t *n = calloc(1, sizeof(tree_node_t));
    if (n)
    {
        n->name = tt_strdup(name);
        n->is_leaf = is_leaf;
    }
    return n;
}

static void tree_node_free(tree_node_t *n)
{
    if (!n)
        return;
    for (int i = 0; i < n->child_count; i++)
        tree_node_free(n->children[i]);
    free(n->children);
    free(n->name);
    free(n);
}

static tree_node_t *tree_node_find_child(tree_node_t *parent, const char *name)
{
    for (int i = 0; i < parent->child_count; i++)
    {
        if (strcmp(parent->children[i]->name, name) == 0)
            return parent->children[i];
    }
    return NULL;
}

static void tree_node_add_child(tree_node_t *parent, tree_node_t *child)
{
    if (parent->child_count >= parent->child_cap)
    {
        int new_cap = parent->child_cap ? parent->child_cap * 2 : 4;
        tree_node_t **tmp = realloc(parent->children,
                                    (size_t)new_cap * sizeof(tree_node_t *));
        if (!tmp) return;
        parent->children = tmp;
        parent->child_cap = new_cap;
    }
    parent->children[parent->child_count++] = child;
}

static void insert_path(tree_node_t *root, const char *path, int depth)
{
    int part_count = 0;
    char **parts = tt_str_split(path, '/', &part_count);
    if (!parts || part_count == 0)
    {
        tt_str_split_free(parts);
        return;
    }

    /* Depth truncation */
    bool collapsed = false;
    if (depth > 0 && part_count > depth + 1)
    {
        part_count = depth + 1;
        collapsed = true;
    }

    tree_node_t *current = root;
    for (int i = 0; i < part_count; i++)
    {
        bool is_last = (i == part_count - 1);
        bool is_dir = !is_last || collapsed;

        tree_node_t *child = tree_node_find_child(current, parts[i]);
        if (child)
        {
            /* Node exists — if it was a leaf but we need a dir, upgrade it */
            if (is_dir && child->is_leaf)
                child->is_leaf = false;
            /* If the node already exists as a leaf and we're adding same leaf, skip */
            if (is_last && child->is_leaf)
            {
                tt_str_split_free(parts);
                return;
            }
            current = child;
        }
        else
        {
            child = tree_node_new(parts[i], !is_dir);
            tree_node_add_child(current, child);
            current = child;
        }
    }

    tt_str_split_free(parts);
}

static cJSON *tree_node_to_json(tree_node_t *node)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "name", node->name ? node->name : "");

    if (node->child_count > 0)
    {
        cJSON *children = cJSON_CreateArray();
        for (int i = 0; i < node->child_count; i++)
        {
            cJSON *child = tree_node_to_json(node->children[i]);
            if (child)
                cJSON_AddItemToArray(children, child);
        }
        cJSON_AddItemToObject(obj, "children", children);
    }

    return obj;
}

/* Print tree in table format with connectors */
static void print_tree(tree_node_t *node, const char *prefix, bool is_last)
{
    /* Print this node */
    const char *connector = is_last ? "└── " : "├── ";
    printf("%s%s%s%s\n", prefix, connector, node->name ? node->name : "",
           node->child_count > 0 ? "/" : "");

    /* Print children */
    for (int i = 0; i < node->child_count; i++)
    {
        tt_strbuf_t new_prefix;
        tt_strbuf_init(&new_prefix);
        tt_strbuf_append_str(&new_prefix, prefix);
        tt_strbuf_append_str(&new_prefix, is_last ? "    " : "│   ");

        print_tree(node->children[i], new_prefix.data,
                   i == node->child_count - 1);
        tt_strbuf_free(&new_prefix);
    }
}

cJSON *tt_cmd_inspect_tree_exec(tt_cli_opts_t *opts)
{
    /* Resolve project path */
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

    /* Open database */
    tt_database_t db;
    memset(&db, 0, sizeof(db));
    if (tt_database_open(&db, project_path) < 0)
    {
        cJSON *err = make_error("storage_error", "Failed to open database", NULL);
        free(project_path);
        return err;
    }

    tt_index_store_t store;
    if (tt_store_init(&store, &db) < 0)
    {
        tt_database_close(&db);
        cJSON *err = make_error("storage_error", "Failed to prepare statements", NULL);
        free(project_path);
        return err;
    }

    /* Get all files */
    tt_file_record_t *all_files = NULL;
    int file_count = 0;
    tt_store_get_all_files(&store, &all_files, &file_count);

    /* Filter by language */
    const char **paths = calloc(file_count > 0 ? (size_t)file_count : 1,
                                sizeof(char *));
    int path_count = 0;

    for (int i = 0; i < file_count; i++)
    {
        /* --language filter */
        if (opts->language && opts->language[0])
        {
            const char *norm_lang = tt_normalize_language(all_files[i].language);
            const char *norm_filter = tt_normalize_language(opts->language);
            if (tt_strcasecmp(norm_lang, norm_filter) != 0)
                continue;
        }

        /* --filter / --exclude */
        if (!tt_matches_path_filters(all_files[i].path, opts->filter, opts->exclude))
            continue;

        paths[path_count++] = all_files[i].path;
    }

    /* Build tree */
    tree_node_t *root = tree_node_new(".", false);
    for (int i = 0; i < path_count; i++)
    {
        insert_path(root, paths[i], opts->depth);
    }
    free(paths);

    /* Format output */
    cJSON *result = NULL;

    if (opts->format && strcmp(opts->format, "table") == 0)
    {
        /* Tree format with connectors */
        printf(".\n");
        for (int i = 0; i < root->child_count; i++)
        {
            print_tree(root->children[i], "",
                       i == root->child_count - 1);
        }
        fflush(stdout);
        result = NULL;
    }
    else if (opts->format && strcmp(opts->format, "jsonl") == 0)
    {
        /* Flat list */
        cJSON *arr = cJSON_CreateArray();
        for (int i = 0; i < file_count; i++)
        {
            /* Re-apply filters since we might have skipped some */
            if (opts->language && opts->language[0])
            {
                const char *norm_lang = tt_normalize_language(all_files[i].language);
                const char *norm_filter = tt_normalize_language(opts->language);
                if (tt_strcasecmp(norm_lang, norm_filter) != 0)
                    continue;
            }
            if (!tt_matches_path_filters(all_files[i].path, opts->filter, opts->exclude))
                continue;

            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "path", all_files[i].path);
            cJSON_AddStringToObject(item, "lang",
                                    all_files[i].language ? all_files[i].language : "");
            cJSON_AddItemToArray(arr, item);
        }
        tt_output_jsonl(arr);
        cJSON_Delete(arr);
        result = NULL;
    }
    else
    {
        /* JSON: {"tree": [...], "files": count} */
        cJSON *tree_arr = cJSON_CreateArray();
        for (int i = 0; i < root->child_count; i++)
        {
            cJSON *node = tree_node_to_json(root->children[i]);
            if (node)
                cJSON_AddItemToArray(tree_arr, node);
        }

        result = cJSON_CreateObject();
        cJSON_AddItemToObject(result, "tree", tree_arr);
        cJSON_AddNumberToObject(result, "files", path_count);
    }

    /* Track savings */
    if (result)
    {
        int64_t raw_bytes = tt_savings_raw_from_index(&db);
        tt_savings_track(&db, "inspect_tree", raw_bytes, result);
    }

    tree_node_free(root);
    for (int i = 0; i < file_count; i++)
        tt_file_record_free(&all_files[i]);
    free(all_files);
    tt_store_close(&store);
    tt_database_close(&db);
    free(project_path);

    return result;
}

/* ================================================================
 * CLI wrappers
 * ================================================================ */

int tt_cmd_inspect_outline(tt_cli_opts_t *opts)
{
    cJSON *result = tt_cmd_inspect_outline_exec(opts);
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

int tt_cmd_inspect_symbol(tt_cli_opts_t *opts)
{
    int exit_code = 0;
    cJSON *result = tt_cmd_inspect_symbol_exec(opts, &exit_code);
    if (!result)
    {
        const char *err = tt_error_get();
        if (err && err[0])
            return tt_output_error("internal_error", err, NULL);
        return exit_code;
    }

    /* Table format */
    if (opts->format && strcmp(opts->format, "table") == 0)
    {
        /* For table: print each symbol as "--- {kind} {name} ({file}:{line}) ---\n{source}\n\n" */
        cJSON *symbols = cJSON_GetObjectItem(result, "symbols");
        if (!symbols)
        {
            /* Single result mode */
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(result, "name"));
            const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(result, "kind"));
            const char *file = cJSON_GetStringValue(cJSON_GetObjectItem(result, "file"));
            cJSON *line_item = cJSON_GetObjectItem(result, "line");
            const char *source = cJSON_GetStringValue(cJSON_GetObjectItem(result, "source"));

            printf("--- %s %s (%s:%d) ---\n%s\n\n",
                   kind ? kind : "", name ? name : "",
                   file ? file : "",
                   line_item ? (int)line_item->valuedouble : 0,
                   source ? source : "");
        }
        else
        {
            cJSON *item;
            cJSON_ArrayForEach(item, symbols)
            {
                const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(item, "name"));
                const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(item, "kind"));
                const char *file = cJSON_GetStringValue(cJSON_GetObjectItem(item, "file"));
                cJSON *line_item = cJSON_GetObjectItem(item, "line");
                const char *source = cJSON_GetStringValue(cJSON_GetObjectItem(item, "source"));

                printf("--- %s %s (%s:%d) ---\n%s\n\n",
                       kind ? kind : "", name ? name : "",
                       file ? file : "",
                       line_item ? (int)line_item->valuedouble : 0,
                       source ? source : "");
            }
        }

        /* Print errors */
        cJSON *errs = cJSON_GetObjectItem(result, "errors");
        if (errs)
        {
            cJSON *e;
            cJSON_ArrayForEach(e, errs)
            {
                const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(e, "id"));
                const char *msg = cJSON_GetStringValue(cJSON_GetObjectItem(e, "error"));
                printf("ERROR: %s: %s\n", id ? id : "", msg ? msg : "");
            }
        }
        fflush(stdout);
        cJSON_Delete(result);
        return exit_code;
    }

    /* JSON output */
    if (cJSON_GetObjectItem(result, "error"))
    {
        tt_json_print(result);
        cJSON_Delete(result);
        return 1;
    }

    tt_json_print(result);
    cJSON_Delete(result);
    return exit_code;
}

int tt_cmd_inspect_file(tt_cli_opts_t *opts)
{
    cJSON *result = tt_cmd_inspect_file_exec(opts);
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

int tt_cmd_inspect_tree(tt_cli_opts_t *opts)
{
    cJSON *result = tt_cmd_inspect_tree_exec(opts);
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

/* ---- inspect:dependencies ---- */

cJSON *tt_cmd_inspect_dependencies_exec(tt_cli_opts_t *opts)
{
    const char *query = NULL;
    if (opts->search && opts->search[0])
        query = opts->search;
    else if (opts->positional_count > 0)
        query = opts->positional[0];

    if (!query || !query[0])
    {
        return make_error("missing_argument",
                           "Usage: inspect:dependencies <file>",
                           "Specify a file path to trace its dependency graph");
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

    int max_depth = opts->depth > 0 ? opts->depth : 3;
    tt_dependency_t *deps = NULL;
    int count = 0;
    tt_store_get_dependencies(&store, query, max_depth, &deps, &count);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "file", query);
    cJSON_AddNumberToObject(result, "max_depth", max_depth);
    cJSON_AddNumberToObject(result, "n", count);
    cJSON *arr = cJSON_AddArrayToObject(result, "dependents");
    for (int i = 0; i < count; i++)
    {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "file", deps[i].file);
        cJSON_AddNumberToObject(obj, "depth", deps[i].depth);
        cJSON_AddItemToArray(arr, obj);
    }

    tt_dependency_free(deps, count);
    tt_store_close(&store);
    tt_database_close(&db);
    free(project_path);
    return result;
}

int tt_cmd_inspect_dependencies(tt_cli_opts_t *opts)
{
    cJSON *result = tt_cmd_inspect_dependencies_exec(opts);
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

/* ---- inspect:hierarchy ---- */

cJSON *tt_cmd_inspect_hierarchy_exec(tt_cli_opts_t *opts)
{
    const char *query = NULL;
    if (opts->search && opts->search[0])
        query = opts->search;
    else if (opts->positional_count > 0)
        query = opts->positional[0];

    if (!query || !query[0])
    {
        return make_error("missing_argument",
                           "Usage: inspect:hierarchy <file>",
                           "Specify a file path to show its symbol hierarchy");
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

    int limit = opts->limit > 0 ? opts->limit : 200;
    tt_hierarchy_node_t *nodes = NULL;
    int count = 0;
    tt_store_get_hierarchy(&store, query, opts->language, limit, &nodes, &count);

    /* Build tree structure: group nodes by parent_id */
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "file", query);
    cJSON_AddNumberToObject(result, "n", count);
    cJSON *arr = cJSON_AddArrayToObject(result, "nodes");

    for (int i = 0; i < count; i++)
    {
        /* Only emit top-level nodes (no parent or parent not in result set) */
        bool is_child = false;
        if (nodes[i].parent_id)
        {
            for (int j = 0; j < count; j++)
            {
                if (j != i && strcmp(nodes[j].id, nodes[i].parent_id) == 0)
                {
                    is_child = true;
                    break;
                }
            }
        }
        if (is_child)
            continue;

        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "id", nodes[i].id);
        cJSON_AddStringToObject(obj, "name", nodes[i].name);
        cJSON_AddStringToObject(obj, "kind", nodes[i].kind);
        cJSON_AddNumberToObject(obj, "line", nodes[i].line);
        cJSON_AddNumberToObject(obj, "end_line", nodes[i].end_line);
        if (nodes[i].qualified_name && nodes[i].qualified_name[0])
            cJSON_AddStringToObject(obj, "qname", nodes[i].qualified_name);

        /* Collect children */
        cJSON *children = NULL;
        for (int j = 0; j < count; j++)
        {
            if (j != i && nodes[j].parent_id &&
                strcmp(nodes[j].parent_id, nodes[i].id) == 0)
            {
                if (!children)
                    children = cJSON_AddArrayToObject(obj, "children");
                cJSON *child = cJSON_CreateObject();
                cJSON_AddStringToObject(child, "id", nodes[j].id);
                cJSON_AddStringToObject(child, "name", nodes[j].name);
                cJSON_AddStringToObject(child, "kind", nodes[j].kind);
                cJSON_AddNumberToObject(child, "line", nodes[j].line);
                cJSON_AddItemToArray(children, child);
            }
        }

        cJSON_AddItemToArray(arr, obj);
    }

    tt_hierarchy_node_free(nodes, count);
    tt_store_close(&store);
    tt_database_close(&db);
    free(project_path);
    return result;
}

int tt_cmd_inspect_hierarchy(tt_cli_opts_t *opts)
{
    cJSON *result = tt_cmd_inspect_hierarchy_exec(opts);
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

/* ---- inspect:cycles (Tarjan's SCC) ---- */

typedef struct { int v; int adj_pos; } tarjan_frame_t;

typedef struct {
    int *index_arr;
    int *lowlink;
    bool *on_stack;
    int *stack;
    int stack_top;
    int current_index;

    int **sccs;
    int *scc_sizes;
    int scc_count;
    int scc_cap;

    int *flat_adj;
    int *adj_start;
    int *adj_count;

    tarjan_frame_t *call_stack;  /* shared DFS call stack (allocated once) */
} tarjan_state_t;

/* Iterative Tarjan's SCC — avoids stack overflow on large graphs (50K+ nodes).
 * Uses the pre-allocated call_stack in st (sized to n before first call). */
static void tarjan_strongconnect(tarjan_state_t *st, int root)
{
    tarjan_frame_t *call_stack = st->call_stack;
    if (!call_stack) return;
    int cs_top = 0;

    /* Initialize root */
    st->index_arr[root] = st->current_index;
    st->lowlink[root] = st->current_index;
    st->current_index++;
    st->stack[st->stack_top++] = root;
    st->on_stack[root] = true;
    call_stack[cs_top++] = (tarjan_frame_t){root, 0};

    while (cs_top > 0) {
        tarjan_frame_t *f = &call_stack[cs_top - 1];
        int v = f->v;
        int start = st->adj_start[v];
        int count = st->adj_count[v];

        if (f->adj_pos < count) {
            int w = st->flat_adj[start + f->adj_pos];
            f->adj_pos++;

            if (st->index_arr[w] < 0) {
                /* "Recurse" into w */
                st->index_arr[w] = st->current_index;
                st->lowlink[w] = st->current_index;
                st->current_index++;
                st->stack[st->stack_top++] = w;
                st->on_stack[w] = true;
                call_stack[cs_top++] = (tarjan_frame_t){w, 0};
            } else if (st->on_stack[w]) {
                if (st->index_arr[w] < st->lowlink[v])
                    st->lowlink[v] = st->index_arr[w];
            }
        } else {
            /* All neighbors processed — "return" from this frame */
            cs_top--;
            if (cs_top > 0) {
                int parent = call_stack[cs_top - 1].v;
                if (st->lowlink[v] < st->lowlink[parent])
                    st->lowlink[parent] = st->lowlink[v];
            }

            /* Check if v is SCC root */
            if (st->lowlink[v] == st->index_arr[v]) {
                int *scc = NULL;
                int scc_size = 0;
                int scc_cap_local = 0;
                int w;
                do {
                    w = st->stack[--st->stack_top];
                    st->on_stack[w] = false;
                    if (scc_size >= scc_cap_local) {
                        scc_cap_local = scc_cap_local ? scc_cap_local * 2 : 8;
                        scc = realloc(scc, (size_t)scc_cap_local * sizeof(int));
                    }
                    scc[scc_size++] = w;
                } while (w != v);

                /* Keep SCCs with size > 1, or size == 1 with self-edge */
                bool has_self_edge = false;
                if (scc_size == 1) {
                    int sv = scc[0];
                    int s = st->adj_start[sv];
                    int c = st->adj_count[sv];
                    for (int i = 0; i < c; i++) {
                        if (st->flat_adj[s + i] == sv) {
                            has_self_edge = true;
                            break;
                        }
                    }
                }

                if (scc_size > 1 || has_self_edge) {
                    if (st->scc_count >= st->scc_cap) {
                        st->scc_cap = st->scc_cap ? st->scc_cap * 2 : 16;
                        st->sccs = realloc(st->sccs, (size_t)st->scc_cap * sizeof(int *));
                        st->scc_sizes = realloc(st->scc_sizes, (size_t)st->scc_cap * sizeof(int));
                    }
                    st->sccs[st->scc_count] = scc;
                    st->scc_sizes[st->scc_count] = scc_size;
                    st->scc_count++;
                } else {
                    free(scc);
                }
            }
        }
    }
}

cJSON *tt_cmd_inspect_cycles_exec(tt_cli_opts_t *opts)
{
    tt_timer_start();

    char *project_path = tt_resolve_project_path(opts->path);
    if (!project_path) {
        tt_error_set("Failed to resolve project path");
        return NULL;
    }

    if (!tt_database_exists(project_path)) {
        free(project_path);
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "no_index");
        cJSON_AddStringToObject(err, "message", "No index found.");
        return err;
    }

    tt_database_t db;
    memset(&db, 0, sizeof(db));
    if (tt_database_open(&db, project_path) < 0) {
        free(project_path);
        return NULL;
    }

    tt_index_store_t store;
    if (tt_store_init(&store, &db) < 0) {
        tt_database_close(&db);
        free(project_path);
        return NULL;
    }

    /* Build adjacency list from imports */
    const char *sql =
        "SELECT DISTINCT i.source_file, i.resolved_file FROM imports i "
        "WHERE i.source_file IN (SELECT path FROM files) "
        "AND i.resolved_file != ''";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db.db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        tt_store_close(&store);
        tt_database_close(&db);
        free(project_path);
        return NULL;
    }

    tt_hashmap_t *file_to_id = tt_hashmap_new(512);
    char **id_to_file = NULL;
    int file_count = 0;
    int file_cap = 0;

    int *edge_src = NULL, *edge_dst = NULL;
    int edge_count = 0, edge_cap = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *src = (const char *)sqlite3_column_text(stmt, 0);
        const char *dst = (const char *)sqlite3_column_text(stmt, 1);
        if (!src || !dst) continue;

        int src_id, dst_id;

        if (!tt_hashmap_has(file_to_id, src)) {
            if (file_count >= file_cap) {
                file_cap = file_cap ? file_cap * 2 : 256;
                id_to_file = realloc(id_to_file, (size_t)file_cap * sizeof(char *));
            }
            src_id = file_count;
            id_to_file[file_count] = tt_strdup(src);
            tt_hashmap_set(file_to_id, src, (void *)(intptr_t)(src_id + 1));
            file_count++;
        } else {
            src_id = (int)((intptr_t)tt_hashmap_get(file_to_id, src)) - 1;
        }

        if (!tt_hashmap_has(file_to_id, dst)) {
            if (file_count >= file_cap) {
                file_cap = file_cap ? file_cap * 2 : 256;
                id_to_file = realloc(id_to_file, (size_t)file_cap * sizeof(char *));
            }
            dst_id = file_count;
            id_to_file[file_count] = tt_strdup(dst);
            tt_hashmap_set(file_to_id, dst, (void *)(intptr_t)(dst_id + 1));
            file_count++;
        } else {
            dst_id = (int)((intptr_t)tt_hashmap_get(file_to_id, dst)) - 1;
        }

        if (edge_count >= edge_cap) {
            edge_cap = edge_cap ? edge_cap * 2 : 256;
            edge_src = realloc(edge_src, (size_t)edge_cap * sizeof(int));
            edge_dst = realloc(edge_dst, (size_t)edge_cap * sizeof(int));
        }
        edge_src[edge_count] = src_id;
        edge_dst[edge_count] = dst_id;
        edge_count++;
    }
    sqlite3_finalize(stmt);

    /* Build flat adjacency list */
    int *adj_count_arr = calloc((size_t)(file_count > 0 ? file_count : 1), sizeof(int));
    for (int i = 0; i < edge_count; i++)
        adj_count_arr[edge_src[i]]++;

    int *adj_start_arr = calloc((size_t)(file_count > 0 ? file_count : 1), sizeof(int));
    int total_edges = 0;
    for (int i = 0; i < file_count; i++) {
        adj_start_arr[i] = total_edges;
        total_edges += adj_count_arr[i];
    }

    int *flat_adj = calloc((size_t)(total_edges > 0 ? total_edges : 1), sizeof(int));
    int *fill_arr = calloc((size_t)(file_count > 0 ? file_count : 1), sizeof(int));
    for (int i = 0; i < edge_count; i++) {
        int s = edge_src[i];
        flat_adj[adj_start_arr[s] + fill_arr[s]] = edge_dst[i];
        fill_arr[s]++;
    }
    free(fill_arr);
    free(edge_src);
    free(edge_dst);

    /* Run Tarjan's SCC */
    tarjan_state_t st;
    memset(&st, 0, sizeof(st));
    st.flat_adj = flat_adj;
    st.adj_start = adj_start_arr;
    st.adj_count = adj_count_arr;

    if (file_count > 0) {
        st.index_arr = malloc((size_t)file_count * sizeof(int));
        st.lowlink = malloc((size_t)file_count * sizeof(int));
        st.on_stack = calloc((size_t)file_count, sizeof(bool));
        st.stack = malloc((size_t)file_count * sizeof(int));
        st.call_stack = malloc((size_t)file_count * sizeof(tarjan_frame_t));

        for (int i = 0; i < file_count; i++)
            st.index_arr[i] = -1;

        for (int i = 0; i < file_count; i++) {
            if (st.index_arr[i] < 0)
                tarjan_strongconnect(&st, i);
        }
    }

    /* Build result JSON, applying --cross-dir and --min-length filters */
    int min_len_filter = opts->min_length > 0 ? opts->min_length : 0;
    bool cross_dir_filter = opts->cross_dir;

    cJSON *result = cJSON_CreateObject();
    cJSON *cycles_arr = cJSON_CreateArray();
    int cross_dir_count = 0;
    int max_length = 0;
    int filtered_count = 0;

    for (int c = 0; c < st.scc_count; c++) {
        int len = st.scc_sizes[c];

        /* Detect cross-directory cycle */
        bool cross_dir = false;
        const char *first_file = NULL;
        size_t first_dir_len = 0;

        for (int j = 0; j < len; j++) {
            const char *file = id_to_file[st.sccs[c][j]];
            const char *last_sep = strrchr(file, '/');
            size_t dir_len = last_sep ? (size_t)(last_sep - file) : 0;
            if (j == 0) {
                first_file = file;
                first_dir_len = dir_len;
            } else if (dir_len != first_dir_len ||
                       (dir_len > 0 && strncmp(first_file, file, dir_len) != 0)) {
                cross_dir = true;
            }
        }

        if (cross_dir) cross_dir_count++;
        if (len > max_length) max_length = len;

        /* Apply filters */
        if (cross_dir_filter && !cross_dir) {
            free(st.sccs[c]);
            continue;
        }
        if (min_len_filter > 0 && len < min_len_filter) {
            free(st.sccs[c]);
            continue;
        }

        cJSON *cycle = cJSON_CreateObject();
        cJSON *files_arr = cJSON_CreateArray();
        for (int j = 0; j < len; j++) {
            const char *file = id_to_file[st.sccs[c][j]];
            cJSON_AddItemToArray(files_arr, cJSON_CreateString(file));
        }

        cJSON_AddItemToObject(cycle, "files", files_arr);
        cJSON_AddNumberToObject(cycle, "length", len);
        cJSON_AddBoolToObject(cycle, "cross_dir", cross_dir);
        cJSON_AddItemToArray(cycles_arr, cycle);
        filtered_count++;

        free(st.sccs[c]);
    }

    cJSON_AddItemToObject(result, "cycles", cycles_arr);

    cJSON *summary_obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(summary_obj, "total_cycles", st.scc_count);
    cJSON_AddNumberToObject(summary_obj, "cross_dir_cycles", cross_dir_count);
    cJSON_AddNumberToObject(summary_obj, "max_length", max_length);
    if (cross_dir_filter || min_len_filter > 0)
        cJSON_AddNumberToObject(summary_obj, "filtered_count", filtered_count);
    cJSON_AddItemToObject(result, "summary", summary_obj);

    /* Cleanup */
    free(st.sccs);
    free(st.scc_sizes);
    free(st.index_arr);
    free(st.lowlink);
    free(st.on_stack);
    free(st.stack);
    free(st.call_stack);
    free(flat_adj);
    free(adj_start_arr);
    free(adj_count_arr);
    for (int i = 0; i < file_count; i++)
        free(id_to_file[i]);
    free(id_to_file);
    tt_hashmap_free(file_to_id);

    tt_savings_track(&db, "inspect_cycles", 0, result);

    tt_store_close(&store);
    tt_database_close(&db);
    free(project_path);

    return result;
}

int tt_cmd_inspect_cycles(tt_cli_opts_t *opts)
{
    cJSON *result = tt_cmd_inspect_cycles_exec(opts);
    if (!result) {
        const char *err = tt_error_get();
        if (err && err[0])
            return tt_output_error("internal_error", err, NULL);
        return 0;
    }
    if (cJSON_GetObjectItem(result, "error")) {
        tt_json_print(result);
        cJSON_Delete(result);
        return 1;
    }
    tt_json_print(result);
    cJSON_Delete(result);
    return 0;
}

/* ---- inspect:blast (BFS blast radius) ---- */

static int free_blast_cache_entry(const char *key, void *value, void *userdata)
{
    (void)key; (void)userdata;
    free(value);
    return 0;
}

/* Check if `name` appears as a word-boundary match in `content` */
static int count_word_references(const char *content, const char *name)
{
    if (!content || !name || !name[0]) return 0;
    size_t nlen = strlen(name);
    int count = 0;
    const char *p = content;
    while ((p = strstr(p, name)) != NULL) {
        /* Check word boundary before */
        if (p > content) {
            char before = p[-1];
            if (isalnum((unsigned char)before) || before == '_') {
                p += nlen;
                continue;
            }
        }
        /* Check word boundary after */
        char after = p[nlen];
        if (isalnum((unsigned char)after) || after == '_') {
            p += nlen;
            continue;
        }
        count++;
        p += nlen;
    }
    return count;
}

#define BLAST_MAX_DEPTH 3
#define BLAST_MAX_NODES 500

cJSON *tt_cmd_inspect_blast_exec(tt_cli_opts_t *opts)
{
    tt_timer_start();

    if (opts->positional_count < 1 && (!opts->search || !opts->search[0])) {
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "missing_argument");
        cJSON_AddStringToObject(err, "message", "Symbol ID is required.");
        return err;
    }

    const char *sym_id = (opts->search && opts->search[0])
                          ? opts->search
                          : opts->positional[0];

    char *project_path = tt_resolve_project_path(opts->path);
    if (!project_path) {
        tt_error_set("Failed to resolve project path");
        return NULL;
    }

    if (!tt_database_exists(project_path)) {
        free(project_path);
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "no_index");
        cJSON_AddStringToObject(err, "message", "No index found.");
        return err;
    }

    tt_database_t db;
    memset(&db, 0, sizeof(db));
    if (tt_database_open(&db, project_path) < 0) {
        free(project_path);
        return NULL;
    }

    tt_index_store_t store;
    if (tt_store_init(&store, &db) < 0) {
        tt_database_close(&db);
        free(project_path);
        return NULL;
    }

    /* Look up the target symbol */
    tt_symbol_result_t sym_result;
    memset(&sym_result, 0, sizeof(sym_result));
    if (tt_store_get_symbol(&store, sym_id, &sym_result) < 0) {
        tt_store_close(&store);
        tt_database_close(&db);
        free(project_path);
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "not_found");
        cJSON_AddStringToObject(err, "message", "Symbol not found.");
        return err;
    }

    const char *target_name = sym_result.sym.name;
    const char *target_file = sym_result.sym.file;

    int max_depth = opts->depth > 0 ? opts->depth : 2;
    if (max_depth > BLAST_MAX_DEPTH) max_depth = BLAST_MAX_DEPTH;

    /* BFS on reverse import graph */
    tt_hashmap_t *visited = tt_hashmap_new(256);
    /* BFS queue: file path + depth */
    char **bfs_queue = NULL;
    int *bfs_depth = NULL;
    int bfs_head = 0, bfs_tail = 0, bfs_cap = 0;

    /* Seed with target file */
    if (target_file) {
        bfs_cap = 64;
        bfs_queue = malloc((size_t)bfs_cap * sizeof(char *));
        bfs_depth = malloc((size_t)bfs_cap * sizeof(int));
        bfs_queue[bfs_tail] = tt_strdup(target_file);
        bfs_depth[bfs_tail] = 0;
        bfs_tail++;
        tt_hashmap_set(visited, target_file, (void *)1);
    }

    cJSON *confirmed = cJSON_CreateArray();
    cJSON *potential = cJSON_CreateArray();
    int confirmed_count = 0, potential_count = 0;
    int actual_max_depth = 0;

    /* File content cache: avoids re-reading the same file from disk.
     * Keys = relative file paths, values = malloc'd file content strings.
     * Bounded by BLAST_MAX_NODES (500) so memory stays reasonable. */
    tt_hashmap_t *file_cache = tt_hashmap_new(128);

    while (bfs_head < bfs_tail) {
        char *cur_file = bfs_queue[bfs_head];
        int cur_depth = bfs_depth[bfs_head];
        bfs_head++;

        if (cur_depth > 0 && target_name) {
            /* Read file (cached) and scan for references */
            const char *content = (const char *)tt_hashmap_get(file_cache, cur_file);
            if (!content) {
                char *full_path = tt_path_join(project_path, cur_file);
                if (full_path) {
                    size_t flen = 0;
                    char *data = tt_read_file(full_path, &flen);
                    free(full_path);
                    if (data) {
                        tt_hashmap_set(file_cache, cur_file, data);
                        content = data;
                    }
                }
            }

            int refs = content ? count_word_references(content, target_name) : 0;

            if (refs > 0) {
                cJSON *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "file", cur_file);
                cJSON_AddNumberToObject(item, "depth", cur_depth);
                cJSON_AddNumberToObject(item, "references", refs);
                cJSON_AddItemToArray(confirmed, item);
                confirmed_count++;
            } else {
                cJSON *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "file", cur_file);
                cJSON_AddNumberToObject(item, "depth", cur_depth);
                cJSON_AddItemToArray(potential, item);
                potential_count++;
            }
            if (cur_depth > actual_max_depth)
                actual_max_depth = cur_depth;
        }

        /* Expand: find importers of cur_file */
        if (cur_depth < max_depth &&
            (confirmed_count + potential_count) < BLAST_MAX_NODES) {
            tt_import_t *imps = NULL;
            int imp_count = 0;
            tt_store_get_importers(&store, cur_file, &imps, &imp_count);

            for (int i = 0; i < imp_count; i++) {
                const char *from = imps[i].from_file;
                if (!from || tt_hashmap_has(visited, from)) continue;
                tt_hashmap_set(visited, from, (void *)1);

                if (bfs_tail >= bfs_cap) {
                    bfs_cap = bfs_cap * 2;
                    bfs_queue = realloc(bfs_queue, (size_t)bfs_cap * sizeof(char *));
                    bfs_depth = realloc(bfs_depth, (size_t)bfs_cap * sizeof(int));
                }
                bfs_queue[bfs_tail] = tt_strdup(from);
                bfs_depth[bfs_tail] = cur_depth + 1;
                bfs_tail++;
            }
            tt_import_array_free(imps, imp_count);
        }

        free(cur_file);
    }

    /* Build result */
    cJSON *result = cJSON_CreateObject();

    cJSON *target = cJSON_CreateObject();
    cJSON_AddStringToObject(target, "id", sym_id);
    cJSON_AddStringToObject(target, "name", target_name ? target_name : "");
    cJSON_AddStringToObject(target, "file", target_file ? target_file : "");
    cJSON_AddItemToObject(result, "target", target);

    cJSON_AddNumberToObject(result, "depth", max_depth);
    cJSON_AddItemToObject(result, "confirmed", confirmed);
    cJSON_AddItemToObject(result, "potential", potential);

    cJSON *summary = cJSON_CreateObject();
    cJSON_AddNumberToObject(summary, "confirmed_count", confirmed_count);
    cJSON_AddNumberToObject(summary, "potential_count", potential_count);
    cJSON_AddNumberToObject(summary, "max_depth", actual_max_depth);
    cJSON_AddItemToObject(result, "summary", summary);

    /* Cleanup */
    free(bfs_queue);
    free(bfs_depth);
    tt_hashmap_iter(file_cache, free_blast_cache_entry, NULL);
    tt_hashmap_free(file_cache);
    tt_hashmap_free(visited);
    tt_symbol_result_free(&sym_result);
    tt_savings_track(&db, "inspect_blast", 0, result);
    tt_store_close(&store);
    tt_database_close(&db);
    free(project_path);

    return result;
}

int tt_cmd_inspect_blast(tt_cli_opts_t *opts)
{
    cJSON *result = tt_cmd_inspect_blast_exec(opts);
    if (!result) {
        const char *err = tt_error_get();
        if (err && err[0])
            return tt_output_error("internal_error", err, NULL);
        return 0;
    }
    if (cJSON_GetObjectItem(result, "error")) {
        tt_json_print(result);
        cJSON_Delete(result);
        return 1;
    }
    tt_json_print(result);
    cJSON_Delete(result);
    return 0;
}
