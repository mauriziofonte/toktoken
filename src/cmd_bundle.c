/*
 * cmd_bundle.c -- inspect:bundle command.
 *
 * Assembles a self-contained context bundle for a symbol, combining:
 *   1. Symbol definition (full source code)
 *   2. Import statements from the same file
 *   3. File outline (sibling symbols for context)
 *   4. Optionally, importer files (--full flag)
 *
 * This replaces the common multi-tool round-trip pattern:
 *   inspect:symbol → inspect:outline → find:importers
 * with a single call that returns everything needed for AI context.
 *
 * Surpasses upstream get_context_bundle by:
 *   - Including sibling symbols (file outline) for structural context
 *   - Supporting batch IDs (comma-separated)
 *   - Including token savings metrics
 *   - Supporting the --compact flag for ~47% smaller output
 */

#include "cmd_bundle.h"
#include "json_output.h"
#include "error.h"
#include "platform.h"
#include "database.h"
#include "index_store.h"
#include "import_extractor.h"
#include "str_util.h"
#include "token_savings.h"
#include "hashmap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cJSON *make_error(const char *code, const char *message, const char *hint)
{
    cJSON *json = cJSON_CreateObject();
    if (!json) return NULL;
    cJSON_AddStringToObject(json, "error", code);
    cJSON_AddStringToObject(json, "message", message);
    if (hint && hint[0])
        cJSON_AddStringToObject(json, "hint", hint);
    return json;
}

/* Extract import lines from file content using language-aware patterns */
static cJSON *extract_imports_from_content(const char *content, const char *language)
{
    cJSON *arr = cJSON_CreateArray();
    if (!content || !language) return arr;

    int nlines = 0;
    char **lines = tt_str_split(content, '\n', &nlines);
    if (!lines) return arr;

    /* Language-specific import patterns (prefix matching) */
    typedef struct { const char *lang; const char *prefixes[8]; } import_pattern_t;
    static const import_pattern_t patterns[] = {
        {"python",     {"import ", "from ",     NULL}},
        {"javascript", {"import ", "require(",  "const ", "let ", NULL}},
        {"typescript", {"import ", "require(",  NULL}},
        {"go",         {"import ", NULL}},
        {"rust",       {"use ",   "extern crate ", NULL}},
        {"java",       {"import ", NULL}},
        {"kotlin",     {"import ", NULL}},
        {"csharp",     {"using ",  NULL}},
        {"c",          {"#include ", NULL}},
        {"cpp",        {"#include ", NULL}},
        {"swift",      {"import ", NULL}},
        {"ruby",       {"require ", "require_relative ", NULL}},
        {"php",        {"use ",   "require ", "include ", NULL}},
        {"elixir",     {"import ", "alias ", "use ", "require ", NULL}},
        {"dart",       {"import ", "part ", NULL}},
        {"lua",        {"require(", "require '", "require \"", NULL}},
        {"perl",       {"use ",   "require ", NULL}},
        {"haskell",    {"import ", NULL}},
        {"scala",      {"import ", NULL}},
        {"erlang",     {"-include(", "-import(", NULL}},
        {NULL,         {NULL}}
    };

    const char *const *prefixes = NULL;
    for (int i = 0; patterns[i].lang; i++) {
        if (strcmp(patterns[i].lang, language) == 0) {
            prefixes = patterns[i].prefixes;
            break;
        }
    }

    /* Go: special handling for import blocks */
    int in_go_import_block = 0;

    for (int i = 0; i < nlines; i++) {
        const char *line = lines[i];
        const char *trimmed = line;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;

        /* Go import block handling */
        if (strcmp(language, "go") == 0) {
            if (strncmp(trimmed, "import (", 8) == 0) {
                in_go_import_block = 1;
                cJSON_AddItemToArray(arr, cJSON_CreateString(line));
                continue;
            }
            if (in_go_import_block) {
                cJSON_AddItemToArray(arr, cJSON_CreateString(line));
                if (strchr(trimmed, ')')) in_go_import_block = 0;
                continue;
            }
        }

        if (!prefixes) continue;

        /* Check against prefix patterns */
        for (int p = 0; prefixes[p]; p++) {
            size_t plen = strlen(prefixes[p]);

            /* For require(), check if the line contains it anywhere */
            if (strchr(prefixes[p], '(')) {
                if (strstr(trimmed, prefixes[p])) {
                    cJSON_AddItemToArray(arr, cJSON_CreateString(line));
                    break;
                }
            } else if (strncmp(trimmed, prefixes[p], plen) == 0) {
                /* JS/TS: only match const/let if followed by require */
                if ((strcmp(prefixes[p], "const ") == 0 ||
                     strcmp(prefixes[p], "let ") == 0) &&
                    !strstr(trimmed, "require(")) {
                    continue;
                }
                cJSON_AddItemToArray(arr, cJSON_CreateString(line));
                break;
            }
        }
    }

    tt_str_split_free(lines);
    return arr;
}

