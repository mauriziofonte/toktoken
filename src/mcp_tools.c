/*
 * mcp_tools.c -- MCP tool definitions, schemas, and execution bridge.
 *
 * All 21 TokToken tools: schema generation + argument→CLI bridge.
 * Each execute function converts cJSON arguments into tt_cli_opts_t
 * and calls the corresponding tt_cmd_*_exec() function.
 */

#include "mcp_tools.h"
#include "mcp_server.h"
#include "cli.h"
#include "cmd_index.h"
#include "cmd_search.h"
#include "cmd_inspect.h"
#include "cmd_manage.h"
#include "cmd_github.h"
#include "cmd_find.h"
#include "cmd_bundle.h"
#include "cmd_help.h"
#include "error.h"

#include <stdlib.h>
#include <string.h>

/* ---- Progress callback for MCP ---- */

static void mcp_progress_cb(void *ctx, int64_t done, int64_t total)
{
    struct tt_mcp_server_t *srv = (struct tt_mcp_server_t *)ctx;
    mcp_send_progress(srv, done, total, "Indexing files...");
}

/* ---- Schema builder helpers ---- */

static void add_string_prop(cJSON *props, const char *name, const char *desc)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", "string");
    cJSON_AddStringToObject(p, "description", desc);
    cJSON_AddItemToObject(props, name, p);
}

static void add_integer_prop(cJSON *props, const char *name, const char *desc)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", "integer");
    cJSON_AddStringToObject(p, "description", desc);
    cJSON_AddItemToObject(props, name, p);
}

static void add_boolean_prop(cJSON *props, const char *name, const char *desc)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", "boolean");
    cJSON_AddStringToObject(p, "description", desc);
    cJSON_AddItemToObject(props, name, p);
}

static cJSON *make_schema_with_path_only(void)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "object");
    cJSON *props = cJSON_CreateObject();
    add_string_prop(props, "path", "Project root path (default: cwd)");
    cJSON_AddItemToObject(s, "properties", props);
    return s;
}

/* ---- Common opts builder ---- */

static void init_opts_from_args(tt_cli_opts_t *opts,
                                const tt_mcp_server_t *srv,
                                const cJSON *arguments)
{
    memset(opts, 0, sizeof(*opts));
    opts->truncate_width = 120;

    const char *path = NULL;
    if (arguments)
        path = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(arguments, "path"));
    opts->path = path ? path : srv->project_root;
}

/* ---- Schema functions ---- */

static cJSON *schema_codebase_detect(void) { return make_schema_with_path_only(); }
static cJSON *schema_index_create(void)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "object");
    cJSON *props = cJSON_CreateObject();
    add_string_prop(props, "path", "Project root path (default: cwd)");
    add_boolean_prop(props, "full",
                     "Disable smart filter: index all file types and vendored subdirectories");
    cJSON_AddItemToObject(s, "properties", props);
    return s;
}
static cJSON *schema_index_update(void)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "object");
    cJSON *props = cJSON_CreateObject();
    add_string_prop(props, "path", "Project root path (default: cwd)");
    add_boolean_prop(props, "full",
                     "Disable smart filter: index all file types and vendored subdirectories");
    cJSON_AddItemToObject(s, "properties", props);
    return s;
}

static cJSON *schema_search_symbols(void)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "object");
    cJSON *props = cJSON_CreateObject();

    add_string_prop(props, "query", "Search query to find symbols by name");
    add_string_prop(props, "path", "Project root path (default: cwd)");
    add_string_prop(props, "kind",
                    "Comma-separated symbol kinds. "
                    "Code: class,method,function,property,variable,interface,"
                    "trait,constant,enum,namespace,type,directive. "
                    "Documentation: chapter,section,subsection");
    add_integer_prop(props, "limit", "Maximum number of output results");
    add_integer_prop(props, "max", "Maximum index query results (default: 50, max: 200)");
    add_string_prop(props, "filter",
                    "Include only files matching substring (pipe-separated OR)");
    add_string_prop(props, "exclude",
                    "Exclude files matching substring (pipe-separated OR)");
    add_boolean_prop(props, "compact",
                     "Use compact output format (shorter keys, ~47% smaller)");
    add_boolean_prop(props, "unique", "Deduplicate results by file:line:name");
    add_boolean_prop(props, "count", "Return only the count, not the results");
    add_string_prop(props, "sort", "Sort order: score, name, file, line, kind");
    add_boolean_prop(props, "no_sig", "Omit signatures from results");
    add_boolean_prop(props, "no_summary", "Omit summaries from results");
    add_string_prop(props, "language", "Filter by programming language");
    add_string_prop(props, "file", "Filter by file glob pattern");
    add_boolean_prop(props, "debug",
                     "Include per-field score breakdown (name, sig, summary, keyword, docstring) for each result");
    add_string_prop(props, "detail_level",
                    "Result detail: compact (id/name/kind/file/line/byte_length), "
                    "standard (default, + qname/sig/summary), full (+ end_line/docstring)");
    add_integer_prop(props, "token_budget",
                     "Max token budget for results. Results are included until budget is exhausted "
                     "(byte_length / 4). At least 1 result is always returned.");
    add_string_prop(props, "scope_imports_of",
                    "Scope search to files imported by this file (forward import graph)");
    add_string_prop(props, "scope_importers_of",
                    "Scope search to files that import this file (reverse import graph)");

    cJSON_AddItemToObject(s, "properties", props);

    cJSON *req = cJSON_CreateArray();
    cJSON_AddItemToArray(req, cJSON_CreateString("query"));
    cJSON_AddItemToObject(s, "required", req);

    return s;
}

