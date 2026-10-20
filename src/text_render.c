#include "text_render.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <cJSON.h>

#include "output_fmt.h"
#include "str_util.h"

/* ================================================================
 * Agent-line text output (--format text)
 *
 * Rendered from the JSON result returned by the _exec() functions, so
 * the exec path, the MCP layer and savings tracking stay unchanged.
 * Modes that have no grep equivalent (--count, --group-by) keep JSON.
 * ================================================================ */

bool tt_text_format_requested(const tt_cli_opts_t *opts)
{
    return opts && opts->format && strcmp(opts->format, "text") == 0;
}

/* Borrow a string field, trying the long key first then the short one. */
static const char *text_get_str(const cJSON *item, const char *key, const char *alt)
{
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(item, key));
    if (!v && alt)
        v = cJSON_GetStringValue(cJSON_GetObjectItem(item, alt));
    return v ? v : "";
}

/* Borrow a numeric field as int, 0 when absent. */
static int text_get_int(const cJSON *item, const char *key, int def)
{
    const cJSON *n = cJSON_GetObjectItem(item, key);
    if (!n || !cJSON_IsNumber(n))
        return def;
    return (int)n->valuedouble;
}

/*
 * search:text -> grep-style lines.
 *
 *   file:line:text          hit line
 *   file-line-text          context line ('-' like grep's separator)
 *   n=<count>               trailer
 */
char *tt_render_search_text_text(const cJSON *result, bool no_comments)
{
    if (!result)
        return NULL;

    const cJSON *items = cJSON_GetObjectItem(result, "results");
    if (!items || !cJSON_IsArray(items))
        return NULL;

    tt_strbuf_t sb;
    tt_strbuf_init(&sb);

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, items)
    {
        const char *f = text_get_str(item, "f", "file");
        int line = text_get_int(item, "l", 0);
        const char *t = text_get_str(item, "t", "text");
        bool in_block = false;

        const cJSON *before = cJSON_GetObjectItem(item, "before");
        if (before && cJSON_IsArray(before))
        {
            int bn = cJSON_GetArraySize(before);
            for (int i = 0; i < bn; i++)
            {
                const char *b = cJSON_GetStringValue(cJSON_GetArrayItem(before, i));
                if (!b)
                    b = "";
                bool drop = no_comments && tt_line_is_comment_or_blank(b, &in_block);
                if (!drop)
                    tt_strbuf_appendf(&sb, "%s-%d-%s\n", f, line - (bn - i), b);
            }
        }

        tt_strbuf_appendf(&sb, "%s:%d:%s\n", f, line, t);

        const cJSON *after = cJSON_GetObjectItem(item, "after");
        if (after && cJSON_IsArray(after))
        {
            int an = cJSON_GetArraySize(after);
            for (int i = 0; i < an; i++)
            {
                const char *a = cJSON_GetStringValue(cJSON_GetArrayItem(after, i));
                if (!a)
                    a = "";
                bool drop = no_comments && tt_line_is_comment_or_blank(a, &in_block);
                if (!drop)
                    tt_strbuf_appendf(&sb, "%s-%d-%s\n", f, line + i + 1, a);
            }
        }
    }

    tt_strbuf_appendf(&sb, "n=%d\n", text_get_int(result, "n", cJSON_GetArraySize(items)));
    return tt_strbuf_detach(&sb);
}

/*
 * True when the symbol id can be rebuilt from the printed fields.
 *
 * IDs are "{file}::{qualified_name}#{kind}", so "file::name#kind" is
 * derivable from the file, kind and name already on the line.
 */
static bool symbol_id_derivable(const char *id, const char *file,
                                const char *kind, const char *name)
{
    if (!id || !id[0])
        return true;

    tt_strbuf_t expected;
    tt_strbuf_init(&expected);
    tt_strbuf_appendf(&expected, "%s::%s#%s", file ? file : "",
                      name ? name : "", kind ? kind : "");

    bool derivable = expected.data && strcmp(expected.data, id) == 0;
    tt_strbuf_free(&expected);
    return derivable;
}

