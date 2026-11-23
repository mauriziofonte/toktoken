/*
 * output_fmt.c -- Output formatting: filter/exclude/limit/table/JSONL.
 */

#include "output_fmt.h"
#include "str_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ---- Comment / blank line filter (--no-comments) ---- */

/* Return the first non-space character index, or the end of the string. */
static const char *skip_space(const char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    return s;
}

/* True when everything from s onward is whitespace. */
static bool rest_is_blank(const char *s)
{
    return *skip_space(s) == '\0';
}

bool tt_line_is_comment_or_blank(const char *line, bool *in_block)
{
    if (!line)
        return true;

    const char *s = skip_space(line);

    /* Inside a block comment: everything is comment until it is closed */
    if (in_block && *in_block)
    {
        const char *close = strstr(s, "*/");
        if (!close)
            return true;
        *in_block = false;
        return rest_is_blank(close + 2);
    }

    if (*s == '\0')
        return true; /* blank line */

    if (s[0] == '/' && s[1] == '/')
        return true; /* // line comment */

    if (s[0] == '/' && s[1] == '*')
    {
        const char *close = strstr(s + 2, "*/");
        if (!close)
        {
            if (in_block)
                *in_block = true;
            return true; /* comment starts and continues on the next line */
        }
        return rest_is_blank(close + 2); /* comment opens and closes here */
    }

    return false;
}

/* ---- Filter / Exclude ---- */

/* Check if file_path contains any pipe-delimited pattern (case-insensitive).
 * Zero-allocation: scans the delimiter string inline using a stack buffer. */
static bool matches_any_pattern(const char *file_path, const char *patterns)
{
    const char *p = patterns;
    while (*p)
    {
        /* Skip leading whitespace */
        while (*p == ' ' || *p == '\t') p++;
        const char *start = p;
        while (*p && *p != '|') p++;

        /* Trim trailing whitespace */
        const char *end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;

        size_t len = (size_t)(end - start);
        if (len > 0)
        {
            char buf[512];
            size_t copy_len = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
            memcpy(buf, start, copy_len);
            buf[copy_len] = '\0';
            if (tt_str_contains_ci(file_path, buf))
                return true;
        }
        if (*p == '|') p++;
    }
    return false;
}

bool tt_matches_path_filters(const char *file_path,
                             const char *filter,
                             const char *exclude)
{
    if (!file_path)
        return false;

    /* --filter: at least one pattern must match (case-insensitive substring) */
    if (filter && filter[0])
    {
        if (!matches_any_pattern(file_path, filter))
            return false;
    }

    /* --exclude: if any pattern matches, exclude */
    if (exclude && exclude[0])
    {
        if (matches_any_pattern(file_path, exclude))
            return false;
    }

    return true;
}

/* ---- Limit ---- */

void tt_apply_limit(int limit, int *count)
{
    if (!count)
        return;
    if (limit > 0 && *count > limit)
        *count = limit;
}

/* ---- JSONL / NDJSON ---- */

int tt_output_jsonl(cJSON *array)
{
    if (!array || !cJSON_IsArray(array))
        return 0;

    cJSON *item;
    cJSON_ArrayForEach(item, array)
    {
        char *str = cJSON_PrintUnformatted(item);
        if (str)
        {
            fputs(str, stdout);
            fputc('\n', stdout);
            free(str);
        }
    }
    fflush(stdout);
    return 0;
}

/* ---- Table rendering ---- */

static size_t display_len(const char *s)
{
    return s ? tt_utf8_strlen(s) : 0;
}

static void print_padded(const char *s, int width, int truncate)
{
    if (!s)
        s = "";

    size_t slen = display_len(s);

    if (truncate > 0 && (int)slen > truncate)
    {
        /* Truncate: first (truncate-3) chars + "..." */
        int keep = truncate - 3;
        if (keep < 0)
            keep = 0;

        /* We need byte-level truncation at UTF-8 boundary */
        char *copy = tt_strdup(s);
        if (copy)
        {
            tt_utf8_truncate(copy, (size_t)keep);
            size_t kept = display_len(copy);
            fprintf(stdout, " %s...", copy);
            /* Pad remaining */
            int pad = width - (int)kept - 3;
            for (int i = 0; i < pad; i++)
                fputc(' ', stdout);
            fputc(' ', stdout);
            free(copy);
        }
    }
    else
    {
        fprintf(stdout, " %s", s);
        int pad = width - (int)slen;
        for (int i = 0; i < pad; i++)
            fputc(' ', stdout);
        fputc(' ', stdout);
    }
}

void tt_render_table(const char **columns, const int *min_widths, int col_count,
                     const char ***rows, int row_count, int truncate)
{
    if (row_count <= 0 || !rows)
    {
        fprintf(stdout, "(no results)\n");
        fflush(stdout);
        return;
    }

    /* Calculate column widths: max(minWidth, headerLen, maxCellLen) */
    int *widths = calloc((size_t)col_count, sizeof(int));
    if (!widths)
        return;

    for (int c = 0; c < col_count; c++)
    {
        widths[c] = min_widths ? min_widths[c] : 0;
        int hlen = (int)display_len(columns[c]);
        if (hlen > widths[c])
            widths[c] = hlen;
    }

    for (int r = 0; r < row_count; r++)
    {
        for (int c = 0; c < col_count; c++)
        {
            int clen = (int)display_len(rows[r][c]);
            if (clen > widths[c])
                widths[c] = clen;
        }
    }

    /* Cap widths to truncate */
    if (truncate > 0)
    {
        for (int c = 0; c < col_count; c++)
        {
            if (widths[c] > truncate)
                widths[c] = truncate;
        }
    }

    /* Print header (UPPERCASE) */
    for (int c = 0; c < col_count; c++)
    {
        char *upper = tt_strdup(columns[c]);
        if (upper)
        {
            for (char *p = upper; *p; p++)
                *p = (char)toupper((unsigned char)*p);
            print_padded(upper, widths[c], 0);
            free(upper);
        }
    }
    fputc('\n', stdout);

    /* Print separator */
    for (int c = 0; c < col_count; c++)
    {
        fputc(' ', stdout);
        for (int i = 0; i < widths[c]; i++)
            fputc('-', stdout);
        fputc(' ', stdout);
    }
    fputc('\n', stdout);

    /* Print rows */
    for (int r = 0; r < row_count; r++)
    {
        for (int c = 0; c < col_count; c++)
        {
            print_padded(rows[r][c], widths[c], truncate);
        }
        fputc('\n', stdout);
    }

    fflush(stdout);
    free(widths);
}