static cJSON *schema_search_text(void)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "object");
    cJSON *props = cJSON_CreateObject();

    add_string_prop(props, "query", "Text to search for (pipe-separated OR)");
    add_string_prop(props, "path", "Project root path (default: cwd)");
    add_integer_prop(props, "limit", "Maximum number of output results");
    add_integer_prop(props, "max", "Maximum search results (default: 100, max: 500)");
    add_string_prop(props, "filter",
                    "Include only files matching substring (pipe-separated OR)");
    add_string_prop(props, "exclude",
                    "Exclude files matching substring (pipe-separated OR)");
    add_string_prop(props, "group_by", "Group results by: file");
    add_integer_prop(props, "context", "Number of context lines around each match");
    add_boolean_prop(props, "count", "Return only the count, not the results");
    add_boolean_prop(props, "case_sensitive", "Use case-sensitive matching");
    add_boolean_prop(props, "is_regex", "Treat query as a regex pattern");
    add_string_prop(props, "file", "Filter by file glob pattern");

    cJSON_AddItemToObject(s, "properties", props);

    cJSON *req = cJSON_CreateArray();
    cJSON_AddItemToArray(req, cJSON_CreateString("query"));
    cJSON_AddItemToObject(s, "required", req);

    return s;
}

static cJSON *schema_inspect_outline(void)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "object");
    cJSON *props = cJSON_CreateObject();

    add_string_prop(props, "file", "Relative file path to inspect");
    add_string_prop(props, "path", "Project root path (default: cwd)");
    add_string_prop(props, "kind",
                    "Comma-separated symbol kinds. "
                    "Code: class,method,function,property,variable,interface,"
                    "trait,constant,enum,namespace,type,directive. "
                    "Documentation: chapter,section,subsection");
    add_boolean_prop(props, "compact", "Use compact output format");
    add_boolean_prop(props, "no_sig", "Omit signatures");
    add_boolean_prop(props, "no_summary", "Omit summaries");

    cJSON_AddItemToObject(s, "properties", props);

    cJSON *req = cJSON_CreateArray();
    cJSON_AddItemToArray(req, cJSON_CreateString("file"));
    cJSON_AddItemToObject(s, "required", req);

    return s;
}

static cJSON *schema_inspect_symbol(void)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "object");
    cJSON *props = cJSON_CreateObject();

    add_string_prop(props, "id", "Symbol ID or comma-separated IDs");
    add_string_prop(props, "path", "Project root path (default: cwd)");
    add_boolean_prop(props, "compact", "Use compact output format");
    add_integer_prop(props, "context", "Extra context lines around symbol source");

    cJSON_AddItemToObject(s, "properties", props);

    cJSON *req = cJSON_CreateArray();
    cJSON_AddItemToArray(req, cJSON_CreateString("id"));
    cJSON_AddItemToObject(s, "required", req);

    return s;
}

static cJSON *schema_inspect_file(void)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "object");
    cJSON *props = cJSON_CreateObject();

    add_string_prop(props, "file", "Relative file path");
    add_string_prop(props, "path", "Project root path (default: cwd)");
    add_string_prop(props, "lines", "Line range \"start-end\"");

    cJSON_AddItemToObject(s, "properties", props);

    cJSON *req = cJSON_CreateArray();
    cJSON_AddItemToArray(req, cJSON_CreateString("file"));
    cJSON_AddItemToObject(s, "required", req);

    return s;
}

static cJSON *schema_inspect_tree(void)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "object");
    cJSON *props = cJSON_CreateObject();

    add_string_prop(props, "path", "Project root path (default: cwd)");
    add_integer_prop(props, "depth", "Maximum tree depth");
    add_string_prop(props, "language", "Filter by programming language");

    cJSON_AddItemToObject(s, "properties", props);
    return s;
}

static cJSON *schema_stats(void) { return make_schema_with_path_only(); }

static cJSON *schema_projects_list(void)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "object");
    cJSON *props = cJSON_CreateObject();
    cJSON_AddItemToObject(s, "properties", props);
    return s;
}

static cJSON *schema_cache_clear(void)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "object");
    cJSON *props = cJSON_CreateObject();

    add_string_prop(props, "path", "Project root path (default: cwd)");
    add_boolean_prop(props, "all", "Clear all indexes");
    add_boolean_prop(props, "force", "Required confirmation for --all (purge entire data directory)");

    cJSON_AddItemToObject(s, "properties", props);
    return s;
}

/* ---- Execute functions ---- */

static cJSON *execute_codebase_detect(struct tt_mcp_server_t *srv,
                                      const cJSON *arguments)
{
    tt_cli_opts_t opts;
    init_opts_from_args(&opts, srv, arguments);

    int exit_code = 0;
    cJSON *result = tt_cmd_codebase_detect_exec(&opts, &exit_code);
    if (!result)
        return mcp_tool_error(tt_error_get());
    return result;
}

static cJSON *execute_index_create(struct tt_mcp_server_t *srv,
                                   const cJSON *arguments)
{
    tt_cli_opts_t opts;
    init_opts_from_args(&opts, srv, arguments);

    /* Extract full parameter */
    if (arguments)
    {
        const cJSON *full_item = cJSON_GetObjectItemCaseSensitive(arguments, "full");
        if (cJSON_IsTrue(full_item))
            opts.full = true;
    }

    /* Wire progress callback if client sent a progressToken */
    if (srv->progress_token)
    {
        opts.progress_cb = mcp_progress_cb;
        opts.progress_ctx = srv;
    }

    /* If index is created, invalidate our cached db handle */
    if (srv->db)
    {
        tt_database_close(srv->db);
        free(srv->db);
        srv->db = NULL;
    }

    cJSON *result = tt_cmd_index_create_exec(&opts);
    if (!result)
        return mcp_tool_error(tt_error_get());
    return result;
}

