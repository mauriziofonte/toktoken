/*
 * parser_perl.h -- Perl 5 parser: symbols, imports and security sites.
 *
 * Replaces ctags for Perl (.pl, .pm, .cgi, .psgi). Lexer-level handling
 * of POD, comments, strings, heredocs, quote-like operators and the
 * __END__/__DATA__ markers, so that subroutine bodies are brace-matched
 * correctly and statements inside strings/POD never produce edges.
 *
 * Never executes or evaluates Perl.
 */

#ifndef TT_PARSER_PERL_H
#define TT_PARSER_PERL_H

#include "symbol.h"
#include "import_extractor.h"
#include "site.h"

#include <stdbool.h>
#include <stddef.h>

/* File flags (stored in files.flags). */
#define TT_FILE_FLAG_CGI     1  /* CGI entry point: .cgi with shebang, no package */
#define TT_FILE_FLAG_UNAUTH  2  /* reachable before authentication (framework config) */
#define TT_FILE_FLAG_SCRIPT  4  /* non-CGI script entry point: shebang, no package */

typedef struct
{
    tt_symbol_t *symbols; /* [owns] */
    int sym_count;
    int sym_cap;
    tt_import_t *imports; /* [owns] */
    int imp_count;
    int imp_cap;
    tt_site_t *sites; /* [owns] */
    int site_count;
    int site_cap;
    int file_flags; /* TT_FILE_FLAG_* */
} tt_perl_result_t;

/*
 * tt_perl_is_perl_path -- True for extensions handled by this parser
 * (.pl .pm .cgi .psgi .t is NOT included: test files keep ctags/none).
 */
bool tt_perl_is_perl_path(const char *rel_path);

/*
 * tt_perl_shebang_language -- Language named by a "#!" first line
 * ("perl", "bash", "python", ...) or NULL when there is no shebang.
 * Returns a static string.
 */
const char *tt_perl_shebang_language(const char *content, size_t len);

/*
 * tt_parse_perl_content -- Parse one Perl file.
 *
 * content must be NUL-terminated at content[len]. rel_path is the
 * project-relative path used for symbol IDs and sites. Appends to out
 * (which must be zero-initialized by the caller on first use).
 * Returns 0 on success, -1 on allocation failure.
 */
int tt_parse_perl_content(const char *rel_path, const char *content, size_t len,
                          tt_perl_result_t *out);

/*
 * tt_perl_builtins_sorted -- Self-check: the builtin table used by binary
 * search is strictly sorted (strcmp order).
 */
bool tt_perl_builtins_sorted(void);

/* Free everything owned by a result (arrays and their elements). */
void tt_perl_result_free(tt_perl_result_t *r);

#endif /* TT_PARSER_PERL_H */
