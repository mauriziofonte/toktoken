/*
 * text_search.c -- Full-text search across indexed files.
 *
 * Dual strategy: ripgrep (fast) + manual fallback.
 */

#include "text_search.h"
#include "platform.h"
#include "str_util.h"
#include "error.h"
#include "hashmap.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdint.h>

#ifdef TT_PLATFORM_UNIX
#include <unistd.h>
#endif
#ifdef TT_HAVE_POSIX_REGEX
#include <regex.h>
#endif
#ifdef TT_PLATFORM_WINDOWS
#include <process.h>
#endif

/* Default options. */
static const tt_text_search_opts_t DEFAULT_OPTS = {
    .case_sensitive = false,
    .max_results = 100,
    .context_lines = 0};

#ifndef TT_HAVE_POSIX_REGEX
/* Cached ripgrep path (NULL = not checked, "" = not available). */
static char *rg_path_cache = NULL;
static bool rg_path_checked = false;

/* Check if ripgrep is available. Caches the result. */
static const char *find_ripgrep(void)
{
    if (!rg_path_checked)
    {
        rg_path_cache = tt_which("rg");
        rg_path_checked = true;
    }
    return rg_path_cache;
}

#endif

/* Free a single result's strings. */
static void result_free_fields(tt_text_result_t *r)
{
    free(r->file);
    free(r->text);
    for (int i = 0; i < r->before_count; i++)
        free(r->before[i]);
    free(r->before);
    for (int i = 0; i < r->after_count; i++)
        free(r->after[i]);
    free(r->after);
}

/* Truncate text to max 200 chars, append "..." if needed.
 * Returns a new string. [caller-frees] */
static char *truncate_text(const char *text)
{
    if (!text)
        return tt_strdup("");

    /* Trim leading/trailing whitespace */
    while (*text && (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n'))
        text++;

    size_t len = strlen(text);

    /* Trim trailing whitespace */
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t' ||
                       text[len - 1] == '\r' || text[len - 1] == '\n'))
        len--;

    /* Check if UTF-8 char count > 200 */
    size_t char_count = 0;
    size_t byte_pos = 0;
    size_t trunc_byte = 0;

    while (byte_pos < len)
    {
        if (char_count == 200)
        {
            trunc_byte = byte_pos;
        }
        char_count++;

        unsigned char c = (unsigned char)text[byte_pos];
        if (c < 0x80)
            byte_pos++;
        else if (c < 0xE0)
            byte_pos += 2;
        else if (c < 0xF0)
            byte_pos += 3;
        else
            byte_pos += 4;

        if (byte_pos > len)
            byte_pos = len;
    }

    if (char_count > 200)
    {
        /* Truncate at UTF-8 boundary */
        char *result = malloc(trunc_byte + 4);
        if (!result)
            return tt_strdup("");
        memcpy(result, text, trunc_byte);
        memcpy(result + trunc_byte, "...", 3);
        result[trunc_byte + 3] = '\0';
        return result;
    }

    return tt_strndup(text, len);
}

/* Grow results array. Returns 0 on success. */
static int grow_results(tt_text_results_t *out, int *cap)
{
    if (out->count < *cap)
        return 0;
    int new_cap = (*cap) * 2;
    if (new_cap < 32)
        new_cap = 32;
    tt_text_result_t *tmp = realloc(out->results,
                                    (size_t)new_cap * sizeof(tt_text_result_t));
    if (!tmp)
        return -1;
    out->results = tmp;
    *cap = new_cap;
    return 0;
}

/* ---- ReDoS protection ---- */

/* Max allowed regex length. */
#define REGEX_MAX_LEN 200

