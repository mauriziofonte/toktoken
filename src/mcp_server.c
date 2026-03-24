/*
 * mcp_server.c -- MCP server core: message loop, JSON-RPC 2.0 dispatch.
 *
 * Ref: MCP spec https://modelcontextprotocol.io/specification/2025-11-25
 */

#include "mcp_server.h"
#include "mcp_tools.h"
#include "mcp_log.h"
#include "version.h"
#include "error.h"
#include "platform.h"
#include "storage_paths.h"
#include "git_head.h"
#include "json_output.h"
#include "update_check.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#ifdef TT_PLATFORM_WINDOWS
/*
 * getline polyfill for Windows (MinGW lacks POSIX getline).
 * Reads a line from stream into *lineptr, dynamically allocating/resizing.
 * Returns number of characters read, or -1 on EOF/error.
 */
static ssize_t tt_getline(char **lineptr, size_t *n, FILE *stream)
{
    if (!lineptr || !n || !stream)
        return -1;

    size_t pos = 0;
    int c;

    if (*lineptr == NULL || *n == 0)
    {
        *n = 128;
        *lineptr = (char *)malloc(*n);
        if (!*lineptr)
            return -1;
    }

    while ((c = fgetc(stream)) != EOF)
    {
        if (pos + 1 >= *n)
        {
            size_t new_n = *n * 2;
            char *new_ptr = (char *)realloc(*lineptr, new_n);
            if (!new_ptr)
                return -1;
            *lineptr = new_ptr;
            *n = new_n;
        }
        (*lineptr)[pos++] = (char)c;
        if (c == '\n')
            break;
    }

    if (pos == 0)
        return -1; /* EOF with no data */

    (*lineptr)[pos] = '\0';
    return (ssize_t)pos;
}
#define getline tt_getline
#endif

/* Reference to the global interrupt flag (defined in cmd_index.c) */
extern volatile sig_atomic_t tt_interrupted;

/* ---- JSON-RPC helpers ---- */

cJSON *mcp_make_result(const cJSON *id, cJSON *result)
{
    cJSON *resp = cJSON_CreateObject();
    if (!resp)
    {
        cJSON_Delete(result);
        return NULL;
    }
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id)
        cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, true));
    else
        cJSON_AddNullToObject(resp, "id");
    cJSON_AddItemToObject(resp, "result", result);
    return resp;
}

cJSON *mcp_make_error(const cJSON *id, int code, const char *message)
{
    cJSON *resp = cJSON_CreateObject();
    if (!resp)
        return NULL;
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id)
        cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, true));
    else
        cJSON_AddNullToObject(resp, "id");
    cJSON *err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    cJSON_AddItemToObject(resp, "error", err);
    return resp;
}

cJSON *mcp_tool_error(const char *message)
{
    cJSON *result = cJSON_CreateObject();
    if (result)
        cJSON_AddStringToObject(result, "error", message);
    return result;
}

/* ---- JSON extraction helpers ---- */

int mcp_get_int_or_default(const cJSON *obj, const char *key, int def)
{
    if (!obj)
        return def;
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item && cJSON_IsNumber(item))
        return item->valueint;
    return def;
}

bool mcp_get_bool(const cJSON *obj, const char *key)
{
    if (!obj)
        return false;
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return item && cJSON_IsTrue(item);
}

/* ---- Lazy database ---- */

int mcp_ensure_db(tt_mcp_server_t *srv)
{
    if (srv->db)
        return 0;

    char *db_path = tt_storage_db_path(srv->project_root);
    if (!db_path)
    {
        tt_error_set("Cannot determine storage path for %s", srv->project_root);
        return -1;
    }

    if (!tt_file_exists(db_path))
    {
        free(db_path);
        tt_error_set("No index found. Run index_create first.");
        return -1;
    }

    srv->db = calloc(1, sizeof(tt_database_t));
    if (!srv->db)
    {
        free(db_path);
        tt_error_set("Out of memory");
        return -1;
    }

    if (tt_database_open(srv->db, srv->project_root) < 0)
    {
        free(srv->db);
        srv->db = NULL;
        free(db_path);
        return -1;
    }

    free(db_path);
    return 0;
}

