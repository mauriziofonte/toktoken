/*
 * parser_blade.c -- Parser for Laravel Blade templates.
 *
 * 3-pass architecture:
 *   Pass 0: Dead zone marking ({{-- comments --}}, @verbatim blocks)
 *   Pass 1: @directive('argument') extraction (19 recognized directives)
 *   Pass 2: <x-component> and <x-slot:name> extraction
 *
 * Manual pattern matching (no PCRE2 required).
 */

#include "parser_blade.h"
#include "symbol.h"
#include "symbol_kind.h"
#include "line_offsets.h"
#include "fast_hash.h"
#include "str_util.h"
#include "platform.h"
#include "hashmap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── Dead zone infrastructure ─────────────────────────────────────── */

typedef struct { size_t start; size_t end; } dead_zone_t;
typedef struct { dead_zone_t *zones; int count; int cap; } dead_zones_t;

static void dz_init(dead_zones_t *dz)
{
    dz->zones = NULL;
    dz->count = 0;
    dz->cap = 0;
}

static void dz_add(dead_zones_t *dz, size_t start, size_t end)
{
    if (dz->count >= dz->cap) {
        int nc = dz->cap ? dz->cap * 2 : 8;
        dead_zone_t *tmp = realloc(dz->zones, (size_t)nc * sizeof(dead_zone_t));
        if (!tmp) return;
        dz->zones = tmp;
        dz->cap = nc;
    }
    dz->zones[dz->count++] = (dead_zone_t){start, end};
}

static void dz_free(dead_zones_t *dz)
{
    free(dz->zones);
    dz->zones = NULL;
    dz->count = dz->cap = 0;
}

static bool dz_is_dead(const dead_zones_t *dz, size_t pos)
{
    for (int i = 0; i < dz->count; i++) {
        if (pos >= dz->zones[i].start && pos < dz->zones[i].end)
            return true;
    }
    return false;
}

/* ── Pass 0: Build dead zones ─────────────────────────────────────── */

static void build_dead_zones(const char *content, size_t len, dead_zones_t *dz)
{
    size_t pos = 0;
    while (pos < len) {
        /* {{-- ... --}} Blade comments */
        if (pos + 3 < len && content[pos] == '{' && content[pos + 1] == '{' &&
            content[pos + 2] == '-' && content[pos + 3] == '-') {
            size_t start = pos;
            pos += 4;
            while (pos + 3 <= len) {
                if (content[pos] == '-' && content[pos + 1] == '-' &&
                    content[pos + 2] == '}' && content[pos + 3] == '}') {
                    pos += 4;
                    break;
                }
                pos++;
            }
            dz_add(dz, start, pos);
            continue;
        }

        /* @verbatim ... @endverbatim */
        if (content[pos] == '@' && pos + 9 < len &&
            strncmp(content + pos + 1, "verbatim", 8) == 0 &&
            !isalnum((unsigned char)content[pos + 9])) {
            size_t start = pos;
            pos += 9;
            const char *end_tag = "@endverbatim";
            size_t end_len = 12;
            while (pos + end_len <= len) {
                if (strncmp(content + pos, end_tag, end_len) == 0) {
                    pos += end_len;
                    break;
                }
                pos++;
            }
            dz_add(dz, start, pos);
            continue;
        }

        pos++;
    }
}

/* ── Recognized Blade directives ──────────────────────────────────── */

static const char *BLADE_DIRECTIVES[] = {
    "extends", "section", "yield", "component", "livewire",
    "include", "includeIf", "includeWhen", "includeFirst",
    "slot", "push", "stack", "prepend",
    "each", "inject", "use",
    "pushOnce", "pushIf", "prependOnce"};
static const int BLADE_DIRECTIVE_COUNT = 19;

/* Normalize directive name to canonical form. */
static const char *normalize_directive(const char *directive)
{
    if (strcmp(directive, "includeIf") == 0 ||
        strcmp(directive, "includeWhen") == 0 ||
        strcmp(directive, "includeFirst") == 0)
        return "include";
    if (strcmp(directive, "prepend") == 0 ||
        strcmp(directive, "pushOnce") == 0 ||
        strcmp(directive, "prependOnce") == 0)
        return "push";
    if (strcmp(directive, "pushIf") == 0)
        return "push";
    return directive;
}