const char *tt_regex_validate(const char *pattern)
{
    if (!pattern || !pattern[0])
        return "Empty regex pattern";

    if (strlen(pattern) > REGEX_MAX_LEN)
        return "Regex too long (max 200 characters)";

    /* Detect nested quantifiers: a quantifier after a closing group that
     * itself contains a quantifier. Patterns like (a+)+, (a*)+, (a+)*,
     * (?:a+){2,} cause catastrophic backtracking. */
    int depth = 0;
    /* Stack of "group has quantifier" flags, max 32 nesting levels. */
    bool stack[32] = {false};

    for (const char *p = pattern; *p; p++)
    {
        if (*p == '\\' && p[1])
        {
            p++; /* skip escaped character */
            continue;
        }
        if (*p == '[')
        {
            /* Skip character class contents */
            p++;
            if (*p == '^') p++;
            if (*p == ']') p++;
            while (*p && *p != ']')
            {
                if (*p == '\\' && p[1]) p++;
                p++;
            }
            continue;
        }
        if (*p == '(')
        {
            if (depth < 31)
            {
                depth++;
                stack[depth] = false;
            }
            continue;
        }
        if (*p == ')')
        {
            bool inner_had_q = (depth > 0) ? stack[depth] : false;
            if (depth > 0) depth--;
            /* Check if a quantifier follows the closing paren */
            const char *next = p + 1;
            while (*next == ' ' || *next == '\t') next++;
            if (inner_had_q && (*next == '+' || *next == '*' || *next == '{'))
                return "Regex rejected: nested quantifiers can cause catastrophic backtracking";
            continue;
        }
        if (*p == '+' || *p == '*' || *p == '{')
        {
            if (depth > 0 && depth < 32)
                stack[depth] = true;
            /* Skip {n,m} body so digits/comma are not misinterpreted */
            if (*p == '{')
            {
                const char *q = p + 1;
                while (*q && *q != '}' && *q != '\0')
                    q++;
                if (*q == '}')
                    p = q;
            }
        }
    }

    return NULL;
}

#ifndef TT_HAVE_POSIX_REGEX
/* Escape a string for regex (like PHP preg_quote). [caller-frees] */
static char *regex_escape(const char *s)
{
    tt_strbuf_t sb;
    tt_strbuf_init(&sb);

    for (const char *p = s; *p; p++)
    {
        switch (*p)
        {
        case '\\':
        case '.':
        case '+':
        case '*':
        case '?':
        case '[':
        case ']':
        case '(':
        case ')':
        case '{':
        case '}':
        case '^':
        case '$':
        case '|':
            tt_strbuf_append_char(&sb, '\\');
            /* fallthrough */
        default:
            tt_strbuf_append_char(&sb, *p);
            break;
        }
    }

    return tt_strbuf_detach(&sb);
}

/* ---- Ripgrep search ---- */

#endif

#ifndef TT_HAVE_POSIX_REGEX
typedef struct
{
    int file_index;
    tt_text_result_t r;
} rg_hit_t;

static int cmp_rg_hit(const void *a, const void *b)
{
    const rg_hit_t *x = (const rg_hit_t *)a;
    const rg_hit_t *y = (const rg_hit_t *)b;
    if (x->file_index != y->file_index)
        return x->file_index < y->file_index ? -1 : 1;
    return (x->r.line > y->r.line) - (x->r.line < y->r.line);
}

/* Read a JSON string value starting right after the opening quote. */
static char *rg_json_string(const char *lt, const char *eol)
{
    tt_strbuf_t txt;
    tt_strbuf_init(&txt);
    while (*lt && *lt != '"' && lt < eol)
    {
        if (*lt == '\\' && *(lt + 1))
        {
            lt++;
            switch (*lt)
            {
            case 'n': tt_strbuf_append_char(&txt, '\n'); break;
            case 't': tt_strbuf_append_char(&txt, '\t'); break;
            case 'r': tt_strbuf_append_char(&txt, '\r'); break;
            default: tt_strbuf_append_char(&txt, *lt); break;
            }
        }
        else
        {
            tt_strbuf_append_char(&txt, *lt);
        }
        lt++;
    }
    return tt_strbuf_detach(&txt);
}

/*
 * search_ripgrep -- Regex search through ripgrep, used only where the
 * POSIX regex API is unavailable. Paths are passed as positional
 * arguments in chunks (command-line length limits); results are sorted
 * by input order and line for deterministic output. No context lines.
 */