/* ---- Initialize handler ---- */

static cJSON *mcp_handle_initialize(tt_mcp_server_t *srv,
                                    const cJSON *id,
                                    const cJSON *params)
{
    if (srv->initialized)
    {
        return mcp_make_error(id, TT_JSONRPC_INVALID_REQUEST,
                              "Already initialized");
    }

    /* Log client info (optional, for diagnostics) */
    if (params)
    {
        const char *client_version = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(params, "protocolVersion"));
        if (client_version)
            fprintf(stderr, "[mcp] Client protocol version: %s\n", client_version);

        const cJSON *client_info = cJSON_GetObjectItemCaseSensitive(params, "clientInfo");
        if (client_info)
        {
            const char *name = cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(client_info, "name"));
            const char *version = cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(client_info, "version"));
            fprintf(stderr, "[mcp] Client: %s %s\n",
                    name ? name : "unknown", version ? version : "");
        }
    }

    srv->initialized = true;

    /* Log initialize event */
    {
        const char *cname = NULL;
        const char *cver = NULL;
        const cJSON *ci = cJSON_GetObjectItemCaseSensitive(params, "clientInfo");
        if (ci)
        {
            cname = cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(ci, "name"));
            cver = cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(ci, "version"));
        }
        char detail[256];
        snprintf(detail, sizeof(detail), "%s %s",
                 cname ? cname : "unknown", cver ? cver : "");
        tt_mcp_log_lifecycle(TT_MCP_LOG_INITIALIZE, srv->project_root, detail);
    }

    /* Build result */
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "protocolVersion", TT_MCP_PROTOCOL_VERSION);

    cJSON *capabilities = cJSON_CreateObject();
    cJSON *tools_cap = cJSON_CreateObject();
    cJSON_AddBoolToObject(tools_cap, "listChanged", true);
    cJSON_AddItemToObject(capabilities, "tools", tools_cap);
    cJSON_AddItemToObject(result, "capabilities", capabilities);

    cJSON *server_info = cJSON_CreateObject();
    cJSON_AddStringToObject(server_info, "name", "toktoken");
    cJSON_AddStringToObject(server_info, "version", TT_VERSION);
    cJSON_AddItemToObject(result, "serverInfo", server_info);

    return mcp_make_result(id, result);
}

/* ---- tools/list handler ---- */

static cJSON *mcp_handle_tools_list(tt_mcp_server_t *srv,
                                    const cJSON *id,
                                    const cJSON *params)
{
    (void)params;

    cJSON *result = cJSON_CreateObject();
    cJSON *tools = cJSON_CreateArray();

    for (int i = 0; i < TT_MCP_TOOLS_COUNT; i++)
    {
        cJSON *tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", TT_MCP_TOOLS[i].name);
        cJSON_AddStringToObject(tool, "description", TT_MCP_TOOLS[i].description);
        cJSON *schema = TT_MCP_TOOLS[i].get_schema();
        cJSON_AddItemToObject(tool, "inputSchema", schema);
        cJSON_AddItemToArray(tools, tool);
    }

    cJSON_AddItemToObject(result, "tools", tools);

    tt_mcp_log_lifecycle(TT_MCP_LOG_TOOLS_LIST, srv->project_root, NULL);

    return mcp_make_result(id, result);
}

/* ---- Staleness detection ---- */

#define STALE_CACHE_MS 5000  /* re-check every 5 seconds */
#define STALE_SAMPLE_SIZE 10 /* stat this many random files */

/*
 * mcp_inject_staleness -- Check if the index is stale and inject into _ttk.
 *
 * Two-tier detection:
 *   Tier 1: git HEAD changed since last index (fast, exact for git repos).
 *   Tier 2: sample random indexed files and compare mtime against
 *           the indexed_at timestamp (catches uncommitted edits, non-git).
 *
 * Results are cached for STALE_CACHE_MS to avoid repeated I/O.
 */
