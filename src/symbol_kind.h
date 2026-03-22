/*
 * symbol_kind.h -- Symbol kind enum and ctags kind mapping.
 *
 * 15 canonical symbol kinds. Maps ctags output kinds (35 entries)
 * to canonical values. Unknown kinds fall back to TT_KIND_VARIABLE.
 */

#ifndef TT_SYMBOL_KIND_H
#define TT_SYMBOL_KIND_H

#include <stdbool.h>

typedef enum
{
    TT_KIND_CLASS,
    TT_KIND_INTERFACE,
    TT_KIND_TRAIT,
    TT_KIND_ENUM,
    TT_KIND_FUNCTION,
    TT_KIND_METHOD,
    TT_KIND_CONSTANT,
    TT_KIND_PROPERTY,
    TT_KIND_VARIABLE,
    TT_KIND_NAMESPACE,
    TT_KIND_TYPE,
    TT_KIND_DIRECTIVE,
    /* Documentation kinds (Markdown, reStructuredText, etc.) */
    TT_KIND_CHAPTER,
    TT_KIND_SECTION,
    TT_KIND_SUBSECTION
} tt_symbol_kind_e;

/* Total number of canonical kinds. */
#define TT_KIND_COUNT 15

/*
 * tt_kind_from_ctags -- Convert ctags kind string to enum.
 *
 * Uses the 35-entry CTAGS_MAP. If not found, tries matching the
 * lowercased string against canonical kind names. Final fallback:
 * TT_KIND_VARIABLE.
 */
tt_symbol_kind_e tt_kind_from_ctags(const char *ctags_kind);

/*
 * tt_kind_to_str -- Convert enum to canonical string.
 *
 * Returns static string: "class", "method", etc.
 */
const char *tt_kind_to_str(tt_symbol_kind_e kind);

/*
 * tt_kind_label -- Human-readable label for display.
 *
 * Returns static string: "Class", "Method", "Type definition", etc.
 */
const char *tt_kind_label(tt_symbol_kind_e kind);

/*
 * tt_kind_is_single_line -- True for kinds that span a single line.
 *
 * Single-line: constant, variable, property, namespace, type, directive.
 * Multi-line:  class, interface, trait, enum, function, method,
 *              chapter, section, subsection.
 */
bool tt_kind_is_single_line(tt_symbol_kind_e kind);

/*
 * tt_kind_is_valid -- Check if a string is a valid canonical kind name.
 *
 * Returns true for: class, interface, trait, enum, function, method,
 * constant, property, variable, namespace, type, directive,
 * chapter, section, subsection.
 * Case-insensitive comparison.
 */
bool tt_kind_is_valid(const char *kind);

#endif /* TT_SYMBOL_KIND_H */