static cJSON *execute_index_update(struct tt_mcp_server_t *srv,
                                   const cJSON *arguments)
{
    tt_cli_opts_t opts;
    init_opts_from_args(&opts, srv, arguments);

    /* Extract full parameter */
    if (arguments)
    {
        const cJSON *full_item = cJSON_GetObjectItemCaseSensitive(arguments, "full");
        if (cJSON_IsTrue(full_item))
            opts.full = true;
    }

    /* Wire progress callback if client sent a progressToken */
    if (srv->progress_token)
    {
        opts.progress_cb = mcp_progress_cb;
        opts.progress_ctx = srv;
    }

    /* Invalidate cached db handle */
    if (srv->db)
    {
        tt_database_close(srv->db);
        free(srv->db);
        srv->db = NULL;
    }

    cJSON *result = tt_cmd_index_update_exec(&opts);
    if (!result)
        return mcp_tool_error(tt_error_get());
    return result;
}

static cJSON *execute_search_symbols(struct tt_mcp_server_t *srv,
                                     const cJSON *arguments)
{
    const char *query = NULL;
    if (arguments)
        query = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(arguments, "query"));

    if (!query || query[0] == '\0')
        return mcp_tool_error("Missing required parameter: query");

    tt_cli_opts_t opts;
    init_opts_from_args(&opts, srv, arguments);

    /* Set the query as first positional argument */
    opts.search = query;

    /* Extract optional parameters */
    if (arguments)
    {
        const char *v;
        v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(arguments, "kind"));
        if (v)
            opts.kind = v;
        v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(arguments, "filter"));
        if (v)
            opts.filter = v;
        v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(arguments, "exclude"));
        if (v)
            opts.exclude = v;
        v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(arguments, "sort"));
        if (v)
            opts.sort = v;
        v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(arguments, "language"));
        if (v)
            opts.language = v;
        v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(arguments, "file"));
        if (v)
            opts.file_glob = v;

        opts.limit = mcp_get_int_or_default(arguments, "limit", 0);
        opts.max = mcp_get_int_or_default(arguments, "max", 50);
        opts.compact = mcp_get_bool(arguments, "compact");
        opts.unique = mcp_get_bool(arguments, "unique");
        opts.count = mcp_get_bool(arguments, "count");
        opts.no_sig = mcp_get_bool(arguments, "no_sig");
        opts.no_summary = mcp_get_bool(arguments, "no_summary");
        opts.debug = mcp_get_bool(arguments, "debug");

        v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(arguments, "detail_level"));
        if (v)
            opts.detail = v;
        opts.token_budget = mcp_get_int_or_default(arguments, "token_budget", 0);

        v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(arguments, "scope_imports_of"));
        if (v) opts.scope_imports_of = v;
        v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(arguments, "scope_importers_of"));
        if (v) opts.scope_importers_of = v;
    }

    cJSON *result = tt_cmd_search_symbols_exec(&opts);
    if (!result)
        return mcp_tool_error(tt_error_get());
    return result;
}

static cJSON *execute_search_text(struct tt_mcp_server_t *srv,
                                  const cJSON *arguments)
{
    const char *query = NULL;
    if (arguments)
        query = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(arguments, "query"));

    if (!query || query[0] == '\0')
        return mcp_tool_error("Missing required parameter: query");

    tt_cli_opts_t opts;
    init_opts_from_args(&opts, srv, arguments);

    opts.search = query;

    if (arguments)
    {
        const char *v;
        v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(arguments, "filter"));
        if (v)
            opts.filter = v;
        v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(arguments, "exclude"));
        if (v)
            opts.exclude = v;
        v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(arguments, "group_by"));
        if (v)
            opts.group_by = v;
        v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(arguments, "file"));
        if (v)
            opts.file_glob = v;

        opts.limit = mcp_get_int_or_default(arguments, "limit", 0);
        opts.max = mcp_get_int_or_default(arguments, "max", 100);
        opts.context = mcp_get_int_or_default(arguments, "context", 0);
        opts.count = mcp_get_bool(arguments, "count");
        opts.case_sensitive = mcp_get_bool(arguments, "case_sensitive");
        opts.regex = mcp_get_bool(arguments, "is_regex");
    }

    cJSON *result = tt_cmd_search_text_exec(&opts);
    if (!result)
        return mcp_tool_error(tt_error_get());
    return result;
}

static cJSON *execute_inspect_outline(struct tt_mcp_server_t *srv,
                                      const cJSON *arguments)
{
    const char *file = NULL;
    if (arguments)
        file = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(arguments, "file"));

    if (!file || file[0] == '\0')
        return mcp_tool_error("Missing required parameter: file");

    tt_cli_opts_t opts;
    init_opts_from_args(&opts, srv, arguments);

    /* file goes as first positional */
    const char *positional[1] = {file};
    opts.positional = positional;
    opts.positional_count = 1;

    if (arguments)
    {
        const char *v;
        v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(arguments, "kind"));
        if (v)
            opts.kind = v;
        opts.compact = mcp_get_bool(arguments, "compact");
        opts.no_sig = mcp_get_bool(arguments, "no_sig");
        opts.no_summary = mcp_get_bool(arguments, "no_summary");
    }

    cJSON *result = tt_cmd_inspect_outline_exec(&opts);
    if (!result)
        return mcp_tool_error(tt_error_get());
    return result;
}