static void mcp_inject_staleness(tt_mcp_server_t *srv, cJSON *meta)
{
    if (!srv->project_root)
        return;

    /* Cache check */
    uint64_t now = tt_monotonic_ms();
    if (srv->stale_check_ms > 0 && (now - srv->stale_check_ms) < STALE_CACHE_MS)
    {
        if (srv->stale_cached)
        {
            cJSON_AddBoolToObject(meta, "stale", true);
            cJSON_AddStringToObject(meta, "stale_reason", srv->stale_reason);
        }
        return;
    }
    srv->stale_check_ms = now;
    srv->stale_cached = false;
    srv->stale_reason[0] = '\0';

    /* Ensure we have a DB handle (tools use their own connections) */
    if (!srv->db && mcp_ensure_db(srv) < 0)
        return;

    /* Get index timestamp from metadata (DB file mtime is unreliable —
     * sqlite3_open modifies it). Parse indexed_at RFC3339 string. */
    time_t indexed_at = 0;
    {
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(srv->db->db,
                               "SELECT value FROM metadata WHERE key = 'indexed_at'",
                               -1, &stmt, NULL) == SQLITE_OK)
        {
            if (sqlite3_step(stmt) == SQLITE_ROW)
            {
                const char *ts = (const char *)sqlite3_column_text(stmt, 0);
                if (ts && strlen(ts) >= 19)
                {
                    /* Parse "YYYY-MM-DDTHH:MM:SS..." → time_t */
                    struct tm tm = {0};
                    if (sscanf(ts, "%d-%d-%dT%d:%d:%d",
                               &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6)
                    {
                        tm.tm_year -= 1900;
                        tm.tm_mon -= 1;
                        tm.tm_isdst = -1;
                        indexed_at = mktime(&tm);
                    }
                }
            }
            sqlite3_finalize(stmt);
        }
    }
    if (indexed_at <= 0)
        return;

    /* Tier 1: git HEAD comparison */
    {
        char *current_head = tt_git_head(srv->project_root);
        if (current_head && current_head[0])
        {
            sqlite3_stmt *stmt = NULL;
            if (sqlite3_prepare_v2(srv->db->db,
                                   "SELECT value FROM metadata WHERE key = 'git_head'",
                                   -1, &stmt, NULL) == SQLITE_OK)
            {
                if (sqlite3_step(stmt) == SQLITE_ROW)
                {
                    const char *stored = (const char *)sqlite3_column_text(stmt, 0);
                    if (stored && stored[0] && strcmp(stored, current_head) != 0)
                    {
                        srv->stale_cached = true;
                        snprintf(srv->stale_reason, sizeof(srv->stale_reason),
                                 "git HEAD changed: %.8s -> %.8s. "
                                 "Call index_update to refresh.",
                                 stored, current_head);
                    }
                }
                sqlite3_finalize(stmt);
            }
        }
        free(current_head);
    }

    /* Tier 2: sample file mtime check (catches uncommitted edits, non-git) */
    if (!srv->stale_cached)
    {
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(srv->db->db,
                               "SELECT path FROM files ORDER BY RANDOM() LIMIT ?",
                               -1, &stmt, NULL) == SQLITE_OK)
        {
            sqlite3_bind_int(stmt, 1, STALE_SAMPLE_SIZE);

            int checked = 0, changed = 0;
            while (sqlite3_step(stmt) == SQLITE_ROW)
            {
                const char *relpath = (const char *)sqlite3_column_text(stmt, 0);
                if (!relpath)
                    continue;

                char *full = tt_path_join(srv->project_root, relpath);
                if (full)
                {
                    time_t fmtime = tt_file_mtime(full);
                    if (fmtime > indexed_at)
                        changed++;
                    free(full);
                }
                checked++;
            }
            sqlite3_finalize(stmt);

            if (changed > 0)
            {
                srv->stale_cached = true;
                snprintf(srv->stale_reason, sizeof(srv->stale_reason),
                         "%d/%d sampled files modified since last index. "
                         "Call index_update to refresh.",
                         changed, checked);
            }
        }
    }

    if (srv->stale_cached)
    {
        cJSON_AddBoolToObject(meta, "stale", true);
        cJSON_AddStringToObject(meta, "stale_reason", srv->stale_reason);
    }
}

/* ---- next-step suggestions for LLM workflow guidance ---- */

