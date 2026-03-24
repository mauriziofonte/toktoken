/*
 * parser_twig.h -- Parser for Twig templates (.twig).
 *
 * Extracts blocks, macros, set variables, template references
 * (extends/include/embed/import/from/use), HTML IDs, and
 * Stimulus.js data-controller attributes.
 */

#ifndef TT_PARSER_TWIG_H
#define TT_PARSER_TWIG_H

#include "symbol.h"

/*
 * tt_parse_twig -- Parse Twig (.twig) files for symbols.
 *
 * project_root: absolute path to the project root.
 * file_paths:   array of RELATIVE paths to .twig files.
 * file_count:   number of file paths.
 * out:          receives allocated array of symbols [caller-frees with tt_symbol_array_free].
 * out_count:    receives number of symbols produced.
 *
 * Returns 0 on success, -1 on error.
 */
int tt_parse_twig(const char *project_root, const char **file_paths, int file_count,
                  tt_symbol_t **out, int *out_count);

#endif /* TT_PARSER_TWIG_H */
