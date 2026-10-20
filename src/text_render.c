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

/*
 * Append one symbol as "// file:line-end kind name" followed by its source.
 *
 * With no_comments, blank and comment-only lines are dropped. With
 * grep_pat, only the body lines matching the (pipe-separated,
 * case-insensitive) pattern are printed, each with its real file line
 * number, and the header records the cut as "[grep: K of N lines]".
 */
static void append_symbol_as_text(tt_strbuf_t *sb, const cJSON *item,
                                  bool no_comments, const char *grep_pat)
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
                                    const char *grep_pat)
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
            append_symbol_as_text(&sb, item, no_comments, grep_pat);
    }
    else
    {
        append_symbol_as_text(&sb, result, no_comments, grep_pat);
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