/*
 * Extract the first result ID from common response shapes.
 * Looks for results[0].id, then symbols[0] patterns, then file field.
 * Returns NULL if nothing found (caller must not free).
 */
static const char *first_result_id(const cJSON *result)
{
    /* results[0].id — search:symbols, find:dead, find:callers */
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(result, "results");
    if (arr && cJSON_IsArray(arr) && cJSON_GetArraySize(arr) > 0)
    {
        const cJSON *first = cJSON_GetArrayItem(arr, 0);
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(first, "id");
        if (id && cJSON_IsString(id))
            return id->valuestring;
    }
    /* symbols array — inspect:outline */
    arr = cJSON_GetObjectItemCaseSensitive(result, "symbols");
    if (arr && cJSON_IsArray(arr) && cJSON_GetArraySize(arr) > 0)
    {
        const cJSON *first = cJSON_GetArrayItem(arr, 0);
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(first, "id");
        if (id && cJSON_IsString(id))
            return id->valuestring;
    }
    return NULL;
}

static const char *first_result_file(const cJSON *result)
{
    /* results[0].f or results[0].file — search:text */
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(result, "results");
    if (arr && cJSON_IsArray(arr) && cJSON_GetArraySize(arr) > 0)
    {
        const cJSON *first = cJSON_GetArrayItem(arr, 0);
        const cJSON *f = cJSON_GetObjectItemCaseSensitive(first, "f");
        if (!f)
            f = cJSON_GetObjectItemCaseSensitive(first, "file");
        if (f && cJSON_IsString(f))
            return f->valuestring;
    }
    /* top-level file field — inspect:outline, inspect:bundle */
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(result, "file");
    if (f && cJSON_IsString(f))
        return f->valuestring;
    return NULL;
}

static int result_count(const cJSON *result)
{
    const cJSON *n = cJSON_GetObjectItemCaseSensitive(result, "n");
    if (n && cJSON_IsNumber(n))
        return (int)n->valuedouble;
    const cJSON *count = cJSON_GetObjectItemCaseSensitive(result, "count");
    if (count && cJSON_IsNumber(count))
        return (int)count->valuedouble;
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(result, "results");
    if (arr && cJSON_IsArray(arr))
        return cJSON_GetArraySize(arr);
    return -1;
}

/*
 * Inject contextual next-step suggestions into the _ttk metadata.
 * Suggestions use MCP tool names and include concrete IDs from results
 * so the LLM can act immediately without guessing.
 */
