/*
 * cmd_security.h -- Security queries over parser-recorded sites.
 *
 * security:sinks       command execution, file writes, eval STRING
 * security:sources     request/environment/config/file inputs
 * security:sanitizers  escaping and validation helpers
 * security:acl-gates   access-control checks
 * security:flow        source -> sink chains across subroutine calls
 *
 * Sites are produced by language parsers that record them (Perl/CGI:
 * .pl .pm .cgi .psgi). Analysis is static and offline: no target code
 * is executed or evaluated.
 */

#ifndef TT_CMD_SECURITY_H
#define TT_CMD_SECURITY_H

#include "cli.h"
#include <cJSON.h>

cJSON *tt_cmd_security_sinks_exec(tt_cli_opts_t *opts);
cJSON *tt_cmd_security_sources_exec(tt_cli_opts_t *opts);
cJSON *tt_cmd_security_sanitizers_exec(tt_cli_opts_t *opts);
cJSON *tt_cmd_security_acl_gates_exec(tt_cli_opts_t *opts);
cJSON *tt_cmd_security_flow_exec(tt_cli_opts_t *opts);

int tt_cmd_security_sinks(tt_cli_opts_t *opts);
int tt_cmd_security_sources(tt_cli_opts_t *opts);
int tt_cmd_security_sanitizers(tt_cli_opts_t *opts);
int tt_cmd_security_acl_gates(tt_cli_opts_t *opts);
int tt_cmd_security_flow(tt_cli_opts_t *opts);

#endif /* TT_CMD_SECURITY_H */