/*
 * search:symbols -> one agent line per symbol.
 *
 *   file:line:kind name       id omitted when derivable from the line
 *   file:line:kind name id=X  id printed when it carries extra information
 *   // not found: <name>      --defs names with no definition
 *   n=<count>                 trailer
 */
char *tt_render_search_symbols_text(const cJSON *result)
{
    if (!result)
        return NULL;

    const cJSON *items = cJSON_GetObjectItem(result, "results");
    if (!items || !cJSON_IsArray(items))
        return NULL;

    tt_strbuf_t sb;
    tt_strbuf_init(&sb);

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, items)
    {
        const char *f = text_get_str(item, "file", "f");
        const char *kind = text_get_str(item, "kind", "k");
        const char *name = text_get_str(item, "name", "n");
        const char *id = text_get_str(item, "id", NULL);
        int line = text_get_int(item, "line", text_get_int(item, "l", 0));

        if (symbol_id_derivable(id, f, kind, name))
            tt_strbuf_appendf(&sb, "%s:%d:%s %s\n", f, line, kind, name);
        else
            tt_strbuf_appendf(&sb, "%s:%d:%s %s id=%s\n", f, line, kind, name, id);
    }

    const cJSON *missing = cJSON_GetObjectItem(result, "not_found");
    if (missing && cJSON_IsArray(missing))
    {
        const cJSON *m = NULL;
        cJSON_ArrayForEach(m, missing)
            tt_strbuf_appendf(&sb, "// not found: %s\n",
                              cJSON_GetStringValue(m) ? cJSON_GetStringValue(m) : "");
    }

    tt_strbuf_appendf(&sb, "n=%d\n", text_get_int(result, "n", cJSON_GetArraySize(items)));
    return tt_strbuf_detach(&sb);
}

/* True when the line contains any pipe-separated alternative (case-insensitive). */
static bool line_matches_grep(const char *line, const char *patterns)
{
    if (!patterns || !patterns[0])
        return true;

    const char *p = patterns;
    while (*p)
    {
        const char *bar = strchr(p, '|');
        size_t len = bar ? (size_t)(bar - p) : strlen(p);

        while (len > 0 && isspace((unsigned char)p[0]))
        {
            p++;
            len--;
        }
        while (len > 0 && isspace((unsigned char)p[len - 1]))
            len--;

        if (len > 0)
        {
            char *pat = tt_strndup(p, len);
            bool hit = pat && tt_str_contains_ci(line, pat);
            free(pat);
            if (hit)
                return true;
        }

        if (!bar)
            break;
        p = bar + 1;
    }
    return false;
}

/* True when a scanned declaration is only an access specifier such as "public:". */
static bool decl_is_access_specifier(const char *decl)
{
    static const char *words[] = { "public", "private", "protected", "signals", "slots", NULL };
    size_t len = strlen(decl);
    if (len < 2 || decl[len - 1] != ':')
        return false;

    for (int i = 0; words[i]; i++)
    {
        size_t wl = strlen(words[i]);
        if (len - 1 == wl && strncmp(decl, words[i], wl) == 0)
            return true;
    }
    return false;
}

/* Append a trimmed declaration to the output array. Empty ones are dropped. */
static void push_member(tt_member_t **out, int *cap, int *n, tt_strbuf_t *cur, int cur_line)
{
    char *raw = tt_strbuf_detach(cur);
    if (!raw)
        return;

    char *start = raw;
    while (*start && isspace((unsigned char)*start))
        start++;
    size_t len = strlen(start);
    while (len > 0 && isspace((unsigned char)start[len - 1]))
        len--;

    if (len == 0 || (start[0] == '{' && len == 5))
    {
        free(raw);
        return;
    }

    if (decl_is_access_specifier(start))
    {
        free(raw);
        return;
    }

    char *decl = tt_strndup(start, len);
    free(raw);
    if (!decl)
        return;

    if (*n >= *cap)
    {
        int newcap = *cap ? *cap * 2 : 8;
        tt_member_t *grown = realloc(*out, (size_t)newcap * sizeof(tt_member_t));
        if (!grown)
        {
            free(decl);
            return;
        }
        *out = grown;
        *cap = newcap;
    }

    (*out)[*n].decl = decl;
    (*out)[*n].line = cur_line;
    (*n)++;
}