static void mcp_inject_next_steps(const char *tool_name,
                                  const cJSON *tool_result,
                                  cJSON *ttk)
{
    /* Check for error responses — universal suggestion */
    const cJSON *err = cJSON_GetObjectItemCaseSensitive(tool_result, "error");
    if (err && cJSON_IsString(err))
    {
        if (strcmp(err->valuestring, "no_index") == 0)
        {
            cJSON *next = cJSON_AddArrayToObject(ttk, "next");
            cJSON_AddItemToArray(next, cJSON_CreateString(
                "index_create — no index exists yet"));
            return;
        }
    }

    /* Prepend stale warning if index is stale */
    const cJSON *stale = cJSON_GetObjectItemCaseSensitive(ttk, "stale");
    bool is_stale = (stale && cJSON_IsTrue(stale));

    cJSON *next = cJSON_CreateArray();
    char buf[512];

    if (is_stale)
    {
        cJSON_AddItemToArray(next, cJSON_CreateString(
            "index_update — index is stale"));
    }

    const char *fid = first_result_id(tool_result);
    const char *ffile = first_result_file(tool_result);
    int cnt = result_count(tool_result);

    /* Tool-specific suggestions */
    if (strcmp(tool_name, "codebase_detect") == 0)
    {
        const cJSON *action = cJSON_GetObjectItemCaseSensitive(
            tool_result, "action");
        if (action && cJSON_IsString(action))
        {
            if (strcmp(action->valuestring, "index:create") == 0)
                cJSON_AddItemToArray(next, cJSON_CreateString(
                    "index_create to build the index"));
            else
                cJSON_AddItemToArray(next, cJSON_CreateString(
                    "search_symbols or inspect_tree to explore"));
        }
    }
    else if (strcmp(tool_name, "index_create") == 0 ||
             strcmp(tool_name, "index_file") == 0)
    {
        cJSON_AddItemToArray(next, cJSON_CreateString(
            "search_symbols to find code"));
        cJSON_AddItemToArray(next, cJSON_CreateString(
            "stats for index info"));
    }
    else if (strcmp(tool_name, "index_update") == 0)
    {
        const cJSON *changed = cJSON_GetObjectItemCaseSensitive(
            tool_result, "changed");
        if (changed && cJSON_IsNumber(changed) && changed->valuedouble > 0)
            cJSON_AddItemToArray(next, cJSON_CreateString(
                "search_symbols to find updated code"));
    }
    else if (strcmp(tool_name, "search_symbols") == 0)
    {
        if (cnt > 0 && fid)
        {
            snprintf(buf, sizeof(buf), "inspect_symbol %s", fid);
            cJSON_AddItemToArray(next, cJSON_CreateString(buf));
            snprintf(buf, sizeof(buf), "inspect_bundle %s", fid);
            cJSON_AddItemToArray(next, cJSON_CreateString(buf));
        }
        else if (cnt == 0)
        {
            cJSON_AddItemToArray(next, cJSON_CreateString(
                "search_text for full-text fallback"));
        }
    }
    else if (strcmp(tool_name, "search_text") == 0)
    {
        if (cnt > 0 && ffile)
        {
            snprintf(buf, sizeof(buf), "inspect_outline %s", ffile);
            cJSON_AddItemToArray(next, cJSON_CreateString(buf));
        }
    }
    else if (strcmp(tool_name, "inspect_outline") == 0)
    {
        if (fid)
        {
            snprintf(buf, sizeof(buf), "inspect_symbol %s", fid);
            cJSON_AddItemToArray(next, cJSON_CreateString(buf));
            snprintf(buf, sizeof(buf), "inspect_bundle %s", fid);
            cJSON_AddItemToArray(next, cJSON_CreateString(buf));
        }
    }
    else if (strcmp(tool_name, "inspect_symbol") == 0)
    {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(tool_result, "id");
        if (id && cJSON_IsString(id))
        {
            snprintf(buf, sizeof(buf), "inspect_bundle %s", id->valuestring);
            cJSON_AddItemToArray(next, cJSON_CreateString(buf));
            snprintf(buf, sizeof(buf), "find_callers %s", id->valuestring);
            cJSON_AddItemToArray(next, cJSON_CreateString(buf));
        }
    }
    else if (strcmp(tool_name, "inspect_bundle") == 0)
    {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(tool_result, "id");
        if (id && cJSON_IsString(id))
        {
            snprintf(buf, sizeof(buf), "find_callers %s", id->valuestring);
            cJSON_AddItemToArray(next, cJSON_CreateString(buf));
        }
        if (ffile)
        {
            snprintf(buf, sizeof(buf), "find_importers %s", ffile);
            cJSON_AddItemToArray(next, cJSON_CreateString(buf));
        }
    }
    else if (strcmp(tool_name, "find_importers") == 0)
    {
        if (cnt > 0 && ffile)
        {
            snprintf(buf, sizeof(buf), "inspect_dependencies %s", ffile);
            cJSON_AddItemToArray(next, cJSON_CreateString(buf));
        }
    }
    else if (strcmp(tool_name, "find_callers") == 0)
    {
        if (cnt > 0 && fid)
        {
            snprintf(buf, sizeof(buf), "inspect_symbol %s", fid);
            cJSON_AddItemToArray(next, cJSON_CreateString(buf));
        }
    }
    else if (strcmp(tool_name, "inspect_dependencies") == 0)
    {
        cJSON_AddItemToArray(next, cJSON_CreateString(
            "inspect_cycles to check for circular imports"));
    }
    else if (strcmp(tool_name, "find_dead") == 0)
    {
        if (cnt > 0 && fid)
        {
            snprintf(buf, sizeof(buf), "inspect_symbol %s", fid);
            cJSON_AddItemToArray(next, cJSON_CreateString(buf));
        }
    }
    else if (strcmp(tool_name, "inspect_blast_radius") == 0)
    {
        const cJSON *summary = cJSON_GetObjectItemCaseSensitive(
            tool_result, "summary");
        if (summary)
        {
            const cJSON *cc = cJSON_GetObjectItemCaseSensitive(
                summary, "confirmed_count");
            if (cc && cJSON_IsNumber(cc) && cc->valuedouble > 0)
            {
                const cJSON *confirmed = cJSON_GetObjectItemCaseSensitive(
                    tool_result, "confirmed");
                if (confirmed && cJSON_IsArray(confirmed) &&
                    cJSON_GetArraySize(confirmed) > 0)
                {
                    const cJSON *first = cJSON_GetArrayItem(confirmed, 0);
                    const cJSON *cf = cJSON_GetObjectItemCaseSensitive(
                        first, "file");
                    if (cf && cJSON_IsString(cf))
                    {
                        snprintf(buf, sizeof(buf), "inspect_file %s",
                                 cf->valuestring);
                        cJSON_AddItemToArray(next, cJSON_CreateString(buf));
                    }
                }
            }
        }
    }

    /* Only add if we have suggestions */
    if (cJSON_GetArraySize(next) > 0)
        cJSON_AddItemToObject(ttk, "next", next);
    else
        cJSON_Delete(next);
}

