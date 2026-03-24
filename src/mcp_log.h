/*
 * mcp_log.h -- MCP tool call and lifecycle JSONL logging.
 *
 * Logs tool invocations and lifecycle events to ~/.cache/toktoken/logs/mcp.jsonl.
 * Append-only, best-effort (failures are silently ignored).
 */

#ifndef TT_MCP_LOG_H
#define TT_MCP_LOG_H

#include <cJSON.h>
#include <stdbool.h>
#include <stdint.h>

/* Log event types */
typedef enum {
    TT_MCP_LOG_INITIALIZE,   /* client connected */
    TT_MCP_LOG_TOOLS_LIST,   /* client listed tools */
    TT_MCP_LOG_TOOL_CALL,    /* tool executed */
    TT_MCP_LOG_SHUTDOWN      /* server stopping */
} tt_mcp_log_event_e;

/*
 * tt_mcp_log_lifecycle -- Log a lifecycle event (initialize, tools/list, shutdown).
 *
 * project_root: project path (may be NULL for shutdown).
 * detail:       optional detail string (e.g., client name for initialize).
 */
void tt_mcp_log_lifecycle(tt_mcp_log_event_e event,
                          const char *project_root,
                          const char *detail);

/*
 * tt_mcp_log_tool_call -- Log a tool execution.
 *
 * tool_name:    MCP tool name (e.g. "search_symbols")
 * arguments:    cJSON object of tool arguments (borrowed, not freed)
 * project_root: project path
 * duration_ms:  execution time in milliseconds
 * success:      true if tool succeeded
 * error_reason: error message if !success (NULL if success)
 */
void tt_mcp_log_tool_call(const char *tool_name,
                          const cJSON *arguments,
                          const char *project_root,
                          uint64_t duration_ms,
                          bool success,
                          const char *error_reason);

#endif /* TT_MCP_LOG_H */