/* ── Helpers ──────────────────────────────────────────────────────── */

/* Get byte offset and length of a 1-indexed line. */
static void get_line_bytes(const tt_line_offsets_t *lo, int line_num,
                           size_t content_len,
                           int *out_offset, int *out_length)
{
    *out_offset = tt_line_offsets_offset_at(lo, line_num);
    int next;
    if (line_num + 1 <= lo->count)
        next = tt_line_offsets_offset_at(lo, line_num + 1);
    else
        next = (int)content_len;
    *out_length = next - *out_offset;
}

/* Grow symbols array if at capacity. */
static int grow_symbols(tt_symbol_t **syms, int *count, int *cap)
{
    if (*count < *cap)
        return 0;
    int new_cap = (*cap) * 2;
    if (new_cap < 16)
        new_cap = 16;
    tt_symbol_t *tmp = realloc(*syms, (size_t)new_cap * sizeof(tt_symbol_t));
    if (!tmp)
        return -1;
    *syms = tmp;
    *cap = new_cap;
    return 0;
}

/*
 * Build keywords for a Blade directive.
 * words = [directive] + split(argument, "." or "\") → lowercase, unique, filter >1 char.
 * Returns JSON array string. [caller-frees]
 */
static char *blade_extract_keywords(const char *directive, const char *argument)
{
    char *parts[64];
    int part_count = 0;

    parts[part_count++] = tt_str_tolower(directive);

    /* Split argument by '.' and '\' */
    const char *s = argument;
    while (*s && part_count < 63) {
        const char *sep = s;
        while (*sep && *sep != '.' && *sep != '\\')
            sep++;
        size_t plen = (size_t)(sep - s);
        if (plen > 1) {
            char *chunk = tt_strndup(s, plen);
            if (chunk) {
                char *lower = tt_str_tolower(chunk);
                free(chunk);
                if (lower) parts[part_count++] = lower;
            }
        }
        if (!*sep) break;
        s = sep + 1;
    }

    /* Build JSON array with dedup */
    tt_strbuf_t sb;
    tt_strbuf_init(&sb);
    tt_strbuf_append_char(&sb, '[');
    int written = 0;
    for (int i = 0; i < part_count; i++) {
        bool dup = false;
        for (int j = 0; j < i; j++) {
            if (strcmp(parts[i], parts[j]) == 0) { dup = true; break; }
        }
        if (dup || strlen(parts[i]) <= 1) continue;
        if (written > 0) tt_strbuf_append_char(&sb, ',');
        tt_strbuf_appendf(&sb, "\"%s\"", parts[i]);
        written++;
    }
    tt_strbuf_append_char(&sb, ']');

    for (int i = 0; i < part_count; i++)
        free(parts[i]);

    return tt_strbuf_detach(&sb);
}

/* Add a symbol to the array. Returns 0 on success. */
static int add_symbol(tt_symbol_t **symbols, int *sym_count, int *sym_cap,
                      const char *rel_path, const char *name,
                      const char *qualified_name, const char *signature,
                      const char *keywords, const char *content_hash,
                      int line_num, int byte_offset, int byte_length)
{
    if (grow_symbols(symbols, sym_count, sym_cap) < 0)
        return -1;

    char *id = tt_symbol_make_id(rel_path, qualified_name, "directive", 0);
    tt_symbol_t *sym = &(*symbols)[*sym_count];
    sym->id = id;
    sym->file = tt_strdup(rel_path);
    sym->name = tt_strdup(name);
    sym->qualified_name = tt_strdup(qualified_name);
    sym->kind = TT_KIND_DIRECTIVE;
    sym->language = tt_strdup("blade");
    sym->signature = tt_strdup(signature);
    sym->docstring = tt_strdup("");
    sym->summary = tt_strdup("");
    sym->decorators = tt_strdup("[]");
    sym->keywords = tt_strdup(keywords);
    sym->parent_id = NULL;
    sym->line = line_num;
    sym->end_line = line_num;
    sym->byte_offset = byte_offset;
    sym->byte_length = byte_length;
    sym->content_hash = tt_strdup(content_hash);
    (*sym_count)++;
    return 0;
}

/* ── Pass 1: @directive('argument') extraction ────────────────────── */