/* ---- tools/call handler ---- */

static cJSON *mcp_handle_tools_call(tt_mcp_server_t *srv,
                                    const cJSON *id,
                                    const cJSON *params)
{
    const char *name = NULL;
    const cJSON *arguments = NULL;

    if (params)
    {
        name = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(params, "name"));
        arguments = cJSON_GetObjectItemCaseSensitive(params, "arguments");
    }

    if (!name)
    {
        return mcp_make_error(id, TT_JSONRPC_INVALID_PARAMS,
                              "Missing tool name");
    }

    /* Find the tool */
    const tt_mcp_tool_t *tool = NULL;
    for (int i = 0; i < TT_MCP_TOOLS_COUNT; i++)
    {
        if (strcmp(TT_MCP_TOOLS[i].name, name) == 0)
        {
            tool = &TT_MCP_TOOLS[i];
            break;
        }
    }

    if (!tool)
    {
        return mcp_make_error(id, TT_JSONRPC_INVALID_PARAMS, "Unknown tool");
    }

    /* Extract progressToken from _meta if present */
    const char *progress_token = NULL;
    if (params)
    {
        const cJSON *meta = cJSON_GetObjectItemCaseSensitive(params, "_meta");
        if (meta)
        {
            const cJSON *pt = cJSON_GetObjectItemCaseSensitive(meta, "progressToken");
            if (pt)
                progress_token = cJSON_GetStringValue(pt);
        }
    }
    srv->progress_token = progress_token;

    /* Measure execution time */
    uint64_t start_ms = tt_monotonic_ms();

    /* Execute the tool */
    cJSON *tool_result = tool->execute(srv, arguments);

    srv->progress_token = NULL;
    uint64_t elapsed_ms = tt_monotonic_ms() - start_ms;

    /* Reset staleness cache after write operations */
    if (strcmp(name, "index_create") == 0 ||
        strcmp(name, "index_update") == 0 ||
        strcmp(name, "cache_clear") == 0)
    {
        srv->stale_check_ms = 0;
        srv->stale_cached = false;
    }

    /* Handle NULL result (internal error) */
    if (!tool_result)
    {
        tool_result = mcp_tool_error(tt_error_get());
    }

    /* Inject _ttk envelope into tool_result */
    cJSON *tt = cJSON_CreateObject();
    cJSON_AddStringToObject(tt, "timestamp", tt_now_rfc3339());
    cJSON_AddNumberToObject(tt, "duration_ms", (double)elapsed_ms);
    cJSON_AddStringToObject(tt, "version", TT_VERSION);

    /* Inject staleness signal (cached, ~0ms when warm) */
    mcp_inject_staleness(srv, tt);

    /* Inject update-available signal (cached, ~0ms when warm) */
    {
        tt_update_info_t uinfo = tt_update_check();
        if (uinfo.update_available) {
            cJSON_AddStringToObject(tt, "update_available", uinfo.upstream_version);
        }
        tt_update_info_free(&uinfo);
    }

    /* Inject next-step suggestions for LLM workflow guidance */
    mcp_inject_next_steps(name, tool_result, tt);

    cJSON_AddItemToObject(tool_result, "_ttk", tt);

    /* Serialize tool result to text */
    char *text = cJSON_PrintUnformatted(tool_result);

    /* Determine if this is a tool-level error */
    bool is_error = false;
    cJSON *err_field = cJSON_GetObjectItemCaseSensitive(tool_result, "error");
    cJSON *success_field = cJSON_GetObjectItemCaseSensitive(tool_result, "success");
    if (err_field || (success_field && !cJSON_IsTrue(success_field)))
    {
        is_error = true;
    }

    /* Log tool call */
    {
        const char *error_reason = NULL;
        if (is_error && err_field)
            error_reason = cJSON_GetStringValue(err_field);
        tt_mcp_log_tool_call(name, arguments, srv->project_root,
                             elapsed_ms, !is_error, error_reason);
    }

    /* Build MCP content envelope */
    cJSON *result = cJSON_CreateObject();

    cJSON *content = cJSON_CreateArray();
    cJSON *content_item = cJSON_CreateObject();
    cJSON_AddStringToObject(content_item, "type", "text");
    cJSON_AddStringToObject(content_item, "text", text ? text : "{}");
    cJSON_AddItemToArray(content, content_item);
    cJSON_AddItemToObject(result, "content", content);

    cJSON_AddBoolToObject(result, "isError", is_error);

    free(text);
    cJSON_Delete(tool_result);

    return mcp_make_result(id, result);
}

