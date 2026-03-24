/*
 * mcp_log.c -- MCP tool call and lifecycle JSONL logging.
 *
 * Appends one JSON line per event to ~/.cache/toktoken/logs/mcp.jsonl.
 * Best-effort: failures are silently ignored (never blocks MCP operation).
 */

#include "mcp_log.h"
#include "storage_paths.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TT_MCP_LOG_FILE "mcp.jsonl"

/* ---- Internal ---- */

static const char *event_name(tt_mcp_log_event_e event)
{
    switch (event)
    {
    case TT_MCP_LOG_INITIALIZE: return "initialize";
    case TT_MCP_LOG_TOOLS_LIST: return "tools_list";
    case TT_MCP_LOG_TOOL_CALL:  return "tool_call";
    case TT_MCP_LOG_SHUTDOWN:   return "shutdown";
    }
    return "unknown";
}

/*
 * Write a cJSON object as a single JSONL line to mcp.jsonl.
 * Takes ownership of entry and frees it.
 */
static void log_write_entry(cJSON *entry)
{
    if (!entry)
        return;

    char *logs_dir = tt_storage_logs_dir();
    if (!logs_dir)
    {
        cJSON_Delete(entry);
        return;
    }

    /* Ensure logs directory exists (idempotent) */
    tt_mkdir_p(logs_dir);

    char *path = tt_path_join(logs_dir, TT_MCP_LOG_FILE);
    free(logs_dir);
    if (!path)
    {
        cJSON_Delete(entry);
        return;
    }

    char *json = cJSON_PrintUnformatted(entry);
    cJSON_Delete(entry);
    if (!json)
    {
        free(path);
        return;
    }

    FILE *fp = fopen(path, "a");
    free(path);
    if (!fp)
    {
        free(json);
        return;
    }

    fprintf(fp, "%s\n", json);
    fclose(fp);
    free(json);
}

/* ---- Public API ---- */

void tt_mcp_log_lifecycle(tt_mcp_log_event_e event,
                          const char *project_root,
                          const char *detail)
{
    cJSON *entry = cJSON_CreateObject();
    if (!entry)
        return;

    cJSON_AddStringToObject(entry, "ts", tt_now_rfc3339());
    cJSON_AddStringToObject(entry, "event", event_name(event));

    if (project_root)
        cJSON_AddStringToObject(entry, "project", project_root);

    if (detail)
        cJSON_AddStringToObject(entry, "detail", detail);

    log_write_entry(entry);
}

void tt_mcp_log_tool_call(const char *tool_name,
                          const cJSON *arguments,
                          const char *project_root,
                          uint64_t duration_ms,
                          bool success,
                          const char *error_reason)
{
    cJSON *entry = cJSON_CreateObject();
    if (!entry)
        return;

    cJSON_AddStringToObject(entry, "ts", tt_now_rfc3339());
    cJSON_AddStringToObject(entry, "event", "tool_call");

    if (tool_name)
        cJSON_AddStringToObject(entry, "tool", tool_name);

    if (arguments)
        cJSON_AddItemToObject(entry, "args", cJSON_Duplicate(arguments, 1));

    if (project_root)
        cJSON_AddStringToObject(entry, "project", project_root);

    cJSON_AddNumberToObject(entry, "duration_ms", (double)duration_ms);
    cJSON_AddBoolToObject(entry, "success", success);

    if (!success && error_reason)
        cJSON_AddStringToObject(entry, "error", error_reason);

    log_write_entry(entry);
}