tt_member_t *tt_scan_members(const char *source, int start_line, bool comma_sep,
                             int *out_count)
{
    if (out_count)
        *out_count = 0;
    if (!source)
        return NULL;

    tt_member_t *out = NULL;
    int cap = 0;
    int n = 0;

    tt_strbuf_t cur;
    tt_strbuf_init(&cur);

    int line = start_line > 0 ? start_line : 1;
    int cur_line = line;
    int depth = 0;         /* (), [] nesting; 0 means top level of the body */
    bool in_body = false;
    bool started = false;  /* the current declaration has a non-space token */

    const char *p = source;
    while (*p)
    {
        char c = *p;

        if (c == '\n')
        {
            line++;
            if (started && cur.len > 0 && cur.data && cur.data[cur.len - 1] != ' ')
                tt_strbuf_append_char(&cur, ' ');
            p++;
            continue;
        }

        if (c == '/' && p[1] == '/')
        {
            while (*p && *p != '\n')
                p++;
            continue;
        }

        if (c == '/' && p[1] == '*')
        {
            p += 2;
            while (*p && !(*p == '*' && p[1] == '/'))
            {
                if (*p == '\n')
                    line++;
                p++;
            }
            if (*p)
                p += 2;
            continue;
        }

        /* preprocessor line, including backslash continuations */
        if (c == '#' && !started)
        {
            while (*p && *p != '\n')
            {
                if (*p == '\\' && p[1] == '\n')
                {
                    line++;
                    p += 2;
                    continue;
                }
                p++;
            }
            continue;
        }

        if (!in_body)
        {
            if (c == '{')
            {
                in_body = true;
                depth = 0;
            }
            p++;
            continue;
        }

        /* nested block: collapse the whole thing to "{...}" */
        if (c == '{')
        {
            int nest = 1;
            int block_line = line;
            p++;
            while (*p && nest > 0)
            {
                if (*p == '{')
                    nest++;
                else if (*p == '}')
                    nest--;
                else if (*p == '\n')
                    line++;
                if (nest > 0)
                    p++;
            }
            if (*p == '}')
                p++;
            if (!started)
            {
                cur_line = block_line;
                started = true;
            }
            tt_strbuf_append_str(&cur, "{...}");
            continue;
        }

        /* end of the type body */
        if (c == '}')
            break;

        /* string and character literals are copied verbatim */
        if (c == '"' || c == '\'')
        {
            char q = c;
            if (!started)
            {
                cur_line = line;
                started = true;
            }
            tt_strbuf_append_char(&cur, c);
            p++;
            while (*p && *p != q)
            {
                if (*p == '\\' && p[1])
                {
                    if (p[1] == '\n')
                        line++;
                    tt_strbuf_append_char(&cur, *p);
                    tt_strbuf_append_char(&cur, p[1]);
                    p += 2;
                    continue;
                }
                if (*p == '\n')
                {
                    line++;
                    p++;
                    continue;
                }
                tt_strbuf_append_char(&cur, *p);
                p++;
            }
            if (*p)
            {
                tt_strbuf_append_char(&cur, *p);
                p++;
            }
            continue;
        }

        if (c == '(' || c == '[' || c == ')' || c == ']')
        {
            if (!started)
            {
                cur_line = line;
                started = true;
            }
            if (c == '(' || c == '[')
                depth++;
            else if (depth > 0)
                depth--;
            tt_strbuf_append_char(&cur, c);
            p++;
            continue;
        }

        if (c == ';' || (comma_sep && c == ',' && depth == 0))
        {
            push_member(&out, &cap, &n, &cur, cur_line);
            started = false;
            p++;
            continue;
        }

        if (isspace((unsigned char)c))
        {
            if (started && cur.len > 0 && cur.data && cur.data[cur.len - 1] != ' ')
                tt_strbuf_append_char(&cur, ' ');
            p++;
            continue;
        }

        if (!started)
        {
            cur_line = line;
            started = true;
        }
        tt_strbuf_append_char(&cur, c);
        p++;
    }

    /* a trailing declaration without its terminator (enum's last value) */
    push_member(&out, &cap, &n, &cur, cur_line);
    tt_strbuf_free(&cur);

    if (out_count)
        *out_count = n;
    return out;
}