/* ---- Main dispatch ---- */

static cJSON *mcp_dispatch(tt_mcp_server_t *srv, const cJSON *request)
{
    /* Validate request is an object */
    if (!cJSON_IsObject(request))
    {
        return mcp_make_error(NULL, TT_JSONRPC_INVALID_REQUEST,
                              "Invalid Request: not an object");
    }

    const cJSON *method_item = cJSON_GetObjectItemCaseSensitive(request, "method");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(request, "id");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(request, "params");

    /* method must be a string if present */
    const char *method = NULL;
    if (method_item)
    {
        if (!cJSON_IsString(method_item))
        {
            return mcp_make_error(id, TT_JSONRPC_INVALID_REQUEST,
                                  "Invalid Request: method must be a string");
        }
        method = method_item->valuestring;
    }

    /* Notification: has method but NO id */
    bool is_notification = (method != NULL && id == NULL);

    if (!method)
    {
        return mcp_make_error(id, TT_JSONRPC_INVALID_REQUEST,
                              "Invalid Request: missing method");
    }

    /* Route by method name */
    if (strcmp(method, "initialize") == 0)
    {
        return mcp_handle_initialize(srv, id, params);
    }
    if (strcmp(method, "notifications/initialized") == 0)
    {
        /* Confirmation notification -- no response */
        return NULL;
    }

    /* All methods below require initialization */
    if (!srv->initialized)
    {
        if (is_notification)
            return NULL;
        return mcp_make_error(id, TT_MCP_NOT_INITIALIZED,
                              "Server not initialized");
    }

    if (strcmp(method, "tools/list") == 0)
    {
        return mcp_handle_tools_list(srv, id, params);
    }
    if (strcmp(method, "tools/call") == 0)
    {
        return mcp_handle_tools_call(srv, id, params);
    }
    if (strcmp(method, "ping") == 0)
    {
        return mcp_make_result(id, cJSON_CreateObject());
    }

    /* Unknown notification -- ignore silently */
    if (is_notification)
    {
        return NULL;
    }

    /* Unknown method -- error */
    return mcp_make_error(id, TT_JSONRPC_METHOD_NOT_FOUND, "Method not found");
}

/* ---- Server lifecycle ---- */