/* Build a bundle for a single symbol. Returns cJSON object or NULL on error.
 * On error, sets *err_id to the failing ID (does not allocate). */
static cJSON *build_single_bundle(tt_index_store_t *store,
                                  const char *project_path,
                                  const char *sym_id,
                                  const tt_cli_opts_t *opts,
                                  char **file_content_out,
                                  size_t *file_len_out)
{
    tt_symbol_result_t sym_result;
    memset(&sym_result, 0, sizeof(sym_result));
    if (tt_store_get_symbol(store, sym_id, &sym_result) < 0)
        return NULL;

    tt_symbol_t *sym = &sym_result.sym;
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "id", sym->id ? sym->id : "");

    /* 1. Symbol definition */
    cJSON *definition = cJSON_CreateObject();
    cJSON_AddStringToObject(definition, "name", sym->name ? sym->name : "");
    cJSON_AddStringToObject(definition, "kind",
                             sym->kind ? tt_kind_to_str(sym->kind) : "");
    cJSON_AddStringToObject(definition, "file", sym->file ? sym->file : "");
    cJSON_AddNumberToObject(definition, "line", sym->line);
    cJSON_AddNumberToObject(definition, "end_line", sym->end_line);
    if (!opts->no_sig && sym->signature && sym->signature[0])
        cJSON_AddStringToObject(definition, "signature", sym->signature);
    if (sym->docstring && sym->docstring[0])
        cJSON_AddStringToObject(definition, "docstring", sym->docstring);
    if (sym->language && sym->language[0])
        cJSON_AddStringToObject(definition, "language", sym->language);

    /* Read source code for the symbol */
    char *full_path = sym->file ? tt_path_join(project_path, sym->file) : NULL;
    char *file_content = NULL;
    size_t file_len = 0;
    if (full_path)
        file_content = tt_read_file(full_path, &file_len);

    if (file_content && sym->line > 0 && sym->end_line >= sym->line) {
        int target_start = sym->line;
        int target_end = sym->end_line;
        const char *p = file_content;
        const char *line_start = p;
        int cur_line = 1;
        const char *extract_begin = NULL;
        const char *extract_end = NULL;

        while (*p) {
            if (cur_line == target_start && !extract_begin)
                extract_begin = line_start;
            if (*p == '\n') {
                if (cur_line == target_end) {
                    extract_end = p;
                    break;
                }
                cur_line++;
                line_start = p + 1;
            }
            p++;
        }
        if (extract_begin && !extract_end)
            extract_end = p;
        if (extract_begin && extract_end) {
            size_t src_len = (size_t)(extract_end - extract_begin);
            char *source = tt_strndup(extract_begin, src_len);
            if (source) {
                cJSON_AddStringToObject(definition, "source", source);
                free(source);
            }
        }
    }
    cJSON_AddItemToObject(result, "definition", definition);

    /* 2. Imports from the same file */
    cJSON *imports = extract_imports_from_content(
        file_content, sym->language ? sym->language : "");
    cJSON_AddItemToObject(result, "imports", imports);

    /* 3. File outline (sibling symbols) */
    if (sym->file) {
        tt_symbol_result_t *siblings = NULL;
        int sib_count = 0;
        tt_store_get_symbols_by_file(store, sym->file, &siblings, &sib_count);

        cJSON *outline = cJSON_CreateArray();
        for (int i = 0; i < sib_count; i++) {
            tt_symbol_t *sib = &siblings[i].sym;
            if (sib->id && sym->id && strcmp(sib->id, sym->id) == 0)
                continue;

            cJSON *sib_obj = cJSON_CreateObject();
            if (opts->compact) {
                cJSON_AddStringToObject(sib_obj, "n", sib->name ? sib->name : "");
                cJSON_AddStringToObject(sib_obj, "k",
                                         sib->kind ? tt_kind_to_str(sib->kind) : "");
                cJSON_AddNumberToObject(sib_obj, "l", sib->line);
            } else {
                cJSON_AddStringToObject(sib_obj, "name", sib->name ? sib->name : "");
                cJSON_AddStringToObject(sib_obj, "kind",
                                         sib->kind ? tt_kind_to_str(sib->kind) : "");
                cJSON_AddNumberToObject(sib_obj, "line", sib->line);
                if (!opts->no_sig && sib->signature && sib->signature[0])
                    cJSON_AddStringToObject(sib_obj, "signature", sib->signature);
            }
            cJSON_AddItemToArray(outline, sib_obj);
        }
        cJSON_AddItemToObject(result, "outline", outline);

        for (int i = 0; i < sib_count; i++)
            tt_symbol_result_free(&siblings[i]);
        free(siblings);
    }

    /* 4. Importers (when --full is set) */
    if (opts->full && sym->file) {
        tt_import_t *imps = NULL;
        int imp_count = 0;
        tt_store_get_importers(store, sym->file, &imps, &imp_count);

        cJSON *importers = cJSON_CreateArray();
        for (int i = 0; i < imp_count; i++) {
            cJSON *imp = cJSON_CreateObject();
            cJSON_AddStringToObject(imp, "file",
                                     imps[i].from_file ? imps[i].from_file : "");
            cJSON_AddNumberToObject(imp, "line", imps[i].line);
            cJSON_AddItemToArray(importers, imp);
        }
        cJSON_AddItemToObject(result, "importers", importers);

        tt_import_array_free(imps, imp_count);
    }

    /* 5. Callers (when --include-callers is set) */
    if (opts->include_callers && sym->id) {
        tt_caller_t *callers = NULL;
        int caller_count = 0;
        tt_store_find_callers(store, sym->id, 50, &callers, &caller_count);

        if (caller_count > 0) {
            cJSON *callers_arr = cJSON_CreateArray();
            for (int i = 0; i < caller_count; i++) {
                cJSON *c = cJSON_CreateObject();
                cJSON_AddStringToObject(c, "id", callers[i].id);
                cJSON_AddStringToObject(c, "file", callers[i].file);
                cJSON_AddStringToObject(c, "name", callers[i].name);
                cJSON_AddStringToObject(c, "kind", callers[i].kind);
                cJSON_AddNumberToObject(c, "line", callers[i].line);
                if (!opts->no_sig && callers[i].signature && callers[i].signature[0])
                    cJSON_AddStringToObject(c, "sig", callers[i].signature);
                cJSON_AddItemToArray(callers_arr, c);
            }
            cJSON_AddItemToObject(result, "callers", callers_arr);
        }
        tt_caller_free(callers, caller_count);
    }

    /* Pass file content back for savings tracking */
    if (file_content_out) {
        *file_content_out = file_content;
        *file_len_out = file_len;
        file_content = NULL; /* caller owns it */
    }

    free(file_content);
    free(full_path);
    tt_symbol_result_free(&sym_result);

    return result;
}