/*
 * Extract a single quoted string starting at *p (expects p to point at ' or ").
 * Returns the extracted string (caller-frees) and advances *p past the closing quote.
 * Returns NULL if no valid string found.
 */
static char *extract_quoted_string(const char **pp, const char *end)
{
    const char *p = *pp;
    if (p >= end || (*p != '\'' && *p != '"'))
        return NULL;
    char quote = *p;
    p++;
    const char *start = p;
    while (p < end && *p != quote && *p != '\n')
        p++;
    if (p >= end || *p != quote)
        return NULL;
    char *result = tt_strndup(start, (size_t)(p - start));
    *pp = p + 1;
    return result;
}

static void pass1_directives(const char *content, size_t content_len,
                             const tt_line_offsets_t *lo,
                             const dead_zones_t *dz,
                             const char *rel_path,
                             tt_symbol_t **symbols, int *sym_count, int *sym_cap,
                             tt_hashmap_t *seen)
{
    size_t pos = 0;
    while (pos < content_len) {
        const char *at = memchr(content + pos, '@', content_len - pos);
        if (!at) break;
        size_t at_pos = (size_t)(at - content);
        pos = at_pos + 1;

        if (dz_is_dead(dz, at_pos))
            continue;

        for (int d = 0; d < BLADE_DIRECTIVE_COUNT; d++) {
            size_t dlen = strlen(BLADE_DIRECTIVES[d]);
            if (at_pos + 1 + dlen > content_len)
                continue;
            if (strncmp(at + 1, BLADE_DIRECTIVES[d], dlen) != 0)
                continue;

            /* Ensure the directive name isn't a prefix of a longer identifier */
            char next_ch = (at_pos + 1 + dlen < content_len) ? at[1 + dlen] : '\0';
            if (isalnum((unsigned char)next_ch) || next_ch == '_')
                continue;

            const char *directive_name = BLADE_DIRECTIVES[d];
            const char *normalized = normalize_directive(directive_name);
            const char *p = at + 1 + dlen;
            const char *end = content + content_len;

            /* Skip whitespace */
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
                p++;
            if (p >= end || *p != '(')
                continue;
            p++;

            /* Skip whitespace */
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
                p++;

            /*
             * @pushIf and @includeWhen: first arg is a condition/expression,
             * second arg is the string we want. Scan past first arg to comma.
             */
            bool skip_first_arg = (strcmp(directive_name, "pushIf") == 0 ||
                                   strcmp(directive_name, "includeWhen") == 0);

            if (skip_first_arg) {
                /* Skip to comma separating first arg from second */
                int depth = 0;
                while (p < end) {
                    if (*p == '(') depth++;
                    else if (*p == ')') {
                        if (depth == 0) break;
                        depth--;
                    }
                    else if (*p == ',' && depth == 0) {
                        p++;
                        break;
                    }
                    p++;
                }
                /* Skip whitespace after comma */
                while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
                    p++;
            }

            /*
             * @includeFirst: expect ['path1', 'path2'] list syntax
             */
            if (strcmp(directive_name, "includeFirst") == 0 && p < end && *p == '[') {
                p++; /* skip '[' */
                while (p < end && *p != ']') {
                    while (p < end && *p != '\'' && *p != '"' && *p != ']')
                        p++;
                    if (p >= end || *p == ']') break;

                    char *arg = extract_quoted_string(&p, end);
                    if (!arg) { p++; continue; }

                    /* Build dedup key */
                    char dedup_key[512];
                    snprintf(dedup_key, sizeof(dedup_key), "%s:%s", normalized, arg);
                    intptr_t occ = 0;
                    if (tt_hashmap_has(seen, dedup_key))
                        occ = (intptr_t)tt_hashmap_get(seen, dedup_key);
                    occ++;
                    tt_hashmap_set(seen, dedup_key, (void *)occ);

                    /* Build qualified name */
                    char qn[512];
                    if (occ > 1)
                        snprintf(qn, sizeof(qn), "%s.%s~%d", normalized, arg, (int)occ);
                    else
                        snprintf(qn, sizeof(qn), "%s.%s", normalized, arg);

                    /* Signature */
                    char sig[512];
                    snprintf(sig, sizeof(sig), "@%s('%s')", directive_name, arg);

                    int line_num = tt_line_offsets_line_at(lo, (int)at_pos);
                    int lbo, lbl;
                    get_line_bytes(lo, line_num, content_len, &lbo, &lbl);

                    char *hash = NULL;
                    if (lbl > 0 && lbo + lbl <= (int)content_len)
                        hash = tt_fast_hash_hex(content + lbo, (size_t)lbl);
                    if (!hash) hash = tt_strdup("");

                    char *kw = blade_extract_keywords(directive_name, arg);
                    add_symbol(symbols, sym_count, sym_cap,
                               rel_path, arg, qn, sig, kw, hash,
                               line_num, lbo, lbl);
                    free(kw);
                    free(hash);
                    free(arg);
                }
                break; /* matched this '@' */
            }

            /* Standard single-string argument */
            if (p >= end || (*p != '\'' && *p != '"'))
                continue;
            char *argument = extract_quoted_string(&p, end);
            if (!argument)
                continue;

            /* Dedup */
            char dedup_key[512];
            snprintf(dedup_key, sizeof(dedup_key), "%s:%s", normalized, argument);
            intptr_t occurrence = 0;
            if (tt_hashmap_has(seen, dedup_key))
                occurrence = (intptr_t)tt_hashmap_get(seen, dedup_key);
            occurrence++;
            tt_hashmap_set(seen, dedup_key, (void *)occurrence);

            /* Qualified name */
            char qn[512];
            if (occurrence > 1)
                snprintf(qn, sizeof(qn), "%s.%s~%d", normalized, argument, (int)occurrence);
            else
                snprintf(qn, sizeof(qn), "%s.%s", normalized, argument);

            /* Signature: uses ORIGINAL directive name */
            char sig[512];
            snprintf(sig, sizeof(sig), "@%s('%s')", directive_name, argument);

            int line_num = tt_line_offsets_line_at(lo, (int)at_pos);
            int line_byte_offset, line_byte_length;
            get_line_bytes(lo, line_num, content_len, &line_byte_offset, &line_byte_length);

            char *content_hash = NULL;
            if (line_byte_length > 0 &&
                line_byte_offset + line_byte_length <= (int)content_len)
                content_hash = tt_fast_hash_hex(content + line_byte_offset,
                                                (size_t)line_byte_length);
            if (!content_hash) content_hash = tt_strdup("");

            char *keywords = blade_extract_keywords(directive_name, argument);

            add_symbol(symbols, sym_count, sym_cap,
                       rel_path, argument, qn, sig, keywords, content_hash,
                       line_num, line_byte_offset, line_byte_length);

            free(keywords);
            free(content_hash);
            free(argument);
            break; /* matched this '@', move on */
        }
    }
}