int tt_mcp_server_init(tt_mcp_server_t *srv, const char *project_root)
{
    memset(srv, 0, sizeof(*srv));
    srv->running = true;

    if (project_root)
    {
        srv->project_root = strdup(project_root);
        if (!srv->project_root)
        {
            tt_error_set("Out of memory");
            return -1;
        }
    }

    return 0;
}

/*
 * Send a JSON-RPC response to stdout.
 * Returns 0 on success, -1 if writing fails (broken pipe).
 */
static int mcp_send_response(cJSON *response)
{
    char *json_str = cJSON_PrintUnformatted(response);
    if (!json_str)
        return -1;

    int ret = 0;
    if (fprintf(stdout, "%s\n", json_str) < 0)
    {
        ret = -1;
    }
    if (fflush(stdout) != 0)
    {
        ret = -1;
    }

    free(json_str);
    return ret;
}

void mcp_send_progress(tt_mcp_server_t *srv, int64_t progress, int64_t total,
                        const char *message)
{
    if (!srv || !srv->progress_token)
        return;

    cJSON *notif = cJSON_CreateObject();
    if (!notif) return;

    cJSON_AddStringToObject(notif, "jsonrpc", "2.0");
    cJSON_AddStringToObject(notif, "method", "notifications/progress");

    cJSON *params = cJSON_CreateObject();
    if (params)
    {
        cJSON_AddStringToObject(params, "progressToken", srv->progress_token);
        cJSON_AddNumberToObject(params, "progress", (double)progress);
        cJSON_AddNumberToObject(params, "total", (double)total);
        if (message && message[0])
            cJSON_AddStringToObject(params, "message", message);
        cJSON_AddItemToObject(notif, "params", params);
    }

    char *json_str = cJSON_PrintUnformatted(notif);
    if (json_str)
    {
        fprintf(stdout, "%s\n", json_str);
        fflush(stdout);
        free(json_str);
    }
    cJSON_Delete(notif);
}

int tt_mcp_server_run(tt_mcp_server_t *srv)
{
    /* Ignore SIGPIPE so we get write errors instead of crashes */
#ifndef _WIN32
    {
        struct sigaction sa_pipe;
        memset(&sa_pipe, 0, sizeof(sa_pipe));
        sa_pipe.sa_handler = SIG_IGN;
        sigemptyset(&sa_pipe.sa_mask);
        sigaction(SIGPIPE, &sa_pipe, NULL);
    }
#endif

    char *line = NULL;
    size_t line_cap = 0;

    while (srv->running && !tt_interrupted)
    {
        /* getline handles dynamic allocation automatically.
         * With SA_RESTART disabled, SIGINT/SIGTERM cause getline to return -1
         * with errno=EINTR. The loop condition checks tt_interrupted. */
        ssize_t nread = getline(&line, &line_cap, stdin);
        if (nread < 0)
        {
            /* EOF, EINTR (signal), or I/O error — all mean we should stop */
            break;
        }

        /* Remove trailing newline */
        if (nread > 0 && line[nread - 1] == '\n')
        {
            line[nread - 1] = '\0';
            nread--;
        }

        /* Skip empty lines */
        if (nread == 0 || line[0] == '\0')
            continue;

        /* Parse JSON */
        cJSON *request = cJSON_Parse(line);
        if (!request)
        {
            cJSON *err_resp = mcp_make_error(NULL, TT_JSONRPC_PARSE_ERROR,
                                             "Parse error");
            if (mcp_send_response(err_resp) < 0)
            {
                cJSON_Delete(err_resp);
                break;
            }
            cJSON_Delete(err_resp);
            continue;
        }

        /* Dispatch */
        cJSON *response = mcp_dispatch(srv, request);
        cJSON_Delete(request);

        if (response)
        {
            int send_ok = mcp_send_response(response);
            cJSON_Delete(response);
            if (send_ok < 0)
                break;
        }
    }

    tt_mcp_log_lifecycle(TT_MCP_LOG_SHUTDOWN, srv->project_root, NULL);

    free(line);
    return 0;
}

void tt_mcp_server_free(tt_mcp_server_t *srv)
{
    if (srv->db)
    {
        tt_database_close(srv->db);
        free(srv->db);
        srv->db = NULL;
    }
    free(srv->project_root);
    srv->project_root = NULL;
}
