/*
 * parser_razor.c -- Parser for ASP.NET Razor views (.cshtml).
 *
 * Extracts:
 *   1. C# methods from @functions { ... } and @code { ... } blocks
 *   2. HTML element IDs (id="..." attributes) as constants
 *   3. @model, @using, @inject directives
 */

#include "parser_razor.h"
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

/* Add a symbol to the array. Returns 0 on success. */
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

    /* Keywords: just the name lowercased */
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
    sym->language = tt_strdup("razor");
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

/*
 * Extract C# method signatures from a code block.
 * Looks for patterns like: <access> <return_type> <Name>(...) {
 * Simplified: find lines matching "identifier identifier(" pattern.
 */
static void extract_methods(const char *block_start, size_t block_len,
                            int base_line, const char *rel_path,
                            const tt_line_offsets_t *lo,
                            const char *full_content, size_t full_len,
                            tt_symbol_t **syms, int *count, int *cap)
{
    const char *p = block_start;
    const char *end = block_start + block_len;
    int line_offset = 0;

    while (p < end)
    {
        /* Find start of next line */
        const char *line_start = p;

        /* Find end of line */
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol)
            eol = end;

        /* Skip leading whitespace */
        const char *lp = line_start;
        while (lp < eol && (*lp == ' ' || *lp == '\t'))
            lp++;

        /* Look for C# method pattern:
         * [public|private|protected|internal|static|async|override|virtual]*
         * <return_type> <MethodName>( */
        if (lp < eol && (isalpha((unsigned char)*lp) || *lp == '['))
        {
            /* Skip modifiers and return type, look for identifier followed by '(' */
            const char *scan = lp;
            const char *last_word_start = NULL;
            const char *last_word_end = NULL;
            int word_count = 0;

            while (scan < eol)
            {
                /* Skip whitespace */
                while (scan < eol && (*scan == ' ' || *scan == '\t'))
                    scan++;
                if (scan >= eol)
                    break;

                if (*scan == '(')
                {
                    /* Found '(' — last_word is the method name.
                     * A real method definition needs at least 2 words
                     * before '(' (return_type + name), e.g. "void MyMethod(".
                     * A single word like "Console.WriteLine(" is a call. */
                    if (last_word_start && last_word_end && word_count >= 2)
                    {
                        size_t name_len = (size_t)(last_word_end - last_word_start);
                        /* Skip if it looks like a keyword (if, for, while, etc.) */
                        if (name_len > 0 && name_len < 64 &&
                            isupper((unsigned char)*last_word_start))
                        {
                            char name[64];
                            memcpy(name, last_word_start, name_len);
                            name[name_len] = '\0';

                            /* Build signature from trimmed line */
                            size_t sig_len = (size_t)(eol - lp);
                            if (sig_len > 200) sig_len = 200;
                            char sig[201];
                            memcpy(sig, lp, sig_len);
                            sig[sig_len] = '\0';
                            /* Trim trailing whitespace */
                            while (sig_len > 0 && (sig[sig_len - 1] == '\r' ||
                                                    sig[sig_len - 1] == '\n' ||
                                                    sig[sig_len - 1] == ' '))
                                sig[--sig_len] = '\0';

                            add_symbol(syms, count, cap, rel_path, name, name,
                                       TT_KIND_METHOD, "method", sig,
                                       base_line + line_offset, lo,
                                       full_content, full_len);
                        }
                    }
                    break;
                }

                /* Read an identifier */
                if (isalpha((unsigned char)*scan) || *scan == '_')
                {
                    last_word_start = scan;
                    while (scan < eol && (isalnum((unsigned char)*scan) ||
                                          *scan == '_' || *scan == '<' ||
                                          *scan == '>' || *scan == '.' ||
                                          *scan == '[' || *scan == ']'))
                        scan++;
                    last_word_end = scan;
                    word_count++;
                }
                else
                {
                    scan++;
                }
            }
        }

        p = (eol < end) ? eol + 1 : end;
        line_offset++;
    }
}

