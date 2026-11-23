/*
 * mcp_tools.h -- MCP tool registration and execution bridge.
 *
 * Defines all 32 TokToken tools exposed via MCP, mapping each to
 * the corresponding tt_cmd_*_exec() core function.
 */

#ifndef TT_MCP_TOOLS_H
#define TT_MCP_TOOLS_H

#include <cJSON.h>

/* Forward declaration */
struct tt_mcp_server_t;

/*
 * Definition of a single MCP tool.
 */
typedef struct
{
    const char *name;           /* Tool name (e.g. "search_symbols") */
    const char *description;    /* Human-readable description */
    cJSON *(*get_schema)(void); /* Generate inputSchema JSON Schema [caller-frees] */
    cJSON *(*execute)(struct tt_mcp_server_t *srv,
                      const cJSON *arguments); /* Execute the tool [caller-frees] */
} tt_mcp_tool_t;

/* Array of all registered tools and its count */
extern const tt_mcp_tool_t TT_MCP_TOOLS[];
extern const int TT_MCP_TOOLS_COUNT;

#endif /* TT_MCP_TOOLS_H */
