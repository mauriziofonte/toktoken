/*
 * cmd_help.c -- Help introspection command.
 *
 * Lists all tools or returns detailed usage for a specific tool.
 * Derives parameter info from TT_MCP_TOOLS[] schemas at runtime.
 */

#include "cmd_help.h"
#include "mcp_tools.h"
#include "json_output.h"
#include "version.h"

#include <stdio.h>
#include <string.h>

/* ---- Name normalization ---- */

/* Normalize CLI name (colon-separated) to MCP name (underscore-separated).
 * Writes into buf (must be >= len+1). Returns buf. */
static char *normalize_to_mcp(const char *name, char *buf, size_t len)
{
    size_t i;
    for (i = 0; i < len && name[i]; i++)
        buf[i] = (name[i] == ':') ? '_' : name[i];
    buf[i] = '\0';
    return buf;
}

/* Convert MCP name to CLI name. Writes into buf. Returns buf. */
static char *mcp_to_cli(const char *mcp_name, char *buf, size_t len)
{
    size_t i;
    for (i = 0; i < len && mcp_name[i]; i++)
        buf[i] = (mcp_name[i] == '_') ? ':' : mcp_name[i];
    buf[i] = '\0';
    return buf;
}

/* Find a tool by name (accepts both CLI and MCP naming). */
static const tt_mcp_tool_t *find_tool(const char *name)
{
    char normalized[128];
    normalize_to_mcp(name, normalized, sizeof(normalized) - 1);

    for (int i = 0; i < TT_MCP_TOOLS_COUNT; i++)
    {
        if (strcmp(TT_MCP_TOOLS[i].name, normalized) == 0)
            return &TT_MCP_TOOLS[i];
    }
    return NULL;
}

/* ---- List all tools ---- */

static cJSON *help_list_all(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *tools = cJSON_AddArrayToObject(root, "tools");
    char cli_name[128];

    for (int i = 0; i < TT_MCP_TOOLS_COUNT; i++)
    {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "name", TT_MCP_TOOLS[i].name);
        mcp_to_cli(TT_MCP_TOOLS[i].name, cli_name, sizeof(cli_name) - 1);
        cJSON_AddStringToObject(entry, "cli", cli_name);
        cJSON_AddStringToObject(entry, "summary", TT_MCP_TOOLS[i].description);
        cJSON_AddItemToArray(tools, entry);
    }

    cJSON_AddNumberToObject(root, "count", TT_MCP_TOOLS_COUNT);
    cJSON_AddStringToObject(root, "tip",
                            "Call help with a tool name for detailed usage: help({command: 'search_symbols'})");
    return root;
}

/* ---- Detailed help for one tool ---- */

static cJSON *help_for_tool(const tt_mcp_tool_t *tool)
{
    cJSON *root = cJSON_CreateObject();
    char cli_name[128];

    cJSON_AddStringToObject(root, "tool", tool->name);
    mcp_to_cli(tool->name, cli_name, sizeof(cli_name) - 1);
    cJSON_AddStringToObject(root, "cli", cli_name);
    cJSON_AddStringToObject(root, "description", tool->description);

    /* Extract params from the tool's JSON Schema */
    cJSON *schema = tool->get_schema();
    if (schema)
    {
        cJSON *props = cJSON_GetObjectItem(schema, "properties");
        cJSON *required = cJSON_GetObjectItem(schema, "required");

        if (props && cJSON_GetArraySize(props) > 0)
        {
            cJSON *params = cJSON_AddArrayToObject(root, "params");
            cJSON *prop = NULL;
            cJSON_ArrayForEach(prop, props)
            {
                cJSON *param = cJSON_CreateObject();
                cJSON_AddStringToObject(param, "name", prop->string);

                cJSON *type = cJSON_GetObjectItem(prop, "type");
                if (type && cJSON_IsString(type))
                    cJSON_AddStringToObject(param, "type", type->valuestring);

                cJSON *desc = cJSON_GetObjectItem(prop, "description");
                if (desc && cJSON_IsString(desc))
                    cJSON_AddStringToObject(param, "description", desc->valuestring);

                /* Check if required */
                bool is_required = false;
                if (required && cJSON_IsArray(required))
                {
                    cJSON *req = NULL;
                    cJSON_ArrayForEach(req, required)
                    {
                        if (cJSON_IsString(req) &&
                            strcmp(req->valuestring, prop->string) == 0)
                        {
                            is_required = true;
                            break;
                        }
                    }
                }
                if (is_required)
                    cJSON_AddTrueToObject(param, "required");

                cJSON_AddItemToArray(params, param);
            }
        }

        cJSON_Delete(schema);
    }

    return root;
}

/* ---- Public API ---- */

cJSON *tt_cmd_help_exec(const char *command)
{
    if (!command || command[0] == '\0')
        return help_list_all();

    const tt_mcp_tool_t *tool = find_tool(command);
    if (!tool)
    {
        /* Build error with available tool names */
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "unknown_tool");

        char msg[256];
        snprintf(msg, sizeof(msg), "Unknown tool: %s", command);
        cJSON_AddStringToObject(err, "message", msg);

        cJSON *available = cJSON_AddArrayToObject(err, "available");
        for (int i = 0; i < TT_MCP_TOOLS_COUNT; i++)
            cJSON_AddItemToArray(available,
                                 cJSON_CreateString(TT_MCP_TOOLS[i].name));
        return err;
    }

    return help_for_tool(tool);
}

int tt_cmd_help(tt_cli_opts_t *opts)
{
    const char *command = NULL;
    if (opts->positional_count > 0)
        command = opts->positional[0];

    cJSON *result = tt_cmd_help_exec(command);
    if (!result)
        return tt_output_error("internal_error", "Failed to generate help", NULL);

    /* Check if it's an error response */
    if (cJSON_GetObjectItem(result, "error"))
    {
        tt_json_print(result);
        cJSON_Delete(result);
        return 1;
    }

    return tt_output_success(result);
}
