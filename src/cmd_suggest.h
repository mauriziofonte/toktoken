/*
 * cmd_suggest.h -- suggest command: onboarding discovery tool.
 *
 * Surfaces top keywords, most-imported files, kind/language distribution,
 * and ready-to-run example queries for exploring unfamiliar repos.
 */

#ifndef TT_CMD_SUGGEST_H
#define TT_CMD_SUGGEST_H

#include "cli.h"
#include <cJSON.h>

int tt_cmd_suggest(tt_cli_opts_t *opts);
cJSON *tt_cmd_suggest_exec(tt_cli_opts_t *opts);

#endif /* TT_CMD_SUGGEST_H */
