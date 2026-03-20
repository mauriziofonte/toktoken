/*
 * cmd_index.h -- index:create and index:update commands.
 *
 * Dual-mode: _exec() returns cJSON* for MCP, wrapper returns exit code for CLI.
 */

#ifndef TT_CMD_INDEX_H
#define TT_CMD_INDEX_H

#include "cli.h"

#include <cJSON.h>
#include <signal.h>

/* Signal flag for clean interruption */
extern volatile sig_atomic_t tt_interrupted;

/* ---- Core functions (return cJSON*, caller frees) ---- */

/*
 * tt_cmd_index_create_exec -- Core index:create logic.
 *
 * Returns cJSON result on success, NULL on internal error (check tt_error_get).
 * Result may contain "error" field for user-facing errors.
 */
cJSON *tt_cmd_index_create_exec(tt_cli_opts_t *opts);

/*
 * tt_cmd_index_update_exec -- Core index:update logic.
 *
 * Returns cJSON result on success, NULL on internal error (check tt_error_get).
 * Result may contain "error" field for user-facing errors.
 */
cJSON *tt_cmd_index_update_exec(tt_cli_opts_t *opts);

/* ---- CLI wrappers (print JSON, return exit code) ---- */

int tt_cmd_index_create(tt_cli_opts_t *opts);
int tt_cmd_index_update(tt_cli_opts_t *opts);
int tt_cmd_index_file(tt_cli_opts_t *opts);

cJSON *tt_cmd_index_file_exec(tt_cli_opts_t *opts);

#endif /* TT_CMD_INDEX_H */
