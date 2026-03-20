/*
 * cmd_help.h -- help introspection command.
 *
 * Lists all tools or returns detailed usage for a specific tool.
 * Derives parameter info from MCP tool schemas at runtime.
 *
 * Dual-mode: _exec() returns cJSON* for MCP, wrapper returns exit code for CLI.
 */

#ifndef TT_CMD_HELP_H
#define TT_CMD_HELP_H

#include "cli.h"
#include <cJSON.h>

cJSON *tt_cmd_help_exec(const char *command);
int tt_cmd_help(tt_cli_opts_t *opts);

#endif /* TT_CMD_HELP_H */
