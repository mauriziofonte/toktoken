/*
 * cmd_inspect.h -- inspect:outline, inspect:symbol, inspect:file, inspect:tree.
 *
 * Dual-mode: _exec() returns cJSON* for MCP, wrapper returns exit code for CLI.
 */

#ifndef TT_CMD_INSPECT_H
#define TT_CMD_INSPECT_H

#include "cli.h"

#include <cJSON.h>

/* ---- Core functions (return cJSON*, caller frees) ---- */

cJSON *tt_cmd_inspect_outline_exec(tt_cli_opts_t *opts);
cJSON *tt_cmd_inspect_file_exec(tt_cli_opts_t *opts);
cJSON *tt_cmd_inspect_tree_exec(tt_cli_opts_t *opts);
cJSON *tt_cmd_inspect_dependencies_exec(tt_cli_opts_t *opts);
cJSON *tt_cmd_inspect_hierarchy_exec(tt_cli_opts_t *opts);

/*
 * tt_cmd_inspect_symbol_exec -- Core inspect:symbol logic.
 *
 * Returns cJSON result. *out_exit_code receives the exit code:
 *   0 = all symbols found
 *   1 = no symbols found
 *   2 = partial success
 */
cJSON *tt_cmd_inspect_symbol_exec(tt_cli_opts_t *opts, int *out_exit_code);

/* ---- CLI wrappers (print JSON, return exit code) ---- */

int tt_cmd_inspect_outline(tt_cli_opts_t *opts);
int tt_cmd_inspect_symbol(tt_cli_opts_t *opts);
int tt_cmd_inspect_file(tt_cli_opts_t *opts);
int tt_cmd_inspect_tree(tt_cli_opts_t *opts);
int tt_cmd_inspect_dependencies(tt_cli_opts_t *opts);
int tt_cmd_inspect_hierarchy(tt_cli_opts_t *opts);
int tt_cmd_inspect_cycles(tt_cli_opts_t *opts);
int tt_cmd_inspect_blast(tt_cli_opts_t *opts);

cJSON *tt_cmd_inspect_cycles_exec(tt_cli_opts_t *opts);
cJSON *tt_cmd_inspect_blast_exec(tt_cli_opts_t *opts);

#endif /* TT_CMD_INSPECT_H */
