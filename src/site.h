/*
 * site.h -- Security-relevant code sites (sinks, sources, sanitizers,
 * ACL gates, call sites) extracted by language parsers.
 *
 * A site is a single occurrence in a file, attributed to its enclosing
 * named scope (subroutine symbol ID) or to the file scope (NULL).
 * Taint-lite origin masks summarize, per scope, which origins can reach
 * an expression: request/environment/config/file sources and the
 * scope's own positional parameters. No program evaluation is involved.
 */

#ifndef TT_SITE_H
#define TT_SITE_H

#include <stdint.h>

typedef enum
{
    TT_SITE_SINK,      /* command execution, file write, eval STRING */
    TT_SITE_SOURCE,    /* attacker-influenced input */
    TT_SITE_SANITIZER, /* escaping / validation helpers */
    TT_SITE_ACL,       /* access-control gates */
    TT_SITE_CALL,      /* call of a user-defined subroutine */
    TT_SITE_HARNESS    /* framework configuration (e.g. Webmin unauthcgi) */
} tt_site_category_e;

/* ---- Taint-lite origin mask bits ---- */

#define TT_TAINT_IN        (1u << 0)  /* CGI request: $in{...}, %in */
#define TT_TAINT_ENV       (1u << 1)  /* $ENV{...}, %ENV */
#define TT_TAINT_ARGV      (1u << 2)  /* @ARGV */
#define TT_TAINT_CONFIG    (1u << 3)  /* $config{...}, $gconfig{...} */
#define TT_TAINT_FILE      (1u << 4)  /* read_file* results, <FH> */
#define TT_TAINT_STDIN     (1u << 5)  /* <STDIN> */
#define TT_TAINT_SOURCES   0x3Fu      /* all source bits */

#define TT_TAINT_PARAM0_BIT 8         /* bits 8..23: positional params 0..15 */
#define TT_TAINT_MAX_PARAMS 16
#define TT_TAINT_PARAM(i)  (1u << (TT_TAINT_PARAM0_BIT + (i)))
#define TT_TAINT_PARAMS    0x00FFFF00u
#define TT_TAINT_ALL_ARGS  (1u << 24) /* @_ forwarded as a whole */
#define TT_TAINT_SANITIZED (1u << 30) /* passed through a sanitizer call */

typedef struct
{
    char *file;                  /* [owns] relative path */
    int line;                    /* 1-indexed */
    tt_site_category_e category;
    char *name;                  /* [owns] canonical name (system, open_pipe, in, callee...) */
    char *qualifier;             /* [owns] package/module qualifier, "->" for methods, or NULL */
    char *scope_id;              /* [owns] enclosing subroutine symbol ID, NULL = file scope */
    char *args;                  /* [owns] raw argument text (collapsed, truncated), never NULL */
    uint32_t mask;               /* origin mask of the argument expression */
    char *arg_masks;             /* [owns] calls: JSON array of per-argument masks, or NULL */
    char *trace;                 /* [owns] sinks: "var@line" witnesses, or NULL */
} tt_site_t;

/* Canonical string for a category ("sink", "source", ...). */
const char *tt_site_category_str(tt_site_category_e cat);

/* Parse a category string. Returns -1 if unknown. */
int tt_site_category_from_str(const char *s);

/* Free owned strings of one site (not the struct). */
void tt_site_free(tt_site_t *site);

/* Free an array of sites and the array itself. */
void tt_site_array_free(tt_site_t *sites, int count);

#endif /* TT_SITE_H */