/* True when --members applies to a symbol of this kind. */
static bool kind_has_members(const char *kind)
{
    static const char *kinds[] = { "class", "struct", "union", "enum",
                                   "type", "interface", "trait", NULL };
    if (!kind)
        return false;

    for (int i = 0; kinds[i]; i++)
    {
        if (strcmp(kind, kinds[i]) == 0)
            return true;
    }
    return false;
}

/* Render a type symbol as one row per member: "LINE: declaration". */
static void append_members_as_text(tt_strbuf_t *sb, const char *file, int line,
                                   int end, const char *kind, const char *name,
                                   const char *source)
{
    int count = 0;
    bool comma_sep = kind && strcmp(kind, "enum") == 0;
    tt_member_t *members = tt_scan_members(source, line, comma_sep, &count);

    if (end > line)
        tt_strbuf_appendf(sb, "// %s:%d-%d %s %s", file, line, end, kind, name);
    else
        tt_strbuf_appendf(sb, "// %s:%d %s %s", file, line, kind, name);
    tt_strbuf_appendf(sb, " [member: %d]\n", count);

    for (int i = 0; i < count; i++)
        tt_strbuf_appendf(sb, "%d: %s\n", members[i].line, members[i].decl);

    for (int i = 0; i < count; i++)
        free(members[i].decl);
    free(members);
}

/*
 * Append one symbol as "// file:line-end kind name" followed by its source.
 *
 * With no_comments, blank and comment-only lines are dropped. With
 * grep_pat, only the body lines matching the (pipe-separated,
 * case-insensitive) pattern are printed, each with its real file line
 * number, and the header records the cut as "[grep: K of N lines]". With
 * members, a type symbol is reduced to one row per member.
 */
static void append_symbol_as_text(tt_strbuf_t *sb, const cJSON *item,
                                  bool no_comments, const char *grep_pat,
                                  bool members)
{
    const char *file = text_get_str(item, "file", NULL);
    const char *kind = text_get_str(item, "kind", NULL);
    const char *name = text_get_str(item, "name", NULL);
    int line = text_get_int(item, "line", text_get_int(item, "l", 0));
    int end = text_get_int(item, "end", text_get_int(item, "e", 0));
    const char *source = text_get_str(item, "source", NULL);
    if (!source[0])
        source = text_get_str(item, "src", NULL);

    bool filtering = no_comments || (grep_pat && grep_pat[0]);

    if (members && kind_has_members(kind) && source[0])
    {
        append_members_as_text(sb, file, line, end, kind, name, source);
        return;
    }

    int total = 0;
    char **lines = NULL;
    bool *show = NULL;
    int shown = 0;

    if (source[0])
    {
        lines = tt_str_split(source, '\n', &total);
        if (lines && total > 0 && lines[total - 1] && lines[total - 1][0] == '\0')
            total--; /* trailing newline, not a real line */

        if (lines && filtering)
        {
            show = calloc((size_t)(total > 0 ? total : 1), sizeof(bool));
            bool in_block = false;
            for (int i = 0; i < total; i++)
            {
                const char *ln = lines[i] ? lines[i] : "";
                bool keep = true;
                if (no_comments && tt_line_is_comment_or_blank(ln, &in_block))
                    keep = false;
                if (keep && grep_pat && grep_pat[0] && !line_matches_grep(ln, grep_pat))
                    keep = false;
                if (show)
                    show[i] = keep;
                if (keep)
                    shown++;
            }
        }
        else if (lines)
        {
            shown = total;
        }
    }

    if (end > line)
        tt_strbuf_appendf(sb, "// %s:%d-%d %s %s", file, line, end, kind, name);
    else
        tt_strbuf_appendf(sb, "// %s:%d %s %s", file, line, kind, name);

    if (grep_pat && grep_pat[0])
        tt_strbuf_appendf(sb, " [grep: %d of %d lines]", shown, total);

    tt_strbuf_append_char(sb, '\n');

    if (!lines)
    {
        free(show);
        return;
    }

    for (int i = 0; i < total; i++)
    {
        const char *ln = lines[i] ? lines[i] : "";
        if (filtering && show && !show[i])
            continue;
        if (grep_pat && grep_pat[0])
            tt_strbuf_appendf(sb, "%d: %s\n", line + i, ln);
        else
        {
            tt_strbuf_append_str(sb, ln);
            tt_strbuf_append_char(sb, '\n');
        }
    }

    free(show);
    tt_str_split_free(lines);
}

