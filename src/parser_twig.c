/*
 * parser_twig.c -- Parser for Twig templates (.twig).
 *
 * 3-pass architecture:
 *   Pass 1: Mark dead zones ({# comments #}, verbatim, raw blocks)
 *   Pass 2: Extract {% tags %} (blocks, macros, set, extends, include, etc.)
 *   Pass 3: Extract HTML id="..." and data-controller="..." attributes
 *
 * Supports Twig 2.x and 3.x (identical syntax for all extractable tags).
 */

#include "parser_twig.h"
#include "symbol.h"
#include "symbol_kind.h"
#include "line_offsets.h"
#include "fast_hash.h"
#include "str_util.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ---- Dead zone tracking ---- */

typedef struct {
    size_t start;
    size_t end;
} dead_zone_t;

typedef struct {
    dead_zone_t *zones;
    int count;
    int cap;
} dead_zones_t;

static void dz_init(dead_zones_t *dz)
{
    dz->zones = NULL;
    dz->count = 0;
    dz->cap = 0;
}

static void dz_add(dead_zones_t *dz, size_t start, size_t end)
{
    if (dz->count >= dz->cap) {
        dz->cap = dz->cap ? dz->cap * 2 : 8;
        dead_zone_t *tmp = realloc(dz->zones, (size_t)dz->cap * sizeof(dead_zone_t));
        if (!tmp) return;
        dz->zones = tmp;
    }
    dz->zones[dz->count].start = start;
    dz->zones[dz->count].end = end;
    dz->count++;
}

static void dz_free(dead_zones_t *dz)
{
    free(dz->zones);
    dz->zones = NULL;
    dz->count = 0;
    dz->cap = 0;
}

static int dz_is_dead(const dead_zones_t *dz, size_t pos)
{
    for (int i = 0; i < dz->count; i++) {
        if (pos >= dz->zones[i].start && pos < dz->zones[i].end)
            return 1;
    }
    return 0;
}

/* ---- Symbol array management ---- */

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

static int add_symbol(tt_symbol_t **syms, int *count, int *cap,
                      const char *rel_path, const char *name,
                      const char *qualified_name, tt_symbol_kind_e kind,
                      const char *kind_str, const char *signature,
                      int line_num, const tt_line_offsets_t *lo,
                      const char *content, size_t content_len)
{
    if (grow_symbols(syms, count, cap) < 0)
        return -1;

    int byte_off = tt_line_offsets_offset_at(lo, line_num);
    int next_off = (line_num + 1 <= lo->count)
                       ? tt_line_offsets_offset_at(lo, line_num + 1)
                       : (int)content_len;
    int byte_len = next_off - byte_off;

    char *hash = NULL;
    if (byte_len > 0 && byte_off + byte_len <= (int)content_len)
        hash = tt_fast_hash_hex(content + byte_off, (size_t)byte_len);
    if (!hash)
        hash = tt_strdup("");

    char *id = tt_symbol_make_id(rel_path, qualified_name, kind_str, 0);

    char *lower = tt_str_tolower(name);
    tt_strbuf_t kw;
    tt_strbuf_init(&kw);
    tt_strbuf_appendf(&kw, "[\"%s\"]", lower ? lower : name);
    free(lower);

    tt_symbol_t *sym = &(*syms)[*count];
    sym->id = id;
    sym->file = tt_strdup(rel_path);
    sym->name = tt_strdup(name);
    sym->qualified_name = tt_strdup(qualified_name);
    sym->kind = kind;
    sym->language = tt_strdup("twig");
    sym->signature = tt_strdup(signature ? signature : "");
    sym->docstring = tt_strdup("");
    sym->summary = tt_strdup("");
    sym->decorators = tt_strdup("[]");
    sym->keywords = tt_strbuf_detach(&kw);
    sym->parent_id = NULL;
    sym->line = line_num;
    sym->end_line = line_num;
    sym->byte_offset = byte_off;
    sym->byte_length = byte_len;
    sym->content_hash = hash;
    (*count)++;
    return 0;
}

/* ---- Pass 1: Build dead zones ---- */