static int search_ripgrep(const char *project_root, const char **file_paths, int file_count,
                          const char **queries, int query_count,
                          const tt_text_search_opts_t *opts,
                          tt_text_results_t *out, int *cap)
{
    tt_strbuf_t pattern;
    tt_strbuf_init(&pattern);
    for (int i = 0; i < query_count; i++)
    {
        if (i > 0)
            tt_strbuf_append_char(&pattern, '|');
        if (opts->is_regex)
            tt_strbuf_append_str(&pattern, queries[i]);
        else
        {
            char *escaped = regex_escape(queries[i]);
            if (escaped)
            {
                tt_strbuf_append_str(&pattern, escaped);
                free(escaped);
            }
        }
    }

    char max_count_str[32];
    snprintf(max_count_str, sizeof(max_count_str), "%d", opts->max_results);

    tt_hashmap_t *index_of = tt_hashmap_new((size_t)file_count * 2 + 16);
    char **full_paths = calloc((size_t)file_count, sizeof(char *));
    if (!index_of || !full_paths)
    {
        if (index_of)
            tt_hashmap_free(index_of);
        free(full_paths);
        tt_strbuf_free(&pattern);
        return -1;
    }
    for (int i = 0; i < file_count; i++)
    {
        full_paths[i] = tt_path_join(project_root, file_paths[i]);
        if (full_paths[i])
            tt_hashmap_set(index_of, full_paths[i], (void *)(intptr_t)(i + 1));
    }

    rg_hit_t *hits = NULL;
    int nhits = 0, caphits = 0;
    int rc = 0;
    const int CHUNK = 200;
    size_t root_len = strlen(project_root);

    for (int start = 0; start < file_count && rc == 0; start += CHUNK)
    {
        int end = start + CHUNK < file_count ? start + CHUNK : file_count;
        const char **argv = calloc((size_t)(end - start) + 12, sizeof(char *));
        if (!argv)
        {
            rc = -1;
            break;
        }
        int argc = 0;
        argv[argc++] = find_ripgrep();
        argv[argc++] = "--json";
        argv[argc++] = "--no-config";
        argv[argc++] = "--no-messages";
        argv[argc++] = "--max-count";
        argv[argc++] = max_count_str;
        if (!opts->case_sensitive)
            argv[argc++] = "--ignore-case";
        argv[argc++] = "-e";
        argv[argc++] = pattern.data;
        argv[argc++] = "--";
        for (int i = start; i < end; i++)
        {
            if (full_paths[i])
                argv[argc++] = full_paths[i];
        }
        argv[argc] = NULL;

        tt_proc_result_t result = tt_proc_run(argv, NULL, 30000);
        free(argv);
        if (result.exit_code < 0 || result.exit_code >= 2 || !result.stdout_buf)
        {
            tt_proc_result_free(&result);
            rc = -1;
            break;
        }

        const char *line = result.stdout_buf;
        while (*line)
        {
            const char *eol = strchr(line, '\n');
            if (!eol)
                eol = line + strlen(line);
            const char *type_pos = strstr(line, "\"type\":\"match\"");
            if (type_pos && type_pos < eol)
            {
                char *file = NULL;
                char *text = NULL;
                int line_num = 0;
                const char *path_key = strstr(line, "\"path\":");
                if (path_key && path_key < eol)
                {
                    const char *pt = strstr(path_key, "\"text\":\"");
                    if (pt && pt < eol)
                        file = rg_json_string(pt + 8, eol);
                }
                const char *ln_key = strstr(line, "\"line_number\":");
                if (ln_key && ln_key < eol)
                    line_num = atoi(ln_key + 14);
                const char *lines_key = strstr(line, "\"lines\":");
                if (lines_key && lines_key < eol)
                {
                    const char *lt = strstr(lines_key, "\"text\":\"");
                    if (lt && lt < eol)
                        text = rg_json_string(lt + 8, eol);
                }
                intptr_t idx = file ? (intptr_t)tt_hashmap_get(index_of, file) : 0;
                if (idx > 0 && line_num > 0 && text)
                {
                    if (nhits >= caphits)
                    {
                        caphits = caphits ? caphits * 2 : 64;
                        rg_hit_t *tmp = realloc(hits, (size_t)caphits * sizeof(rg_hit_t));
                        if (!tmp)
                        {
                            free(file);
                            free(text);
                            break;
                        }
                        hits = tmp;
                    }
                    rg_hit_t *h = &hits[nhits++];
                    memset(h, 0, sizeof(*h));
                    h->file_index = (int)idx - 1;
                    h->r.file = tt_strdup(file_paths[idx - 1]);
                    h->r.line = line_num;
                    h->r.text = truncate_text(text);
                }
                free(file);
                free(text);
            }
            if (!*eol)
                break;
            line = eol + 1;
        }
        tt_proc_result_free(&result);
    }
    (void)root_len;

    if (rc == 0 && nhits > 0)
    {
        qsort(hits, (size_t)nhits, sizeof(rg_hit_t), cmp_rg_hit);
        for (int i = 0; i < nhits; i++)
        {
            if (out->count < opts->max_results && grow_results(out, cap) == 0)
            {
                out->results[out->count++] = hits[i].r;
            }
            else
            {
                free(hits[i].r.file);
                free(hits[i].r.text);
            }
        }
    }
    else if (hits)
    {
        for (int i = 0; i < nhits; i++)
        {
            free(hits[i].r.file);
            free(hits[i].r.text);
        }
    }
    free(hits);
    for (int i = 0; i < file_count; i++)
        free(full_paths[i]);
    free(full_paths);
    tt_hashmap_free(index_of);
    tt_strbuf_free(&pattern);
    return rc;
}

#endif /* !TT_HAVE_POSIX_REGEX */

/* ---- Manual (PHP-style) fallback search ---- */