int tt_parse_razor(const char *project_root, const char **file_paths, int file_count,
                   tt_symbol_t **out, int *out_count)
{
    if (!out || !out_count)
        return -1;
    *out = NULL;
    *out_count = 0;

    if (!project_root || !file_paths || file_count <= 0)
        return 0;

    int sym_cap = 16;
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

        size_t pos = 0;
        while (pos < content_len)
        {
            const char *at = memchr(content + pos, '@', content_len - pos);
            if (!at)
                break;
            size_t at_pos = (size_t)(at - content);
            pos = at_pos + 1;

            /* @functions { ... } or @code { ... } */
            int is_functions = (at_pos + 10 <= content_len &&
                                strncmp(at + 1, "functions", 9) == 0 &&
                                !isalnum((unsigned char)at[10]));
            int is_code = (at_pos + 5 <= content_len &&
                           strncmp(at + 1, "code", 4) == 0 &&
                           !isalnum((unsigned char)at[5]));

            if (is_functions || is_code)
            {
                /* Find the opening '{' */
                const char *p = at + 1 + (is_functions ? 9 : 4);
                while (p < content + content_len && *p != '{')
                    p++;
                if (p >= content + content_len)
                    continue;
                p++; /* skip '{' */

                /* Find matching '}' (brace counting) */
                int depth = 1;
                const char *block_start = p;
                while (p < content + content_len && depth > 0)
                {
                    if (*p == '{')
                        depth++;
                    else if (*p == '}')
                        depth--;
                    if (depth > 0)
                        p++;
                }

                size_t block_len = (size_t)(p - block_start);
                int base_line = tt_line_offsets_line_at(&lo, (int)(block_start - content));

                extract_methods(block_start, block_len, base_line, rel_path,
                                &lo, content, content_len,
                                &symbols, &sym_count, &sym_cap);
                pos = (size_t)(p - content) + 1;
                continue;
            }

            /* @model TypeName */
            if (at_pos + 7 <= content_len &&
                strncmp(at + 1, "model", 5) == 0 &&
                (at[6] == ' ' || at[6] == '\t'))
            {
                const char *p = at + 7;
                while (p < content + content_len && (*p == ' ' || *p == '\t'))
                    p++;
                const char *name_start = p;
                while (p < content + content_len && !isspace((unsigned char)*p) &&
                       *p != ';' && *p != '\n' && *p != '\r')
                    p++;
                size_t nlen = (size_t)(p - name_start);
                if (nlen > 0 && nlen < 200)
                {
                    char name[200];
                    memcpy(name, name_start, nlen);
                    name[nlen] = '\0';

                    char qn[220];
                    snprintf(qn, sizeof(qn), "model.%s", name);

                    int line = tt_line_offsets_line_at(&lo, (int)at_pos);
                    add_symbol(&symbols, &sym_count, &sym_cap, rel_path,
                               name, qn, TT_KIND_DIRECTIVE, "directive",
                               NULL, line, &lo, content, content_len);
                }
                continue;
            }

            /* @using Namespace */
            if (at_pos + 7 <= content_len &&
                strncmp(at + 1, "using", 5) == 0 &&
                (at[6] == ' ' || at[6] == '\t'))
            {
                const char *p = at + 7;
                while (p < content + content_len && (*p == ' ' || *p == '\t'))
                    p++;
                const char *name_start = p;
                while (p < content + content_len && !isspace((unsigned char)*p) &&
                       *p != ';' && *p != '\n' && *p != '\r')
                    p++;
                size_t nlen = (size_t)(p - name_start);
                if (nlen > 0 && nlen < 200)
                {
                    char name[200];
                    memcpy(name, name_start, nlen);
                    name[nlen] = '\0';

                    char qn[220];
                    snprintf(qn, sizeof(qn), "using.%s", name);

                    int line = tt_line_offsets_line_at(&lo, (int)at_pos);
                    add_symbol(&symbols, &sym_count, &sym_cap, rel_path,
                               name, qn, TT_KIND_DIRECTIVE, "directive",
                               NULL, line, &lo, content, content_len);
                }
                continue;
            }

            /* @inject TypeName varName */
            if (at_pos + 8 <= content_len &&
                strncmp(at + 1, "inject", 6) == 0 &&
                (at[7] == ' ' || at[7] == '\t'))
            {
                const char *p = at + 8;
                while (p < content + content_len && (*p == ' ' || *p == '\t'))
                    p++;
                /* Read type + variable as the name */
                const char *name_start = p;
                while (p < content + content_len && *p != '\n' && *p != '\r')
                    p++;
                size_t nlen = (size_t)(p - name_start);
                /* Trim trailing whitespace */
                while (nlen > 0 && (name_start[nlen - 1] == ' ' ||
                                     name_start[nlen - 1] == '\t'))
                    nlen--;
                if (nlen > 0 && nlen < 200)
                {
                    char name[200];
                    memcpy(name, name_start, nlen);
                    name[nlen] = '\0';

                    char qn[220];
                    snprintf(qn, sizeof(qn), "inject.%s", name);

                    int line = tt_line_offsets_line_at(&lo, (int)at_pos);
                    add_symbol(&symbols, &sym_count, &sym_cap, rel_path,
                               name, qn, TT_KIND_DIRECTIVE, "directive",
                               NULL, line, &lo, content, content_len);
                }
                continue;
            }
        }

        /* Extract HTML id="..." attributes */
        pos = 0;
        while (pos < content_len)
        {
            /* Find id=" or id=' */
            const char *found = NULL;
            for (size_t i = pos; i + 4 < content_len; i++)
            {
                if (content[i] == 'i' && content[i + 1] == 'd' &&
                    content[i + 2] == '=' &&
                    (content[i + 3] == '"' || content[i + 3] == '\''))
                {
                    /* Make sure 'id' is preceded by whitespace (attribute context) */
                    if (i > 0 && !isspace((unsigned char)content[i - 1]))
                        continue;
                    found = content + i;
                    break;
                }
            }
            if (!found)
                break;

            char quote = found[3];
            const char *val_start = found + 4;
            const char *val_end = memchr(val_start, quote,
                                          content_len - (size_t)(val_start - content));
            if (!val_end)
            {
                pos = (size_t)(val_start - content);
                continue;
            }

            size_t vlen = (size_t)(val_end - val_start);
            if (vlen > 0 && vlen < 200 && val_start[0] != '@')
            {
                char name[200];
                memcpy(name, val_start, vlen);
                name[vlen] = '\0';

                int line = tt_line_offsets_line_at(&lo,
                                                   (int)(found - content));
                add_symbol(&symbols, &sym_count, &sym_cap, rel_path,
                           name, name, TT_KIND_CONSTANT, "constant",
                           NULL, line, &lo, content, content_len);
            }
            pos = (size_t)(val_end - content) + 1;
        }

        free(content);
        tt_line_offsets_free(&lo);
    }

    *out = symbols;
    *out_count = sym_count;
    return 0;
}