/* Find closing tag like {% endverbatim %} or {% endraw %} */
static const char *find_end_tag(const char *p, const char *end, const char *tag_name)
{
    size_t tlen = strlen(tag_name);
    while (p < end) {
        const char *found = memchr(p, '{', (size_t)(end - p));
        if (!found || found + 1 >= end)
            break;
        if (found[1] != '%') {
            p = found + 1;
            continue;
        }
        /* Skip whitespace modifier: {%- or {%~ */
        const char *tp = found + 2;
        if (tp < end && (*tp == '-' || *tp == '~'))
            tp++;
        while (tp < end && (*tp == ' ' || *tp == '\t'))
            tp++;
        if (tp + tlen <= end && strncmp(tp, tag_name, tlen) == 0) {
            /* Verify it ends with %} */
            const char *cp = tp + tlen;
            while (cp < end && (*cp == ' ' || *cp == '\t'))
                cp++;
            if (cp < end && *cp == '-') cp++;
            if (cp < end && *cp == '~') cp++;
            if (cp + 1 < end && cp[0] == '%' && cp[1] == '}')
                return cp + 2;
        }
        p = found + 2;
    }
    return NULL;
}

static void build_dead_zones(const char *content, size_t content_len,
                              dead_zones_t *dz)
{
    const char *p = content;
    const char *end = content + content_len;

    while (p < end) {
        const char *found = memchr(p, '{', (size_t)(end - p));
        if (!found || found + 1 >= end)
            break;

        /* {# comment #} */
        if (found[1] == '#') {
            const char *close = strstr(found + 2, "#}");
            if (close) {
                dz_add(dz, (size_t)(found - content), (size_t)(close + 2 - content));
                p = close + 2;
            } else {
                /* Unclosed comment: dead zone to end */
                dz_add(dz, (size_t)(found - content), content_len);
                break;
            }
            continue;
        }

        /* {% verbatim %} or {% raw %} */
        if (found[1] == '%') {
            const char *tp = found + 2;
            if (tp < end && (*tp == '-' || *tp == '~'))
                tp++;
            while (tp < end && (*tp == ' ' || *tp == '\t'))
                tp++;

            if (tp + 8 <= end && strncmp(tp, "verbatim", 8) == 0 &&
                (tp[8] == ' ' || tp[8] == '%' || tp[8] == '-' || tp[8] == '~')) {
                /* Find closing %} of this opening tag */
                const char *tag_end = strstr(tp, "%}");
                if (tag_end) {
                    const char *block_end = find_end_tag(tag_end + 2, end, "endverbatim");
                    if (block_end) {
                        dz_add(dz, (size_t)(found - content),
                               (size_t)(block_end - content));
                        p = block_end;
                        continue;
                    }
                }
            }

            if (tp + 3 <= end && strncmp(tp, "raw", 3) == 0 &&
                (tp[3] == ' ' || tp[3] == '%' || tp[3] == '-' || tp[3] == '~')) {
                const char *tag_end = strstr(tp, "%}");
                if (tag_end) {
                    const char *block_end = find_end_tag(tag_end + 2, end, "endraw");
                    if (block_end) {
                        dz_add(dz, (size_t)(found - content),
                               (size_t)(block_end - content));
                        p = block_end;
                        continue;
                    }
                }
            }
        }

        p = found + 1;
    }
}

/* ---- Path extraction helpers ---- */

/*
 * Extract a quoted string literal starting at *pp.
 * Returns strdup'd path or NULL. Advances *pp past the closing quote.
 */
static char *extract_quoted_path(const char **pp, const char *end)
{
    const char *p = *pp;
    if (p >= end) return NULL;
    char q = *p;
    if (q != '\'' && q != '"') return NULL;
    p++;
    const char *start = p;
    while (p < end && *p != q && *p != '\n')
        p++;
    if (p >= end || *p != q) return NULL;
    size_t len = (size_t)(p - start);
    *pp = p + 1;
    if (len == 0) return NULL;
    return tt_strndup(start, len);
}

/*
 * Check if string is _self (case sensitive).
 */
static int is_self(const char *s)
{
    return s && strcmp(s, "_self") == 0;
}

/* ---- Pass 2: Tag extraction ---- */