/* Check if line contains any of the queries. */
static bool line_matches(const char *line_text,
                         const char **queries, int query_count,
                         const char **queries_lower,
                         bool case_sensitive)
{
    for (int q = 0; q < query_count; q++)
    {
        if (case_sensitive)
        {
            if (strstr(line_text, queries[q]))
                return true;
        }
        else
        {
            if (tt_strcasestr(line_text, queries_lower[q]))
                return true;
        }
    }
    return false;
}

/* Append the match on line index li (0-based) with optional context. */
static int emit_line_result(tt_text_results_t *out, int *cap, const char *file,
                            char **lines, int total_lines, int li, int ctx)
{
    if (grow_results(out, cap) < 0)
        return -1;
    tt_text_result_t *r = &out->results[out->count];
    r->file = tt_strdup(file);
    r->line = li + 1;
    r->text = truncate_text(lines[li]);
    r->before = NULL;
    r->before_count = 0;
    r->after = NULL;
    r->after_count = 0;
    if (ctx > 0)
    {
        int before_start = li - ctx < 0 ? 0 : li - ctx;
        int before_cnt = li - before_start;
        if (before_cnt > 0)
        {
            r->before = malloc(sizeof(char *) * (size_t)before_cnt);
            if (r->before)
            {
                r->before_count = before_cnt;
                for (int b = 0; b < before_cnt; b++)
                    r->before[b] = tt_strdup(lines[before_start + b]);
            }
        }
        int after_end = li + 1 + ctx > total_lines ? total_lines : li + 1 + ctx;
        int after_cnt = after_end - (li + 1);
        if (after_cnt > 0)
        {
            r->after = malloc(sizeof(char *) * (size_t)after_cnt);
            if (r->after)
            {
                r->after_count = after_cnt;
                for (int a = 0; a < after_cnt; a++)
                    r->after[a] = tt_strdup(lines[li + 1 + a]);
            }
        }
    }
    out->count++;
    return 0;
}

#ifdef TT_HAVE_POSIX_REGEX
/*
 * search_regex_posix -- POSIX ERE search (grep -E semantics: REG_NEWLINE
 * makes ^ $ . line-oriented). Each file buffer is scanned with regexec;
 * only files with a match are split into lines. Deterministic: input file
 * order, then line order.
 */
static int search_regex_posix(const char *project_root, const char **file_paths, int file_count,
                              const char **queries, int query_count,
                              const tt_text_search_opts_t *opts,
                              tt_text_results_t *out, int *cap)
{
    tt_strbuf_t pattern;
    tt_strbuf_init(&pattern);
    for (int i = 0; i < query_count; i++)
    {
        if (i > 0)
            tt_strbuf_append_char(&pattern, '|');
        if (query_count > 1)
            tt_strbuf_append_char(&pattern, '(');
        tt_strbuf_append_str(&pattern, queries[i]);
        if (query_count > 1)
            tt_strbuf_append_char(&pattern, ')');
    }
    regex_t re;
    int cflags = REG_EXTENDED | REG_NEWLINE | (opts->case_sensitive ? 0 : REG_ICASE);
    int crc = regcomp(&re, pattern.data ? pattern.data : "", cflags);
    tt_strbuf_free(&pattern);
    if (crc != 0)
    {
        char msg[160];
        regerror(crc, &re, msg, sizeof(msg));
        tt_error_set("Invalid regex: %s", msg);
        return -2;
    }

    int *hit_lines = NULL;
    int hit_cap = 0;
    for (int fi = 0; fi < file_count && out->count < opts->max_results; fi++)
    {
        char *full_path = tt_path_join(project_root, file_paths[fi]);
        if (!full_path)
            continue;
        size_t len = 0;
        char *content = tt_read_file(full_path, &len);
        free(full_path);
        if (!content)
            continue;

        int nh = 0;
        size_t off = 0;
        int line_no = 1;
        size_t counted = 0;
        while (off <= len && out->count + nh < opts->max_results)
        {
            regmatch_t m;
            int ef = (off > 0 && content[off - 1] != '\n') ? REG_NOTBOL : 0;
            if (regexec(&re, content + off, 1, &m, ef) != 0)
                break;
            size_t ms = off + (size_t)m.rm_so;
            for (; counted < ms; counted++)
            {
                if (content[counted] == '\n')
                    line_no++;
            }
            if (nh >= hit_cap)
            {
                hit_cap = hit_cap ? hit_cap * 2 : 64;
                int *tmp = realloc(hit_lines, (size_t)hit_cap * sizeof(int));
                if (!tmp)
                    break;
                hit_lines = tmp;
            }
            hit_lines[nh++] = line_no;
            /* Next line: one result per line, like grep. */
            const char *nl = memchr(content + ms, '\n', len - ms);
            if (!nl)
                break;
            off = (size_t)(nl - content) + 1;
        }

        if (nh > 0)
        {
            int total_lines = 0;
            char **lines = tt_str_split(content, '\n', &total_lines);
            if (lines)
            {
                for (int k = 0; k < nh && out->count < opts->max_results; k++)
                {
                    int li = hit_lines[k] - 1;
                    if (li >= 0 && li < total_lines)
                        emit_line_result(out, cap, file_paths[fi], lines, total_lines, li,
                                         opts->context_lines);
                }
                tt_str_split_free(lines);
            }
        }
        free(content);
    }
    free(hit_lines);
    regfree(&re);
    return 0;
}
#endif