static cJSON *execute_inspect_symbol(struct tt_mcp_server_t *srv,
                                     const cJSON *arguments)
{
    const char *id = NULL;
    if (arguments)
        id = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(arguments, "id"));

    if (!id || id[0] == '\0')
        return mcp_tool_error("Missing required parameter: id");

    tt_cli_opts_t opts;
    init_opts_from_args(&opts, srv, arguments);

    /* Split comma-separated IDs into positional args */
    char *id_buf = strdup(id);
    const char *ids[64];
    int id_count = 0;
    char *tok = strtok(id_buf, ",");
    while (tok && id_count < 64)
    {
        while (*tok == ' ')
            tok++;
        ids[id_count++] = tok;
        tok = strtok(NULL, ",");
    }
    opts.positional = ids;
    opts.positional_count = id_count;

    if (arguments)
    {
        opts.compact = mcp_get_bool(arguments, "compact");
        opts.context = mcp_get_int_or_default(arguments, "context", 0);
    }

    int exit_code = 0;
    cJSON *result = tt_cmd_inspect_symbol_exec(&opts, &exit_code);
    free(id_buf);
    if (!result)
        return mcp_tool_error(tt_error_get());
    return result;
}

static cJSON *execute_inspect_file(struct tt_mcp_server_t *srv,
                                   const cJSON *arguments)
{
    const char *file = NULL;
    if (arguments)
        file = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(arguments, "file"));

    if (!file || file[0] == '\0')
        return mcp_tool_error("Missing required parameter: file");

    tt_cli_opts_t opts;
    init_opts_from_args(&opts, srv, arguments);

    const char *positional[1] = {file};
    opts.positional = positional;
    opts.positional_count = 1;

    if (arguments)
    {
        const char *v;
        v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(arguments, "lines"));
        if (v)
            opts.lines = v;
    }

    cJSON *result = tt_cmd_inspect_file_exec(&opts);
    if (!result)
        return mcp_tool_error(tt_error_get());
    return result;
}

static cJSON *execute_inspect_tree(struct tt_mcp_server_t *srv,
                                   const cJSON *arguments)
{
    tt_cli_opts_t opts;
    init_opts_from_args(&opts, srv, arguments);

    if (arguments)
    {
        const char *v;
        v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(arguments, "language"));
        if (v)
            opts.language = v;
        opts.depth = mcp_get_int_or_default(arguments, "depth", 0);
    }

    /* MCP always uses JSON — table/jsonl print to stdout which IS the transport */
    opts.format = NULL;

    cJSON *result = tt_cmd_inspect_tree_exec(&opts);
    if (!result)
        return mcp_tool_error(tt_error_get());
    return result;
}

static cJSON *execute_stats(struct tt_mcp_server_t *srv,
                            const cJSON *arguments)
{
    tt_cli_opts_t opts;
    init_opts_from_args(&opts, srv, arguments);

    cJSON *result = tt_cmd_stats_exec(&opts);
    if (!result)
        return mcp_tool_error(tt_error_get());
    return result;
}

static cJSON *execute_projects_list(struct tt_mcp_server_t *srv,
                                    const cJSON *arguments)
{
    (void)arguments;
    tt_cli_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.truncate_width = 120;
    opts.path = srv->project_root;

    cJSON *result = tt_cmd_projects_list_exec(&opts);
    if (!result)
        return mcp_tool_error(tt_error_get());
    return result;
}

static cJSON *execute_cache_clear(struct tt_mcp_server_t *srv,
                                  const cJSON *arguments)
{
    tt_cli_opts_t opts;
    init_opts_from_args(&opts, srv, arguments);

    if (arguments)
    {
        opts.all = mcp_get_bool(arguments, "all");
        opts.force = mcp_get_bool(arguments, "force");
    }

    /* If cache is cleared, invalidate db handle */
    if (srv->db)
    {
        tt_database_close(srv->db);
        free(srv->db);
        srv->db = NULL;
    }

    cJSON *result = tt_cmd_cache_clear_exec(&opts);
    if (!result)
        return mcp_tool_error(tt_error_get());
    return result;
}

/* ---- index_github ---- */

static cJSON *schema_index_github(void)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "object");
    cJSON *props = cJSON_CreateObject();

    add_string_prop(props, "repository",
                    "GitHub repository in owner/repo format");
    add_string_prop(props, "branch",
                    "Branch to clone (default: repo default branch)");
    add_boolean_prop(props, "force",
                     "Force re-clone even if already present");
    add_boolean_prop(props, "full_clone",
                     "Full clone instead of shallow (--depth 1)");

    cJSON_AddItemToObject(s, "properties", props);

    cJSON *req = cJSON_CreateArray();
    cJSON_AddItemToArray(req, cJSON_CreateString("repository"));
    cJSON_AddItemToObject(s, "required", req);

    return s;
}

static cJSON *execute_index_github(struct tt_mcp_server_t *srv,
                                   const cJSON *arguments)
{
    const char *repository = NULL;
    if (arguments)
        repository = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(arguments, "repository"));

    if (!repository || repository[0] == '\0')
        return mcp_tool_error("Missing required parameter: repository");

    tt_cli_opts_t opts;
    init_opts_from_args(&opts, srv, arguments);

    /* Set repository as first positional */
    const char *positional[1] = {repository};
    opts.positional = positional;
    opts.positional_count = 1;

    if (arguments)
    {
        const char *v;
        v = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(arguments, "branch"));
        if (v)
            opts.branch = v;

        opts.force = mcp_get_bool(arguments, "force");
        opts.full_clone = mcp_get_bool(arguments, "full_clone");
    }

    /* Invalidate cached db handle since we may create a new index */
    if (srv->db)
    {
        tt_database_close(srv->db);
        free(srv->db);
        srv->db = NULL;
    }

    cJSON *result = tt_cmd_index_github_exec(&opts);
    if (!result)
        return mcp_tool_error(tt_error_get());
    return result;
}

/* ---- inspect:bundle ---- */