static void extract_tags(const char *content, size_t content_len,
                          const dead_zones_t *dz, const tt_line_offsets_t *lo,
                          const char *rel_path,
                          tt_symbol_t **syms, int *count, int *cap)
{
    const char *p = content;
    const char *end = content + content_len;

    while (p < end) {
        const char *found = memchr(p, '{', (size_t)(end - p));
        if (!found || found + 1 >= end)
            break;

        if (found[1] != '%') {
            p = found + 1;
            continue;
        }

        size_t tag_pos = (size_t)(found - content);
        if (dz_is_dead(dz, tag_pos)) {
            p = found + 2;
            continue;
        }

        /* Skip {%- or {%~ whitespace modifiers */
        const char *tp = found + 2;
        if (tp < end && (*tp == '-' || *tp == '~'))
            tp++;
        while (tp < end && (*tp == ' ' || *tp == '\t'))
            tp++;

        /* Read tag name (first identifier) */
        const char *name_start = tp;
        while (tp < end && (isalpha((unsigned char)*tp) || *tp == '_'))
            tp++;
        size_t name_len = (size_t)(tp - name_start);
        if (name_len == 0) {
            p = found + 2;
            continue;
        }

        /* Find closing %} */
        const char *close = strstr(tp, "%}");
        if (!close) {
            p = found + 2;
            continue;
        }
        const char *tag_body = tp;
        const char *tag_end = close;

        int line_num = tt_line_offsets_line_at(lo, (int)tag_pos);

        /* Skip whitespace after tag name */
        while (tag_body < tag_end && (*tag_body == ' ' || *tag_body == '\t'))
            tag_body++;

        /* Dispatch by tag name */
        if (name_len == 5 && strncmp(name_start, "block", 5) == 0) {
            /* {% block name %} */
            const char *bp = tag_body;
            while (bp < tag_end && (isalnum((unsigned char)*bp) || *bp == '_'))
                bp++;
            size_t blen = (size_t)(bp - tag_body);
            if (blen > 0) {
                char *bname = tt_strndup(tag_body, blen);
                char qn[300];
                snprintf(qn, sizeof(qn), "block.%s", bname);
                add_symbol(syms, count, cap, rel_path, bname, qn,
                           TT_KIND_DIRECTIVE, "directive", NULL,
                           line_num, lo, content, content_len);
                free(bname);
            }
        }
        else if (name_len == 7 && strncmp(name_start, "extends", 7) == 0) {
            /* {% extends 'path' %} or {% extends ['a', 'b'] %} */
            if (*tag_body == '[') {
                /* List syntax */
                const char *lp = tag_body + 1;
                while (lp < tag_end) {
                    while (lp < tag_end && *lp != '\'' && *lp != '"' && *lp != ']')
                        lp++;
                    if (lp >= tag_end || *lp == ']') break;
                    char *path = extract_quoted_path(&lp, tag_end);
                    if (path && !is_self(path)) {
                        char qn[512];
                        snprintf(qn, sizeof(qn), "extends.%s", path);
                        add_symbol(syms, count, cap, rel_path, path, qn,
                                   TT_KIND_DIRECTIVE, "directive", NULL,
                                   line_num, lo, content, content_len);
                    }
                    free(path);
                }
            } else {
                char *path = extract_quoted_path(&tag_body, tag_end);
                if (path && !is_self(path)) {
                    char qn[512];
                    snprintf(qn, sizeof(qn), "extends.%s", path);
                    add_symbol(syms, count, cap, rel_path, path, qn,
                               TT_KIND_DIRECTIVE, "directive", NULL,
                               line_num, lo, content, content_len);
                }
                free(path);
            }
        }
        else if (name_len == 7 && strncmp(name_start, "include", 7) == 0) {
            /* {% include 'path' %} or list */
            if (*tag_body == '[') {
                const char *lp = tag_body + 1;
                while (lp < tag_end) {
                    while (lp < tag_end && *lp != '\'' && *lp != '"' && *lp != ']')
                        lp++;
                    if (lp >= tag_end || *lp == ']') break;
                    char *path = extract_quoted_path(&lp, tag_end);
                    if (path && !is_self(path)) {
                        char qn[512];
                        snprintf(qn, sizeof(qn), "include.%s", path);
                        add_symbol(syms, count, cap, rel_path, path, qn,
                                   TT_KIND_DIRECTIVE, "directive", NULL,
                                   line_num, lo, content, content_len);
                    }
                    free(path);
                }
            } else {
                /* Skip dynamic: no quote = variable/function */
                char *path = extract_quoted_path(&tag_body, tag_end);
                if (path && !is_self(path)) {
                    char qn[512];
                    snprintf(qn, sizeof(qn), "include.%s", path);
                    add_symbol(syms, count, cap, rel_path, path, qn,
                               TT_KIND_DIRECTIVE, "directive", NULL,
                               line_num, lo, content, content_len);
                }
                free(path);
            }
        }
        else if (name_len == 5 && strncmp(name_start, "embed", 5) == 0) {
            /* {% embed 'path' %} */
            if (*tag_body == '[') {
                const char *lp = tag_body + 1;
                while (lp < tag_end) {
                    while (lp < tag_end && *lp != '\'' && *lp != '"' && *lp != ']')
                        lp++;
                    if (lp >= tag_end || *lp == ']') break;
                    char *path = extract_quoted_path(&lp, tag_end);
                    if (path && !is_self(path)) {
                        char qn[512];
                        snprintf(qn, sizeof(qn), "embed.%s", path);
                        add_symbol(syms, count, cap, rel_path, path, qn,
                                   TT_KIND_DIRECTIVE, "directive", NULL,
                                   line_num, lo, content, content_len);
                    }
                    free(path);
                }
            } else {
                char *path = extract_quoted_path(&tag_body, tag_end);
                if (path && !is_self(path)) {
                    char qn[512];
                    snprintf(qn, sizeof(qn), "embed.%s", path);
                    add_symbol(syms, count, cap, rel_path, path, qn,
                               TT_KIND_DIRECTIVE, "directive", NULL,
                               line_num, lo, content, content_len);
                }
                free(path);
            }
        }
        else if (name_len == 6 && strncmp(name_start, "import", 6) == 0) {
            /* {% import 'path' as alias %} — skip _self */
            /* Check for _self (unquoted) */
            if (tag_body < tag_end && *tag_body != '\'' && *tag_body != '"') {
                /* Dynamic or _self — skip */
            } else {
                char *path = extract_quoted_path(&tag_body, tag_end);
                if (path && !is_self(path)) {
                    char qn[512];
                    snprintf(qn, sizeof(qn), "import.%s", path);
                    add_symbol(syms, count, cap, rel_path, path, qn,
                               TT_KIND_DIRECTIVE, "directive", NULL,
                               line_num, lo, content, content_len);
                }
                free(path);
            }
        }
        else if (name_len == 4 && strncmp(name_start, "from", 4) == 0) {
            /* {% from 'path' import name1, name2 %} — skip _self */
            if (tag_body < tag_end && *tag_body != '\'' && *tag_body != '"') {
                /* Dynamic or _self — skip */
            } else {
                char *path = extract_quoted_path(&tag_body, tag_end);
                if (path && !is_self(path)) {
                    char qn[512];
                    snprintf(qn, sizeof(qn), "from.%s", path);
                    add_symbol(syms, count, cap, rel_path, path, qn,
                               TT_KIND_DIRECTIVE, "directive", NULL,
                               line_num, lo, content, content_len);
                }
                free(path);
            }
        }
        else if (name_len == 3 && strncmp(name_start, "use", 3) == 0) {
            /* {% use 'path' %} */
            char *path = extract_quoted_path(&tag_body, tag_end);
            if (path && !is_self(path)) {
                char qn[512];
                snprintf(qn, sizeof(qn), "use.%s", path);
                add_symbol(syms, count, cap, rel_path, path, qn,
                           TT_KIND_DIRECTIVE, "directive", NULL,
                           line_num, lo, content, content_len);
            }
            free(path);
        }
        else if (name_len == 5 && strncmp(name_start, "macro", 5) == 0) {
            /* {% macro name(args) %} */
            const char *mp = tag_body;
            const char *mname_start = mp;
            while (mp < tag_end && (isalnum((unsigned char)*mp) || *mp == '_'))
                mp++;
            size_t mname_len = (size_t)(mp - mname_start);
            if (mname_len > 0) {
                char *mname = tt_strndup(mname_start, mname_len);

                /* Build signature: "macro name(args)" */
                /* Find closing paren for signature */
                const char *paren = memchr(mp, '(', (size_t)(tag_end - mp));
                char sig[300];
                if (paren) {
                    const char *cparen = memchr(paren, ')', (size_t)(tag_end - paren));
                    if (cparen) {
                        size_t siglen = (size_t)(cparen + 1 - mname_start);
                        if (siglen > sizeof(sig) - 1) siglen = sizeof(sig) - 1;
                        memcpy(sig, mname_start, siglen);
                        sig[siglen] = '\0';
                    } else {
                        snprintf(sig, sizeof(sig), "%s(...)", mname);
                    }
                } else {
                    snprintf(sig, sizeof(sig), "%s", mname);
                }

                add_symbol(syms, count, cap, rel_path, mname, mname,
                           TT_KIND_METHOD, "method", sig,
                           line_num, lo, content, content_len);
                free(mname);
            }
        }
        else if (name_len == 3 && strncmp(name_start, "set", 3) == 0) {
            /* {% set name = value %} */
            const char *sp = tag_body;
            const char *sname_start = sp;
            while (sp < tag_end && (isalnum((unsigned char)*sp) || *sp == '_'))
                sp++;
            size_t sname_len = (size_t)(sp - sname_start);
            if (sname_len > 0) {
                char *sname = tt_strndup(sname_start, sname_len);
                add_symbol(syms, count, cap, rel_path, sname, sname,
                           TT_KIND_VARIABLE, "variable", NULL,
                           line_num, lo, content, content_len);
                free(sname);
            }
        }
        /* Skip end* tags, for, if, else, etc. */

        p = close + 2;
    }
}

