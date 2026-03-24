/*
 * parser_razor.h -- Parser for ASP.NET Razor views (.cshtml).
 *
 * Extracts C# methods from @functions/@code blocks,
 * HTML element IDs, and @model/@using/@inject directives.
 */

#ifndef TT_PARSER_RAZOR_H
#define TT_PARSER_RAZOR_H

#include "symbol.h"

/*
 * tt_parse_razor -- Parse Razor (.cshtml) files for symbols.
 *
 * project_root: absolute path to the project root.
 * file_paths:   array of RELATIVE paths to .cshtml files.
 * file_count:   number of file paths.
 * out:          receives allocated array of symbols [caller-frees with tt_symbol_array_free].
 * out_count:    receives number of symbols produced.
 *
 * Returns 0 on success, -1 on error.
 */
int tt_parse_razor(const char *project_root, const char **file_paths, int file_count,
                   tt_symbol_t **out, int *out_count);

#endif /* TT_PARSER_RAZOR_H */