static cJSON *schema_inspect_bundle(void)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "object");
    cJSON *props = cJSON_CreateObject();

    add_string_prop(props, "id", "Symbol ID (from search_symbols results). Comma-separated for multi-symbol bundles (max 20).");
    add_string_prop(props, "path", "Project root path (default: cwd)");
    add_boolean_prop(props, "compact", "Use compact output format");
    add_boolean_prop(props, "full",
                     "Include importers (files that depend on this symbol's file)");
    add_boolean_prop(props, "no_sig", "Omit signatures from outline");
    add_boolean_prop(props, "include_callers",
                     "Include callers: symbols in other files that reference this symbol");
    add_string_prop(props, "output_format",
                    "Output format: 'json' (default) or 'markdown'");

    cJSON_AddItemToObject(s, "properties", props);

    cJSON *req = cJSON_CreateArray();
    cJSON_AddItemToArray(req, cJSON_CreateString("id"));
    cJSON_AddItemToObject(s, "required", req);

    return s;
}

static cJSON *execute_inspect_bundle(struct tt_mcp_server_t *srv,
                                      const cJSON *arguments)
{
    const char *id = NULL;
    if (arguments)
        id = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(arguments, "id"));

    if (!id || id[0] == '\0')
        return mcp_tool_error("Missing required parameter: id");

    tt_cli_opts_t opts;
    init_opts_from_args(&opts, srv, arguments);

    const char *positional[1] = {id};
    opts.positional = positional;
    opts.positional_count = 1;

    const char *output_format = NULL;
    if (arguments) {
        opts.compact = mcp_get_bool(arguments, "compact");
        opts.full = mcp_get_bool(arguments, "full");
        opts.no_sig = mcp_get_bool(arguments, "no_sig");
        opts.include_callers = mcp_get_bool(arguments, "include_callers");
        output_format = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(arguments, "output_format"));
    }

    cJSON *result = tt_cmd_inspect_bundle_exec(&opts);
    if (!result)
        return mcp_tool_error(tt_error_get());

    if (output_format && strcmp(output_format, "markdown") == 0) {
        char *md = tt_bundle_render_markdown(result);
        cJSON_Delete(result);
        if (!md)
            return mcp_tool_error("Failed to render markdown");
        cJSON *wrap = cJSON_CreateObject();
        cJSON_AddStringToObject(wrap, "content", md);
        free(md);
        return wrap;
    }

    return result;
}

/* ---- find:importers / find:references ---- */

static cJSON *schema_find_importers(void)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "object");
    cJSON *props = cJSON_CreateObject();
    add_string_prop(props, "file", "File path to find importers for");
    add_boolean_prop(props, "has_importers",
                     "Enrich each result with a has_importers boolean "
                     "indicating whether the importing file is itself imported");
    add_string_prop(props, "path", "Project root path (default: cwd)");
    cJSON_AddItemToObject(s, "properties", props);

    cJSON *req = cJSON_CreateArray();
    cJSON_AddItemToArray(req, cJSON_CreateString("file"));
    cJSON_AddItemToObject(s, "required", req);
    return s;
}

static cJSON *schema_find_references(void)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "object");
    cJSON *props = cJSON_CreateObject();
    add_string_prop(props, "identifier", "Symbol or module name to find references for");
    add_string_prop(props, "path", "Project root path (default: cwd)");
    cJSON_AddItemToObject(s, "properties", props);

    cJSON *req = cJSON_CreateArray();
    cJSON_AddItemToArray(req, cJSON_CreateString("identifier"));
    cJSON_AddItemToObject(s, "required", req);
    return s;
}

static cJSON *execute_find_importers(struct tt_mcp_server_t *srv,
                                     const cJSON *arguments)
{
    const char *file = NULL;
    if (arguments)
        file = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(arguments, "file"));

    if (!file || file[0] == '\0')
        return mcp_tool_error("Missing required parameter: file");

    tt_cli_opts_t opts;
    init_opts_from_args(&opts, srv, arguments);
    opts.search = file;
    if (arguments)
        opts.has_importers = mcp_get_bool(arguments, "has_importers");

    cJSON *result = tt_cmd_find_importers_exec(&opts);
    if (!result)
        return mcp_tool_error(tt_error_get());
    return result;
}

static cJSON *execute_find_references(struct tt_mcp_server_t *srv,
                                      const cJSON *arguments)
{
    const char *identifier = NULL;
    if (arguments)
        identifier = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(arguments, "identifier"));

    if (!identifier || identifier[0] == '\0')
        return mcp_tool_error("Missing required parameter: identifier");

    tt_cli_opts_t opts;
    init_opts_from_args(&opts, srv, arguments);
    opts.search = identifier;

    cJSON *result = tt_cmd_find_references_exec(&opts);
    if (!result)
        return mcp_tool_error(tt_error_get());
    return result;
}

/* ---- find:callers ---- */

static cJSON *schema_find_callers(void)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "object");
    cJSON *props = cJSON_CreateObject();
    add_string_prop(props, "id", "Symbol ID to find callers for");
    add_string_prop(props, "path", "Project root path (default: cwd)");
    add_integer_prop(props, "limit", "Maximum number of results");
    cJSON_AddItemToObject(s, "properties", props);

    cJSON *req = cJSON_CreateArray();
    cJSON_AddItemToArray(req, cJSON_CreateString("id"));
    cJSON_AddItemToObject(s, "required", req);
    return s;
}

