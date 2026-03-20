/*
 * cmd_find.h -- find:importers and find:references commands.
 */

#ifndef TT_CMD_FIND_H
#define TT_CMD_FIND_H

#include "cli.h"
#include <cJSON.h>

int tt_cmd_find_importers(tt_cli_opts_t *opts);
int tt_cmd_find_references(tt_cli_opts_t *opts);
int tt_cmd_find_callers(tt_cli_opts_t *opts);
int tt_cmd_find_dead(tt_cli_opts_t *opts);

cJSON *tt_cmd_find_importers_exec(tt_cli_opts_t *opts);
cJSON *tt_cmd_find_references_exec(tt_cli_opts_t *opts);
cJSON *tt_cmd_find_callers_exec(tt_cli_opts_t *opts);
cJSON *tt_cmd_find_dead_exec(tt_cli_opts_t *opts);

#endif /* TT_CMD_FIND_H */