#define MAX_BUNDLE_IDS 20

cJSON *tt_cmd_inspect_bundle_exec(tt_cli_opts_t *opts)
{
    tt_timer_start();

    if (opts->positional_count < 1 && (!opts->search || !opts->search[0])) {
        return make_error("missing_argument",
                           "Symbol ID is required.",
                           "Usage: toktoken inspect:bundle <id>");
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
        return make_error("no_index", "No index found.",
                           "Run \"toktoken index:create\" first.");
    }

    tt_database_t db;
    memset(&db, 0, sizeof(db));
    if (tt_database_open(&db, project_path) < 0) {
        free(project_path);
        return make_error("storage_error", "Failed to open database", NULL);
    }

    tt_index_store_t store;
    if (tt_store_init(&store, &db) < 0) {
        tt_database_close(&db);
        free(project_path);
        return make_error("storage_error", "Failed to prepare statements", NULL);
    }

    /* Check for multi-symbol (comma-separated IDs) */
    bool is_multi = (strchr(sym_id, ',') != NULL);

    if (!is_multi) {
        /* Single symbol: backward-compatible flat structure */
        char *file_content = NULL;
        size_t file_len = 0;
        cJSON *result = build_single_bundle(&store, project_path, sym_id, opts,
                                            &file_content, &file_len);
        if (!result) {
            free(file_content);
            tt_store_close(&store);
            tt_database_close(&db);
            free(project_path);
            return make_error("not_found",
                               "Symbol not found in index.",
                               "Verify the symbol ID with search:symbols");
        }

        int64_t raw_bytes = file_content ? (int64_t)file_len : 0;
        tt_savings_track(&db, "inspect_bundle", raw_bytes, result);

        free(file_content);
        tt_store_close(&store);
        tt_database_close(&db);
        free(project_path);
        return result;
    }

    /* Multi-symbol: split IDs, build array, deduplicate imports */
    int id_count = 0;
    char **ids = tt_str_split(sym_id, ',', &id_count);
    if (!ids || id_count == 0) {
        free(ids);
        tt_store_close(&store);
        tt_database_close(&db);
        free(project_path);
        return make_error("missing_argument", "No valid IDs provided.", NULL);
    }

    if (id_count > MAX_BUNDLE_IDS)
        id_count = MAX_BUNDLE_IDS;

    cJSON *result = cJSON_CreateObject();
    cJSON *symbols_arr = cJSON_CreateArray();
    cJSON *errors_arr = cJSON_CreateArray();
    tt_hashmap_t *seen_ids = tt_hashmap_new(64);
    int64_t total_raw_bytes = 0;

    for (int i = 0; i < id_count; i++) {
        /* Trim whitespace */
        char *id = ids[i];
        while (*id == ' ' || *id == '\t') id++;
        char *end = id + strlen(id) - 1;
        while (end > id && (*end == ' ' || *end == '\t')) *end-- = '\0';

        if (!id[0]) continue;
        if (tt_hashmap_has(seen_ids, id)) continue; /* skip duplicates */
        tt_hashmap_set(seen_ids, id, (void *)1);

        char *file_content = NULL;
        size_t file_len = 0;
        cJSON *bundle = build_single_bundle(&store, project_path, id, opts,
                                            &file_content, &file_len);
        if (!bundle) {
            cJSON *err = cJSON_CreateObject();
            cJSON_AddStringToObject(err, "id", id);
            cJSON_AddStringToObject(err, "error", "not_found");
            cJSON_AddItemToArray(errors_arr, err);
        } else {
            cJSON_AddItemToArray(symbols_arr, bundle);
            if (file_content)
                total_raw_bytes += (int64_t)file_len;
        }
        free(file_content);
    }

    tt_hashmap_free(seen_ids);
    tt_str_split_free(ids);

    cJSON_AddNumberToObject(result, "symbol_count", cJSON_GetArraySize(symbols_arr));
    cJSON_AddItemToObject(result, "symbols", symbols_arr);
    if (cJSON_GetArraySize(errors_arr) > 0)
        cJSON_AddItemToObject(result, "errors", errors_arr);
    else
        cJSON_Delete(errors_arr);

    tt_savings_track(&db, "inspect_bundle", total_raw_bytes, result);

    tt_store_close(&store);
    tt_database_close(&db);
    free(project_path);

    return result;
}