static cJSON *execute_find_callers(struct tt_mcp_server_t *srv,
                                   const cJSON *arguments)
{
    const char *id = NULL;
    if (arguments)
        id = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(arguments, "id"));

    if (!id || id[0] == '\0')
        return mcp_tool_error("Missing required parameter: id");

    tt_cli_opts_t opts;
    init_opts_from_args(&opts, srv, arguments);

    const char *positional[1] = {id};
    opts.positional = positional;
    opts.positional_count = 1;

    if (arguments)
        opts.limit = mcp_get_int_or_default(arguments, "limit", 0);

    cJSON *result = tt_cmd_find_callers_exec(&opts);
    if (!result)
        return mcp_tool_error(tt_error_get());
    return result;
}

/* ---- search:cooccurrence ---- */

static cJSON *schema_search_cooccurrence(void)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "object");
    cJSON *props = cJSON_CreateObject();
    add_string_prop(props, "query",
                    "Two symbol names separated by comma (e.g. \"Logger,Database\")");
    add_string_prop(props, "path", "Project root path (default: cwd)");
    add_string_prop(props, "language", "Filter by programming language");
    add_integer_prop(props, "limit", "Maximum number of results");
    cJSON_AddItemToObject(s, "properties", props);

    cJSON *req = cJSON_CreateArray();
    cJSON_AddItemToArray(req, cJSON_CreateString("query"));
    cJSON_AddItemToObject(s, "required", req);
    return s;
}

static cJSON *execute_search_cooccurrence(struct tt_mcp_server_t *srv,
                                          const cJSON *arguments)
{
    const char *query = NULL;
    if (arguments)
        query = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(arguments, "query"));

    if (!query || query[0] == '\0')
        return mcp_tool_error("Missing required parameter: query");

    tt_cli_opts_t opts;
    init_opts_from_args(&opts, srv, arguments);
    opts.search = query;

    if (arguments)
    {
        const char *v;
        v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(arguments, "language"));
        if (v)
            opts.language = v;
        opts.limit = mcp_get_int_or_default(arguments, "limit", 0);
    }

    cJSON *result = tt_cmd_search_cooccurrence_exec(&opts);
    if (!result)
        return mcp_tool_error(tt_error_get());
    return result;
}

/* ---- search:similar ---- */

static cJSON *schema_search_similar(void)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "object");
    cJSON *props = cJSON_CreateObject();
    add_string_prop(props, "id", "Symbol ID to find similar symbols for");
    add_string_prop(props, "path", "Project root path (default: cwd)");
    add_integer_prop(props, "limit", "Maximum number of results");
    cJSON_AddItemToObject(s, "properties", props);

    cJSON *req = cJSON_CreateArray();
    cJSON_AddItemToArray(req, cJSON_CreateString("id"));
    cJSON_AddItemToObject(s, "required", req);
    return s;
}

static cJSON *execute_search_similar(struct tt_mcp_server_t *srv,
                                     const cJSON *arguments)
{
    const char *id = NULL;
    if (arguments)
        id = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(arguments, "id"));

    if (!id || id[0] == '\0')
        return mcp_tool_error("Missing required parameter: id");

    tt_cli_opts_t opts;
    init_opts_from_args(&opts, srv, arguments);

    const char *positional[1] = {id};
    opts.positional = positional;
    opts.positional_count = 1;

    if (arguments)
        opts.limit = mcp_get_int_or_default(arguments, "limit", 0);

    cJSON *result = tt_cmd_search_similar_exec(&opts);
    if (!result)
        return mcp_tool_error(tt_error_get());
    return result;
}

/* ---- inspect:dependencies ---- */

static cJSON *schema_inspect_dependencies(void)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "object");
    cJSON *props = cJSON_CreateObject();
    add_string_prop(props, "file", "File path to trace dependencies for");
    add_string_prop(props, "path", "Project root path (default: cwd)");
    add_integer_prop(props, "depth", "Maximum recursion depth (default: 3, max: 10)");
    cJSON_AddItemToObject(s, "properties", props);

    cJSON *req = cJSON_CreateArray();
    cJSON_AddItemToArray(req, cJSON_CreateString("file"));
    cJSON_AddItemToObject(s, "required", req);
    return s;
}

static cJSON *execute_inspect_dependencies(struct tt_mcp_server_t *srv,
                                           const cJSON *arguments)
{
    const char *file = NULL;
    if (arguments)
        file = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(arguments, "file"));

    if (!file || file[0] == '\0')
        return mcp_tool_error("Missing required parameter: file");

    tt_cli_opts_t opts;
    init_opts_from_args(&opts, srv, arguments);

    const char *positional[1] = {file};
    opts.positional = positional;
    opts.positional_count = 1;

    if (arguments)
        opts.depth = mcp_get_int_or_default(arguments, "depth", 0);

    cJSON *result = tt_cmd_inspect_dependencies_exec(&opts);
    if (!result)
        return mcp_tool_error(tt_error_get());
    return result;
}

/* ---- inspect:hierarchy ---- */

static cJSON *schema_inspect_hierarchy(void)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "object");
    cJSON *props = cJSON_CreateObject();
    add_string_prop(props, "file", "File path to show hierarchy for");
    add_string_prop(props, "path", "Project root path (default: cwd)");
    add_string_prop(props, "language", "Filter by programming language");
    add_integer_prop(props, "limit", "Maximum number of results");
    cJSON_AddItemToObject(s, "properties", props);

    cJSON *req = cJSON_CreateArray();
    cJSON_AddItemToArray(req, cJSON_CreateString("file"));
    cJSON_AddItemToObject(s, "required", req);
    return s;
}