static int search_manual(const char *project_root, const char **file_paths, int file_count,
                         const char **queries, int query_count,
                         const tt_text_search_opts_t *opts,
                         tt_text_results_t *out, int *cap)
{
    /* Pre-lowercase queries for case-insensitive matching */
    char **queries_lower = NULL;
    if (!opts->case_sensitive)
    {
        queries_lower = malloc(sizeof(char *) * (size_t)query_count);
        if (!queries_lower)
            return -1;
        for (int i = 0; i < query_count; i++)
        {
            queries_lower[i] = tt_str_tolower(queries[i]);
        }
    }

    for (int fi = 0; fi < file_count && out->count < opts->max_results; fi++)
    {
        char *full_path = tt_path_join(project_root, file_paths[fi]);
        if (!full_path)
            continue;

        size_t content_len = 0;
        char *content = tt_read_file(full_path, &content_len);
        free(full_path);
        if (!content)
            continue;

        /* Split into lines */
        int total_lines = 0;
        char **lines = tt_str_split(content, '\n', &total_lines);
        if (!lines)
        {
            free(content);
            continue;
        }

        for (int li = 0; li < total_lines && out->count < opts->max_results; li++)
        {
            const char *line_text = lines[li];

            if (!line_matches(line_text, queries, query_count,
                              (const char **)queries_lower, opts->case_sensitive))
                continue;

            if (emit_line_result(out, cap, file_paths[fi], lines, total_lines, li,
                                 opts->context_lines) < 0)
                break;
        }

        tt_str_split_free(lines);
        free(content);
    }

    if (queries_lower)
    {
        for (int i = 0; i < query_count; i++)
            free(queries_lower[i]);
        free(queries_lower);
    }

    return 0;
}

/* ---- Public API ---- */

int tt_text_search(const char *project_root, const char **file_paths, int file_count,
                   const char **queries, int query_count,
                   const tt_text_search_opts_t *opts,
                   tt_text_results_t *out)
{
    if (!out)
        return -1;
    memset(out, 0, sizeof(*out));

    if (!project_root || !file_paths || file_count <= 0 ||
        !queries || query_count <= 0)
    {
        return 0;
    }

    if (!opts)
        opts = &DEFAULT_OPTS;

    /* ReDoS validation for regex mode */
    if (opts->is_regex)
    {
        for (int i = 0; i < query_count; i++)
        {
            const char *err = tt_regex_validate(queries[i]);
            if (err)
            {
                tt_error_set("%s", err);
                return -1;
            }
        }
    }

    int cap = 32;
    out->results = malloc(sizeof(tt_text_result_t) * (size_t)cap);
    if (!out->results)
        return -1;

    int rc;

    if (!opts->is_regex)
    {
        /* Literal: in-process, deterministic (input file order). */
        return search_manual(project_root, file_paths, file_count,
                             queries, query_count, opts, out, &cap);
    }

#ifdef TT_HAVE_POSIX_REGEX
    rc = search_regex_posix(project_root, file_paths, file_count,
                            queries, query_count, opts, out, &cap);
    return rc == -2 ? -1 : rc;
#else
    /* No POSIX regex API (e.g. MinGW): ripgrep is required for regex. */
    if (!find_ripgrep())
    {
        tt_error_set("Regex search needs ripgrep (rg) on this platform");
        return -1;
    }
    rc = search_ripgrep(project_root, file_paths, file_count,
                        queries, query_count, opts, out, &cap);
    if (rc < 0)
        tt_error_set("ripgrep failed to run the regex search");
    return rc;
#endif
}

void tt_text_results_free(tt_text_results_t *r)
{
    if (!r)
        return;
    for (int i = 0; i < r->count; i++)
    {
        result_free_fields(&r->results[i]);
    }
    free(r->results);
    r->results = NULL;
    r->count = 0;
}