/*
 * inspect:symbol -> header line plus source. A batch result renders every
 * symbol in "symbols"; misses in "errors" print as "// not found: <id>".
 */
char *tt_render_inspect_symbol_text(const cJSON *result, bool no_comments,
                                    const char *grep_pat, bool members)
{
    if (!result || cJSON_GetObjectItem(result, "error"))
        return NULL;

    tt_strbuf_t sb;
    tt_strbuf_init(&sb);

    const cJSON *symbols = cJSON_GetObjectItem(result, "symbols");
    if (symbols && cJSON_IsArray(symbols))
    {
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, symbols)
            append_symbol_as_text(&sb, item, no_comments, grep_pat, members);
    }
    else
    {
        append_symbol_as_text(&sb, result, no_comments, grep_pat, members);
    }

    const cJSON *errs = cJSON_GetObjectItem(result, "errors");
    if (errs && cJSON_IsArray(errs))
    {
        const cJSON *e = NULL;
        cJSON_ArrayForEach(e, errs)
            tt_strbuf_appendf(&sb, "// not found: %s\n", text_get_str(e, "id", NULL));
    }

    return tt_strbuf_detach(&sb);
}

/*
 * inspect:file -> numbered lines "N: line".
 * Line numbers come from the returned range, so they stay correct even
 * when --no-comments drops lines.
 */
char *tt_render_inspect_file_text(const cJSON *result, bool no_comments)
{
    if (!result)
        return NULL;

    const char *content = text_get_str(result, "content", NULL);
    if (!content[0])
        return NULL;

    int start = 1;
    const cJSON *range = cJSON_GetObjectItem(result, "range");
    if (range && cJSON_IsArray(range) && cJSON_GetArraySize(range) >= 1)
    {
        const cJSON *first = cJSON_GetArrayItem(range, 0);
        if (first && cJSON_IsNumber(first))
            start = (int)first->valuedouble;
    }
    if (start < 1)
        start = 1;

    int line_count = 0;
    char **lines = tt_str_split(content, '\n', &line_count);
    if (!lines)
        return NULL;

    tt_strbuf_t sb;
    tt_strbuf_init(&sb);
    bool in_block = false;
    for (int i = 0; i < line_count; i++)
    {
        const char *ln = lines[i] ? lines[i] : "";
        if (no_comments && tt_line_is_comment_or_blank(ln, &in_block))
            continue;
        tt_strbuf_appendf(&sb, "%d: %s\n", start + i, ln);
    }

    tt_str_split_free(lines);
    return tt_strbuf_detach(&sb);
}