static cJSON *execute_inspect_hierarchy(struct tt_mcp_server_t *srv,
                                        const cJSON *arguments)
{
    const char *file = NULL;
    if (arguments)
        file = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(arguments, "file"));

    if (!file || file[0] == '\0')
        return mcp_tool_error("Missing required parameter: file");

    tt_cli_opts_t opts;
    init_opts_from_args(&opts, srv, arguments);

    const char *positional[1] = {file};
    opts.positional = positional;
    opts.positional_count = 1;

    if (arguments)
    {
        const char *v;
        v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(arguments, "language"));
        if (v)
            opts.language = v;
        opts.limit = mcp_get_int_or_default(arguments, "limit", 0);
    }

    cJSON *result = tt_cmd_inspect_hierarchy_exec(&opts);
    if (!result)
        return mcp_tool_error(tt_error_get());
    return result;
}

/* ---- inspect:cycles ---- */

static cJSON *schema_inspect_cycles(void)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "object");
    cJSON *props = cJSON_CreateObject();

    add_string_prop(props, "path", "Project root path (default: cwd)");
    add_boolean_prop(props, "cross_dir",
                     "Show only cross-directory cycles (default: false)");
    add_integer_prop(props, "min_length",
                     "Minimum cycle length to include (default: 0, no filter)");

    cJSON_AddItemToObject(s, "properties", props);
    return s;
}

static cJSON *execute_inspect_cycles(struct tt_mcp_server_t *srv,
                                     const cJSON *arguments)
{
    tt_cli_opts_t opts;
    init_opts_from_args(&opts, srv, arguments);

    if (arguments) {
        opts.cross_dir = mcp_get_bool(arguments, "cross_dir");
        opts.min_length = mcp_get_int_or_default(arguments, "min_length", 0);
    }

    cJSON *result = tt_cmd_inspect_cycles_exec(&opts);
    if (!result)
        return mcp_tool_error(tt_error_get());
    return result;
}

static cJSON *schema_index_file(void)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "object");
    cJSON *props = cJSON_CreateObject();

    add_string_prop(props, "file", "File path to reindex (relative to project root)");
    add_string_prop(props, "path", "Project root path (default: cwd)");

    cJSON_AddItemToObject(s, "properties", props);

    cJSON *req = cJSON_CreateArray();
    cJSON_AddItemToArray(req, cJSON_CreateString("file"));
    cJSON_AddItemToObject(s, "required", req);
    return s;
}

static cJSON *execute_index_file(struct tt_mcp_server_t *srv,
                                 const cJSON *arguments)
{
    tt_cli_opts_t opts;
    init_opts_from_args(&opts, srv, arguments);

    const char *file = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(arguments, "file"));
    if (!file || !file[0])
        return mcp_tool_error("Missing required parameter: file");

    opts.search = file;

    cJSON *result = tt_cmd_index_file_exec(&opts);
    if (!result)
        return mcp_tool_error(tt_error_get());
    return result;
}

static cJSON *schema_find_dead(void)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "object");
    cJSON *props = cJSON_CreateObject();

    add_string_prop(props, "path", "Project root path (default: cwd)");
    add_string_prop(props, "kind",
                    "Filter by symbol kind (comma-separated). "
                    "Code: function,class,method,... "
                    "Documentation: chapter,section,subsection");
    add_string_prop(props, "language", "Filter by language");
    add_string_prop(props, "exclude", "Exclude files matching pattern (pipe-separated)");
    add_boolean_prop(props, "exclude_tests", "Exclude test files (default: false)");
    add_integer_prop(props, "limit", "Max results (default: 100)");

    cJSON_AddItemToObject(s, "properties", props);
    return s;
}

static cJSON *execute_find_dead(struct tt_mcp_server_t *srv,
                                const cJSON *arguments)
{
    tt_cli_opts_t opts;
    init_opts_from_args(&opts, srv, arguments);

    const char *v;
    v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(arguments, "kind"));
    if (v) opts.kind = v;
    v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(arguments, "language"));
    if (v) opts.language = v;
    v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(arguments, "exclude"));
    if (v) opts.exclude = v;

    cJSON *et = cJSON_GetObjectItemCaseSensitive(arguments, "exclude_tests");
    if (cJSON_IsBool(et))
        opts.exclude_tests = cJSON_IsTrue(et);

    opts.limit = mcp_get_int_or_default(arguments, "limit", 0);

    cJSON *result = tt_cmd_find_dead_exec(&opts);
    if (!result)
        return mcp_tool_error(tt_error_get());
    return result;
}

static cJSON *schema_inspect_blast_radius(void)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "object");
    cJSON *props = cJSON_CreateObject();

    add_string_prop(props, "id", "Symbol ID to analyze blast radius for (required)");
    add_integer_prop(props, "depth", "Max BFS depth on reverse import graph (1-3, default: 2)");
    add_string_prop(props, "path", "Project root path (default: cwd)");

    cJSON_AddItemToObject(s, "properties", props);

    cJSON *req = cJSON_CreateArray();
    cJSON_AddItemToArray(req, cJSON_CreateString("id"));
    cJSON_AddItemToObject(s, "required", req);
    return s;
}

static cJSON *execute_inspect_blast_radius(struct tt_mcp_server_t *srv,
                                           const cJSON *arguments)
{
    tt_cli_opts_t opts;
    init_opts_from_args(&opts, srv, arguments);

    const char *id = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(arguments, "id"));
    if (!id || !id[0])
        return mcp_tool_error("Missing required parameter: id");

    opts.search = id;

    int depth = mcp_get_int_or_default(arguments, "depth", 0);
    if (depth > 0)
        opts.depth = depth;

    cJSON *result = tt_cmd_inspect_blast_exec(&opts);
    if (!result)
        return mcp_tool_error(tt_error_get());
    return result;
}

/* ---- help ---- */

static cJSON *schema_help(void)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "object");
    cJSON *props = cJSON_AddObjectToObject(s, "properties");
    add_string_prop(props, "command",
                    "Tool name (e.g. 'search_symbols' or 'search:symbols'). "
                    "Omit to list all tools.");
    return s;
}

