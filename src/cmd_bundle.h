/*
 * cmd_bundle.h -- inspect:bundle command.
 *
 * Returns a self-contained context bundle for a symbol:
 *   - Symbol definition with source code
 *   - All import statements from the symbol's file
 *   - Sibling symbols in the same file (outline)
 *   - Optionally, files that import the symbol's file (callers)
 *
 * Dual-mode: _exec() returns cJSON* for MCP, wrapper returns exit code for CLI.
 */

#ifndef TT_CMD_BUNDLE_H
#define TT_CMD_BUNDLE_H

#include "cli.h"
#include <cJSON.h>

cJSON *tt_cmd_inspect_bundle_exec(tt_cli_opts_t *opts);
int tt_cmd_inspect_bundle(tt_cli_opts_t *opts);

/* Render a bundle JSON result as markdown. [caller-frees] */
char *tt_bundle_render_markdown(const cJSON *result);

#endif /* TT_CMD_BUNDLE_H */
