#ifndef TT_TEXT_RENDER_H
#define TT_TEXT_RENDER_H

#include <cJSON.h>
#include "cli.h"

/*
 * Agent-line ("grep-style") text renderers.
 *
 * Each renderer turns the JSON document returned by an _exec() function
 * into compact text lines. Nothing below changes the JSON contract: this
 * is a rendering of the same result, so the exec path, savings tracking
 * and JSON consumers are untouched. The CLI wrappers and the MCP server
 * both call these functions, so a line shape is defined exactly once.
 *
 * A renderer returns NULL when its result cannot be expressed as text (a
 * JSON error object, or a mode with no grep equivalent such as --count or
 * --group-by); the caller then keeps the JSON output.
 *
 * All non-NULL returns are heap strings. [caller-frees]
 */

/* True when --format text was requested on the command line. */
bool tt_text_format_requested(const tt_cli_opts_t *opts);

/*
 * search:symbols -> "file:line:kind name" per symbol, with " id=..." only
 * when the id carries information the line does not show, "// not found:"
 * for --defs misses, and an "n=<count>" trailer.
 */
char *tt_render_search_symbols_text(const cJSON *result);

/*
 * search:text -> "file:line:text" for hits, "file-line-text" for context
 * lines, and an "n=<count>" trailer. With no_comments, blank and
 * comment-only context lines are dropped; hit lines are always kept.
 */
char *tt_render_search_text_text(const cJSON *result, bool no_comments);

/*
 * inspect:symbol -> "// file:line-end kind name" followed by the source.
 * With grep_pat, only the matching body lines are printed, each with its
 * real file line number, and the header records "[grep: K of N lines]".
 * With no_comments, blank and comment-only body lines are dropped.
 */
char *tt_render_inspect_symbol_text(const cJSON *result, bool no_comments,
                                    const char *grep_pat);

/* inspect:file -> "N: line" using the file's real line numbers. */
char *tt_render_inspect_file_text(const cJSON *result, bool no_comments);

#endif /* TT_TEXT_RENDER_H */