/* ── Pass 2: <x-component> and <x-slot:name> extraction ───────────── */

static void pass2_components(const char *content, size_t content_len,
                             const tt_line_offsets_t *lo,
                             const dead_zones_t *dz,
                             const char *rel_path,
                             tt_symbol_t **symbols, int *sym_count, int *sym_cap,
                             tt_hashmap_t *seen)
{
    size_t pos = 0;
    while (pos + 3 < content_len) {
        /* Find '<x-' */
        const char *lt = memchr(content + pos, '<', content_len - pos);
        if (!lt) break;
        size_t lt_pos = (size_t)(lt - content);
        pos = lt_pos + 1;

        if (dz_is_dead(dz, lt_pos))
            continue;

        if (lt_pos + 2 >= content_len || lt[1] != 'x' || lt[2] != '-')
            continue;

        /* Check this isn't a closing tag </x-... */
        if (lt_pos > 0 && content[lt_pos - 1] == '/')
            continue;
        /* Actually check for </x- pattern (slash before the <) won't happen,
           but </x- as in content[lt_pos] = '<' and content[lt_pos+1] = '/' */

        /* Read component name: alphanumeric, '-', '.', ':' */
        const char *name_start = lt + 3;
        const char *end = content + content_len;
        const char *p = name_start;
        while (p < end && (isalnum((unsigned char)*p) || *p == '-' || *p == '.' || *p == ':' || *p == '_'))
            p++;

        size_t name_len = (size_t)(p - name_start);
        if (name_len == 0)
            continue;

        char *comp_name = tt_strndup(name_start, name_len);
        if (!comp_name) continue;

        /* Skip <x-dynamic-component> */
        if (strcmp(comp_name, "dynamic-component") == 0) {
            free(comp_name);
            continue;
        }

        /* Determine if this is <x-slot:name> */
        bool is_slot = (strncmp(comp_name, "slot:", 5) == 0 ||
                        (strncmp(comp_name, "slot", 4) == 0 && name_len == 4));
        const char *normalized;
        const char *display_name;

        if (is_slot && name_len > 5 && comp_name[4] == ':') {
            /* <x-slot:header> → slot.header */
            normalized = "slot";
            display_name = comp_name + 5;
        } else if (is_slot && name_len == 4) {
            /* <x-slot> — default slot, skip */
            free(comp_name);
            continue;
        } else {
            /* <x-alert> → component.alert */
            normalized = "component";
            display_name = comp_name;
        }

        /* Dedup */
        char dedup_key[512];
        snprintf(dedup_key, sizeof(dedup_key), "%s:%s", normalized, display_name);
        intptr_t occ = 0;
        if (tt_hashmap_has(seen, dedup_key))
            occ = (intptr_t)tt_hashmap_get(seen, dedup_key);
        occ++;
        tt_hashmap_set(seen, dedup_key, (void *)occ);

        /* Qualified name */
        char qn[512];
        if (occ > 1)
            snprintf(qn, sizeof(qn), "%s.%s~%d", normalized, display_name, (int)occ);
        else
            snprintf(qn, sizeof(qn), "%s.%s", normalized, display_name);

        /* Signature */
        char sig[256];
        if (is_slot)
            snprintf(sig, sizeof(sig), "<x-slot:%s>", display_name);
        else
            snprintf(sig, sizeof(sig), "<x-%s>", display_name);

        int line_num = tt_line_offsets_line_at(lo, (int)lt_pos);
        int lbo, lbl;
        get_line_bytes(lo, line_num, content_len, &lbo, &lbl);

        char *hash = NULL;
        if (lbl > 0 && lbo + lbl <= (int)content_len)
            hash = tt_fast_hash_hex(content + lbo, (size_t)lbl);
        if (!hash) hash = tt_strdup("");

        char *keywords = blade_extract_keywords(normalized, display_name);
        add_symbol(symbols, sym_count, sym_cap,
                   rel_path, display_name, qn, sig, keywords, hash,
                   line_num, lbo, lbl);
        free(keywords);
        free(hash);
        free(comp_name);
    }
}