static cJSON *execute_help(struct tt_mcp_server_t *srv, const cJSON *arguments)
{
    (void)srv;
    const char *command = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(arguments, "command"));
    return tt_cmd_help_exec(command);
}

/* ---- Tool registration table ---- */

const tt_mcp_tool_t TT_MCP_TOOLS[] = {
    {"codebase_detect",
     "Detect if a directory contains indexable source code. Returns detected ecosystems and recommended action.",
     schema_codebase_detect,
     execute_codebase_detect},
    {"index_create",
     "Create a full symbol index from scratch. Required before any search or inspect tool can work. "
     "By default, the smart filter excludes non-code files (CSS, HTML, SVG, XML, YAML, TOML, GraphQL) "
     "and vendored subdirectories. Pass full=true to include all file types -- do this when the user's "
     "task involves excluded file types, or when a search returns 0 results on filtered extensions.",
     schema_index_create,
     execute_index_create},
    {"index_update",
     "Incrementally update the index. Only re-processes files with changed content hashes -- faster than index_create. "
     "Pass full=true to include all file types (same smart filter override as index_create).",
     schema_index_update,
     execute_index_update},
    {"search_symbols",
     "Search symbols by name (functions, classes, methods, headings, etc.). Returns IDs usable with inspect_symbol to retrieve full source code.",
     schema_search_symbols,
     execute_search_symbols},
    {"search_text",
     "Grep-like text search across indexed files. Use for string literals, comments, config values, or patterns that aren't symbol names.",
     schema_search_text,
     execute_search_text},
    {"inspect_outline",
     "Show the symbol outline of a file: nested functions, classes, methods, headings with line numbers and signatures.",
     schema_inspect_outline,
     execute_inspect_outline},
    {"inspect_symbol",
     "Retrieve full source code for one or more symbols by ID (from search_symbols results).",
     schema_inspect_symbol,
     execute_inspect_symbol},
    {"inspect_file",
     "Retrieve raw file content, optionally limited to a line range (e.g. lines=10-50).",
     schema_inspect_file,
     execute_inspect_file},
    {"inspect_tree",
     "Show directory tree of indexed source files. Useful for understanding project layout.",
     schema_inspect_tree,
     execute_inspect_tree},
    {"stats",
     "Project statistics: file/symbol counts, language breakdown, staleness, and cumulative token savings.",
     schema_stats,
     execute_stats},
    {"projects_list",
     "List all indexed projects with their paths, file counts, and index age.",
     schema_projects_list,
     execute_projects_list},
    {"cache_clear",
     "Delete the index for a project. Use 'all' parameter to clear all projects. Re-index required after.",
     schema_cache_clear,
     execute_cache_clear},
    {"index_github",
     "Clone and index a GitHub repository. Accepts owner/repo or full URL.",
     schema_index_github,
     execute_index_github},
    {"inspect_bundle",
     "Get a self-contained context bundle for a symbol: definition source, imports, file outline, and optionally importers. Replaces multi-tool round-trips.",
     schema_inspect_bundle,
     execute_inspect_bundle},
    {"find_importers",
     "Find all files that import a given file path. Traces the dependency graph for impact analysis.",
     schema_find_importers,
     execute_find_importers},
    {"find_references",
     "Find import statements referencing a given identifier or module across all indexed files.",
     schema_find_references,
     execute_find_references},
    {"find_callers",
     "Find symbols that likely call a given function/method. Cross-references import graph with symbol signatures. Heuristic: may include false positives.",
     schema_find_callers,
     execute_find_callers},
    {"search_cooccurrence",
     "Find symbols that co-occur in the same file. Discover architectural patterns (e.g. which files use both Logger and Database).",
     schema_search_cooccurrence,
     execute_search_cooccurrence},
    {"search_similar",
     "Find symbols similar to a given one by name keywords and summary. Useful for discovering related functions or alternative implementations.",
     schema_search_similar,
     execute_search_similar},
    {"inspect_dependencies",
     "Trace transitive import graph: find all files that depend on a given file, recursively up to a configurable depth.",
     schema_inspect_dependencies,
     execute_inspect_dependencies},
    {"inspect_hierarchy",
     "Show class/function hierarchy with parent-child relationships. Displays nested symbol structure from parent_id linkage.",
     schema_inspect_hierarchy,
     execute_inspect_hierarchy},
    {"inspect_cycles",
     "Detect circular import cycles in the codebase using Tarjan's SCC algorithm. Returns all file-level import cycles with cross-directory detection.",
     schema_inspect_cycles,
     execute_inspect_cycles},
    {"inspect_blast_radius",
     "Analyze the blast radius of a symbol: BFS on reverse import graph to find all files that could be affected by changes. Returns confirmed (name found in file) and potential (transitive dependency) impact.",
     schema_inspect_blast_radius,
     execute_inspect_blast_radius},
    {"find_dead",
     "Find unreferenced symbols: 'dead' (file has no importers) or 'unreferenced' (file imported but symbol not referenced by name). Entry points (main, init, test_*, etc.) excluded automatically.",
     schema_find_dead,
     execute_find_dead},
    {"index_file",
     "Reindex a single file without rebuilding the entire index. Compares content hash and skips if unchanged. Fast: <100ms for typical files.",
     schema_index_file,
     execute_index_file},
    {"help",
     "Get usage details for a TokToken tool, or list all tools. Returns parameters, types, defaults, and descriptions.",
     schema_help,
     execute_help},
};

const int TT_MCP_TOOLS_COUNT = sizeof(TT_MCP_TOOLS) / sizeof(TT_MCP_TOOLS[0]);