/* ---- Pass 3: HTML attribute extraction ---- */

static void extract_html_attrs(const char *content, size_t content_len,
                                const dead_zones_t *dz, const tt_line_offsets_t *lo,
                                const char *rel_path,
                                tt_symbol_t **syms, int *count, int *cap)
{
    size_t pos = 0;

    while (pos < content_len) {
        /* Search for id=" or data-controller=" */
        const char *found = NULL;
        int is_data_controller = 0;

        for (size_t i = pos; i + 4 < content_len; i++) {
            /* data-controller="..." */
            if (i + 17 < content_len &&
                strncmp(content + i, "data-controller=", 16) == 0 &&
                (content[i + 16] == '"' || content[i + 16] == '\'')) {
                if (i > 0 && !isspace((unsigned char)content[i - 1]))
                    continue;
                if (dz_is_dead(dz, i))
                    continue;
                found = content + i;
                is_data_controller = 1;
                break;
            }

            /* id="..." */
            if (content[i] == 'i' && content[i + 1] == 'd' &&
                content[i + 2] == '=' &&
                (content[i + 3] == '"' || content[i + 3] == '\'')) {
                if (i > 0 && !isspace((unsigned char)content[i - 1]))
                    continue;
                if (dz_is_dead(dz, i))
                    continue;
                found = content + i;
                is_data_controller = 0;
                break;
            }
        }

        if (!found)
            break;

        if (is_data_controller) {
            char quote = found[16];
            const char *val_start = found + 17;
            const char *val_end = memchr(val_start, quote,
                                          content_len - (size_t)(val_start - content));
            if (!val_end) {
                pos = (size_t)(val_start - content);
                continue;
            }
            size_t vlen = (size_t)(val_end - val_start);
            if (vlen > 0 && vlen < 200) {
                /* Skip dynamic values containing {{ */
                if (!tt_memfind(val_start, vlen, "{{", 2)) {
                    char name[200];
                    memcpy(name, val_start, vlen);
                    name[vlen] = '\0';

                    char qn[220];
                    snprintf(qn, sizeof(qn), "stimulus.%s", name);

                    int line = tt_line_offsets_line_at(lo, (int)(found - content));
                    add_symbol(syms, count, cap, rel_path, name, qn,
                               TT_KIND_CONSTANT, "constant", NULL,
                               line, lo, content, content_len);
                }
            }
            pos = (size_t)(val_end - content) + 1;
        } else {
            char quote = found[3];
            const char *val_start = found + 4;
            const char *val_end = memchr(val_start, quote,
                                          content_len - (size_t)(val_start - content));
            if (!val_end) {
                pos = (size_t)(val_start - content);
                continue;
            }
            size_t vlen = (size_t)(val_end - val_start);
            if (vlen > 0 && vlen < 200) {
                /* Skip dynamic IDs: starting with @ or containing {{ */
                if (val_start[0] != '@' &&
                    !tt_memfind(val_start, vlen, "{{", 2)) {
                    char name[200];
                    memcpy(name, val_start, vlen);
                    name[vlen] = '\0';

                    int line = tt_line_offsets_line_at(lo, (int)(found - content));
                    add_symbol(syms, count, cap, rel_path, name, name,
                               TT_KIND_CONSTANT, "constant", NULL,
                               line, lo, content, content_len);
                }
            }
            pos = (size_t)(val_end - content) + 1;
        }
    }
}

/* ---- Entry point ---- */

int tt_parse_twig(const char *project_root, const char **file_paths, int file_count,
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

    for (int fi = 0; fi < file_count; fi++)
    {
        const char *rel_path = file_paths[fi];
        char *full_path = tt_path_join(project_root, rel_path);
        if (!full_path)
            continue;

        size_t content_len = 0;
        char *content = tt_read_file(full_path, &content_len);
        free(full_path);
        if (!content || content_len == 0)
        {
            free(content);
            continue;
        }

        tt_line_offsets_t lo;
        tt_line_offsets_build(&lo, content, content_len);

        /* Pass 1: Build dead zones */
        dead_zones_t dz;
        dz_init(&dz);
        build_dead_zones(content, content_len, &dz);

        /* Pass 2: Extract {% tags %} */
        extract_tags(content, content_len, &dz, &lo, rel_path,
                     &symbols, &sym_count, &sym_cap);

        /* Pass 3: Extract HTML attributes */
        extract_html_attrs(content, content_len, &dz, &lo, rel_path,
                           &symbols, &sym_count, &sym_cap);

        dz_free(&dz);
        free(content);
        tt_line_offsets_free(&lo);
    }

    *out = symbols;
    *out_count = sym_count;
    return 0;
}