/* ── Entry point ──────────────────────────────────────────────────── */

int tt_parse_blade(const char *project_root, const char **file_paths, int file_count,
                   tt_symbol_t **out, int *out_count)
{
    if (!out || !out_count)
        return -1;
    *out = NULL;
    *out_count = 0;

    if (!project_root || !file_paths || file_count <= 0)
        return 0;

    int sym_cap = 32;
    int sym_count = 0;
    tt_symbol_t *symbols = malloc((size_t)sym_cap * sizeof(tt_symbol_t));
    if (!symbols)
        return -1;

    for (int fi = 0; fi < file_count; fi++) {
        const char *rel_path = file_paths[fi];
        char *full_path = tt_path_join(project_root, rel_path);
        if (!full_path) continue;

        size_t content_len = 0;
        char *content = tt_read_file(full_path, &content_len);
        free(full_path);
        if (!content) continue;

        tt_line_offsets_t lo;
        tt_line_offsets_build(&lo, content, content_len);

        /* Pass 0: dead zones */
        dead_zones_t dz;
        dz_init(&dz);
        build_dead_zones(content, content_len, &dz);

        /* Shared dedup map across passes */
        tt_hashmap_t *seen = tt_hashmap_new(32);

        /* Pass 1: @directive('argument') */
        pass1_directives(content, content_len, &lo, &dz, rel_path,
                         &symbols, &sym_count, &sym_cap, seen);

        /* Pass 2: <x-component> and <x-slot:name> */
        pass2_components(content, content_len, &lo, &dz, rel_path,
                         &symbols, &sym_count, &sym_cap, seen);

        tt_hashmap_free(seen);
        dz_free(&dz);
        free(content);
        tt_line_offsets_free(&lo);
    }

    *out = symbols;
    *out_count = sym_count;
    return 0;
}
