/*
 * cmd_search.h -- search:symbols and search:text commands.
 *
 * Dual-mode: _exec() returns cJSON* for MCP, wrapper returns exit code for CLI.
 */

#ifndef TT_CMD_SEARCH_H
#define TT_CMD_SEARCH_H

#include "cli.h"

#include <cJSON.h>

/* ---- Core functions (return cJSON*, caller frees) ---- */

cJSON *tt_cmd_search_symbols_exec(tt_cli_opts_t *opts);

/*
 * tt_cmd_search_defs_exec -- Exact-name definition lookup (--defs).
 *
 * Resolves several names in one call and returns the search:symbols shape
 * plus "not_found" for names with no definition. Used by the CLI when
 * --defs is set and by MCP search_symbols with defs=true.
 */
cJSON *tt_cmd_search_defs_exec(tt_cli_opts_t *opts, const char *const *names,
                               int name_count);
cJSON *tt_cmd_search_text_exec(tt_cli_opts_t *opts);
cJSON *tt_cmd_search_cooccurrence_exec(tt_cli_opts_t *opts);
cJSON *tt_cmd_search_similar_exec(tt_cli_opts_t *opts);

/* ---- CLI wrappers (print JSON, return exit code) ---- */

int tt_cmd_search_symbols(tt_cli_opts_t *opts);
int tt_cmd_search_text(tt_cli_opts_t *opts);
int tt_cmd_search_cooccurrence(tt_cli_opts_t *opts);
int tt_cmd_search_similar(tt_cli_opts_t *opts);

#endif /* TT_CMD_SEARCH_H */
