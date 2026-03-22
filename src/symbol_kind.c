/*
 * symbol_kind.c -- Symbol kind enum and ctags kind mapping.
 */

#include "symbol_kind.h"
#include "platform.h"

#include <string.h>
#include <ctype.h>

/* Canonical kind strings (indexed by tt_symbol_kind_e). */
static const char *KIND_STRINGS[TT_KIND_COUNT] = {
    "class",
    "interface",
    "trait",
    "enum",
    "function",
    "method",
    "constant",
    "property",
    "variable",
    "namespace",
    "type",
    "directive",
    "chapter",
    "section",
    "subsection"};

/* Human-readable labels (indexed by tt_symbol_kind_e). */
static const char *KIND_LABELS[TT_KIND_COUNT] = {
    "Class",
    "Interface",
    "Trait",
    "Enum",
    "Function",
    "Method",
    "Constant",
    "Property",
    "Variable",
    "Namespace",
    "Type definition",
    "Directive",
    "Chapter",
    "Section",
    "Subsection"};

/* CTAGS_MAP: 35 entries (15 identity + 20 aliases). */
typedef struct
{
    const char *ctags_kind;
    tt_symbol_kind_e canonical;
} ctags_map_entry_t;

static const ctags_map_entry_t CTAGS_MAP[] = {
    /* Identity mappings (12) */
    {"class", TT_KIND_CLASS},
    {"interface", TT_KIND_INTERFACE},
    {"trait", TT_KIND_TRAIT},
    {"enum", TT_KIND_ENUM},
    {"function", TT_KIND_FUNCTION},
    {"method", TT_KIND_METHOD},
    {"constant", TT_KIND_CONSTANT},
    {"property", TT_KIND_PROPERTY},
    {"variable", TT_KIND_VARIABLE},
    {"namespace", TT_KIND_NAMESPACE},
    {"type", TT_KIND_TYPE},
    {"directive", TT_KIND_DIRECTIVE},
    /* Aliases (17) */
    {"define", TT_KIND_CONSTANT},
    {"generator", TT_KIND_FUNCTION},
    {"struct", TT_KIND_CLASS},
    {"interfacedecl", TT_KIND_INTERFACE},
    {"func", TT_KIND_FUNCTION},
    {"member", TT_KIND_METHOD},
    {"anonmember", TT_KIND_PROPERTY},
    {"packagename", TT_KIND_NAMESPACE},
    {"module", TT_KIND_NAMESPACE},
    {"implementation", TT_KIND_CLASS},
    {"macro", TT_KIND_FUNCTION},
    {"enumconstant", TT_KIND_CONSTANT},
    {"field", TT_KIND_PROPERTY},
    {"prototype", TT_KIND_FUNCTION},
    {"externvar", TT_KIND_VARIABLE},
    {"typedef", TT_KIND_TYPE},
    {"enumerator", TT_KIND_CONSTANT},
    {"union", TT_KIND_TYPE},
    /* Documentation kinds (3 identity + 3 aliases) */
    {"chapter", TT_KIND_CHAPTER},
    {"section", TT_KIND_SECTION},
    {"subsection", TT_KIND_SUBSECTION},
    {"subsubsection", TT_KIND_SUBSECTION},
    {"l4subsection", TT_KIND_SUBSECTION},
    {"l5subsection", TT_KIND_SUBSECTION},
};

#define CTAGS_MAP_SIZE (sizeof(CTAGS_MAP) / sizeof(CTAGS_MAP[0]))

tt_symbol_kind_e tt_kind_from_ctags(const char *ctags_kind)
{
    if (!ctags_kind || !ctags_kind[0])
        return TT_KIND_VARIABLE;

    /* Step 1: Look up in CTAGS_MAP (case-insensitive). */
    for (size_t i = 0; i < CTAGS_MAP_SIZE; i++)
    {
        if (tt_strcasecmp(CTAGS_MAP[i].ctags_kind, ctags_kind) == 0)
            return CTAGS_MAP[i].canonical;
    }

    /* Step 2: Try matching against canonical kind names (case-insensitive). */
    for (int i = 0; i < TT_KIND_COUNT; i++)
    {
        if (tt_strcasecmp(KIND_STRINGS[i], ctags_kind) == 0)
            return (tt_symbol_kind_e)i;
    }

    /* Step 3: Fallback. */
    return TT_KIND_VARIABLE;
}

const char *tt_kind_to_str(tt_symbol_kind_e kind)
{
    if (kind >= 0 && kind < TT_KIND_COUNT)
        return KIND_STRINGS[kind];
    return "variable";
}

const char *tt_kind_label(tt_symbol_kind_e kind)
{
    if (kind >= 0 && kind < TT_KIND_COUNT)
        return KIND_LABELS[kind];
    return "Variable";
}

bool tt_kind_is_valid(const char *kind)
{
    if (!kind || !kind[0])
        return false;
    for (int i = 0; i < TT_KIND_COUNT; i++)
    {
        if (tt_strcasecmp(KIND_STRINGS[i], kind) == 0)
            return true;
    }
    return false;
}

bool tt_kind_is_single_line(tt_symbol_kind_e kind)
{
    switch (kind)
    {
    case TT_KIND_CONSTANT:
    case TT_KIND_VARIABLE:
    case TT_KIND_PROPERTY:
    case TT_KIND_NAMESPACE:
    case TT_KIND_TYPE:
    case TT_KIND_DIRECTIVE:
        return true;
    default:
        return false;
    }
}