char *tt_bundle_render_markdown(const cJSON *result)
{
    tt_strbuf_t sb;
    tt_strbuf_init(&sb);

    /* Definition section */
    const cJSON *def = cJSON_GetObjectItemCaseSensitive(result, "definition");
    if (def) {
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(def, "name"));
        const char *kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(def, "kind"));
        const char *file = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(def, "file"));
        const cJSON *line_j = cJSON_GetObjectItemCaseSensitive(def, "line");
        const char *lang = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(def, "language"));
        const char *sig = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(def, "signature"));
        const char *doc = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(def, "docstring"));
        const char *source = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(def, "source"));

        tt_strbuf_append_str(&sb, "## Definition\n\n");
        tt_strbuf_appendf(&sb, "**%s** (`%s`)",
                          name ? name : "", kind ? kind : "");
        if (file) {
            tt_strbuf_appendf(&sb, " in `%s", file);
            if (line_j && cJSON_IsNumber(line_j))
                tt_strbuf_appendf(&sb, ":%d", (int)line_j->valuedouble);
            tt_strbuf_append_str(&sb, "`");
        }
        tt_strbuf_append_char(&sb, '\n');

        if (sig && sig[0])
            tt_strbuf_appendf(&sb, "\nSignature: `%s`\n", sig);
        if (doc && doc[0])
            tt_strbuf_appendf(&sb, "\n> %s\n", doc);

        if (source && source[0]) {
            tt_strbuf_appendf(&sb, "\n```%s\n", lang ? lang : "");
            tt_strbuf_append_str(&sb, source);
            tt_strbuf_append_str(&sb, "\n```\n");
        }
    }

    /* Imports section */
    const cJSON *imports = cJSON_GetObjectItemCaseSensitive(result, "imports");
    if (imports && cJSON_GetArraySize(imports) > 0) {
        tt_strbuf_append_str(&sb, "\n## Imports\n\n");
        const cJSON *imp;
        cJSON_ArrayForEach(imp, imports) {
            const char *line = cJSON_GetStringValue(imp);
            if (line)
                tt_strbuf_appendf(&sb, "- `%s`\n", line);
        }
    }

    /* File outline section */
    const cJSON *outline = cJSON_GetObjectItemCaseSensitive(result, "outline");
    if (outline && cJSON_GetArraySize(outline) > 0) {
        tt_strbuf_append_str(&sb, "\n## File Outline\n\n");
        tt_strbuf_append_str(&sb, "| Kind | Name | Line | Signature |\n");
        tt_strbuf_append_str(&sb, "|------|------|------|-----------|\n");
        const cJSON *sib;
        cJSON_ArrayForEach(sib, outline) {
            const char *n = cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(sib, "name"));
            if (!n) n = cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(sib, "n"));
            const char *k = cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(sib, "kind"));
            if (!k) k = cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(sib, "k"));
            const cJSON *l = cJSON_GetObjectItemCaseSensitive(sib, "line");
            if (!l) l = cJSON_GetObjectItemCaseSensitive(sib, "l");
            const char *sig = cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(sib, "signature"));

            tt_strbuf_appendf(&sb, "| %s | %s | %d | %s |\n",
                              k ? k : "", n ? n : "",
                              (l && cJSON_IsNumber(l)) ? (int)l->valuedouble : 0,
                              sig ? sig : "");
        }
    }

    /* Importers section */
    const cJSON *importers = cJSON_GetObjectItemCaseSensitive(result, "importers");
    if (importers && cJSON_GetArraySize(importers) > 0) {
        tt_strbuf_append_str(&sb, "\n## Importers\n\n");
        const cJSON *imp;
        cJSON_ArrayForEach(imp, importers) {
            const char *f = cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(imp, "file"));
            const cJSON *l = cJSON_GetObjectItemCaseSensitive(imp, "line");
            if (f)
                tt_strbuf_appendf(&sb, "- `%s:%d`\n", f,
                    (l && cJSON_IsNumber(l)) ? (int)l->valuedouble : 0);
        }
    }

    /* Callers section */
    const cJSON *callers = cJSON_GetObjectItemCaseSensitive(result, "callers");
    if (callers && cJSON_GetArraySize(callers) > 0) {
        tt_strbuf_append_str(&sb, "\n## Callers\n\n");
        tt_strbuf_append_str(&sb, "| Kind | Name | File | Line |\n");
        tt_strbuf_append_str(&sb, "|------|------|------|------|\n");
        const cJSON *c;
        cJSON_ArrayForEach(c, callers) {
            const char *cn = cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(c, "name"));
            const char *ck = cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(c, "kind"));
            const char *cf = cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(c, "file"));
            const cJSON *cl = cJSON_GetObjectItemCaseSensitive(c, "line");
            tt_strbuf_appendf(&sb, "| %s | %s | %s | %d |\n",
                              ck ? ck : "", cn ? cn : "", cf ? cf : "",
                              (cl && cJSON_IsNumber(cl)) ? (int)cl->valuedouble : 0);
        }
    }

    char *md = sb.data;
    /* Don't free sb.data — caller owns it */
    return md;
}

int tt_cmd_inspect_bundle(tt_cli_opts_t *opts)
{
    cJSON *result = tt_cmd_inspect_bundle_exec(opts);
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

    if (opts->format && strcmp(opts->format, "markdown") == 0) {
        char *md = tt_bundle_render_markdown(result);
        if (md) {
            printf("%s", md);
            fflush(stdout);
            free(md);
        }
        cJSON_Delete(result);
        return 0;
    }

    tt_json_print(result);
    cJSON_Delete(result);
    return 0;
}
