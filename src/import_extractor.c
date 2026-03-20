/*
 * import_extractor.c -- Extract import/require/include statements from source files.
 *
 * Per-language line-by-line extraction. No regex needed; uses strstr/strncmp.
 *
 * Supported languages:
 *   JS/TS/Vue: import ... from '...',  require('...'),  import('...')
 *   Python:  import ...,  from ... import ...
 *   Go:      import "...",  import (...)
 *   Rust:    use ...;,  mod ...;
 *   C/C++:   #include "...",  #include <...>
 *   Java/Kotlin/Scala: import ...;
 *   PHP:     use ...;,  require..., include...
 *   Ruby:    require '...',  require_relative '...'
 *   C#:      using ...;
 *   Swift/Haskell: import ...
 */

#include "import_extractor.h"
#include "platform.h"
#include "str_util.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ---- Helpers ---- */

static void add_import(tt_import_t **arr, int *count, int *cap,
                       const char *from_file, const char *to_specifier,
                       const char *symbol_name, int line,
                       const char *import_type)
{
    if (!to_specifier || !*to_specifier) return;

    if (*count >= *cap) {
        *cap = (*cap == 0) ? 16 : *cap * 2;
        tt_import_t *tmp = realloc(*arr, (size_t)*cap * sizeof(tt_import_t));
        if (!tmp) return;
        *arr = tmp;
    }

    tt_import_t *imp = &(*arr)[*count];
    imp->from_file = tt_strdup(from_file);
    imp->to_specifier = tt_strdup(to_specifier);
    imp->to_file = NULL;
    imp->symbol_name = symbol_name ? tt_strdup(symbol_name) : NULL;
    imp->line = line;
    imp->import_type = tt_strdup(import_type);
    (*count)++;
}

/* Skip whitespace, return pointer to first non-ws char */
static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* Read a quoted string (single or double quotes). Returns strdup'd content.
 * Advances *pp past the closing quote. Returns NULL on failure. */
static char *read_quoted(const char **pp)
{
    const char *p = *pp;
    char q = *p;
    if (q != '\'' && q != '"' && q != '`') return NULL;
    p++;
    const char *start = p;
    while (*p && *p != q && *p != '\n') p++;
    if (*p != q) return NULL;
    char *result = tt_strndup(start, (size_t)(p - start));
    *pp = p + 1;
    return result;
}

/* Read an unquoted word (identifiers, dotted paths). */
static size_t read_word(const char *p)
{
    size_t n = 0;
    while (p[n] && (isalnum((unsigned char)p[n]) || p[n] == '_' || p[n] == '.' || p[n] == '/')) n++;
    return n;
}

/* Read until a delimiter character or end of line */
static size_t read_until(const char *p, char delim)
{
    size_t n = 0;
    while (p[n] && p[n] != delim && p[n] != '\n') n++;
    return n;
}

/* ---- Per-language extractors ---- */

/* JS/TS: import ... from '...', import '...', require('...') */
static void extract_js(const char *from_file, const char *line, int line_num,
                       tt_import_t **arr, int *count, int *cap)
{
    const char *p = skip_ws(line);

    /* import ... from '...' or import '...' */
    if (strncmp(p, "import ", 7) == 0) {
        /* Find "from" keyword */
        const char *fr = strstr(p, " from ");
        if (fr) {
            fr = skip_ws(fr + 6);
            char *spec = read_quoted(&fr);
            if (spec) {
                /* Try to extract symbol name from "import { X } from ..." */
                const char *brace = strchr(p + 7, '{');
                char *sym = NULL;
                if (brace) {
                    brace = skip_ws(brace + 1);
                    size_t slen = read_word(brace);
                    if (slen > 0) sym = tt_strndup(brace, slen);
                }
                add_import(arr, count, cap, from_file, spec, sym, line_num, "import");
                free(sym);
                free(spec);
            }
        } else {
            /* Side-effect import: import '...' */
            const char *q = skip_ws(p + 7);
            char *spec = read_quoted(&q);
            if (spec) {
                add_import(arr, count, cap, from_file, spec, NULL, line_num, "import");
                free(spec);
            }
        }
        return;
    }

    /* require('...') or require("...") */
    const char *req = strstr(p, "require(");
    if (req) {
        const char *q = skip_ws(req + 8);
        char *spec = read_quoted(&q);
        if (spec) {
            add_import(arr, count, cap, from_file, spec, NULL, line_num, "require");
            free(spec);
        }
    }

    /* Dynamic import: import('...'), import("..."), import(`...`) */
    {
        const char *dyn = p;
        while ((dyn = strstr(dyn, "import(")) != NULL) {
            /* Skip if preceded by alphanumeric/underscore (e.g. "reimport(") */
            if (dyn > p && (isalnum((unsigned char)dyn[-1]) || dyn[-1] == '_')) {
                dyn += 7;
                continue;
            }
            const char *q = skip_ws(dyn + 7);
            char *spec = read_quoted(&q);
            if (spec) {
                add_import(arr, count, cap, from_file, spec, NULL, line_num,
                           "dynamic_import");
                free(spec);
            }
            dyn += 7;
        }
    }
}

/* Python: import X, from X import Y */
static void extract_python(const char *from_file, const char *line, int line_num,
                           tt_import_t **arr, int *count, int *cap)
{
    const char *p = skip_ws(line);

    if (strncmp(p, "from ", 5) == 0) {
        const char *np = skip_ws(p + 5);
        size_t mlen = read_word(np);
        if (mlen > 0) {
            char *module = tt_strndup(np, mlen);
            /* Find "import" keyword */
            const char *imp = strstr(np + mlen, " import ");
            char *sym = NULL;
            if (imp) {
                imp = skip_ws(imp + 8);
                size_t slen = read_word(imp);
                if (slen > 0) sym = tt_strndup(imp, slen);
            }
            add_import(arr, count, cap, from_file, module, sym, line_num, "from");
            free(sym);
            free(module);
        }
        return;
    }

    if (strncmp(p, "import ", 7) == 0) {
        const char *np = skip_ws(p + 7);
        size_t mlen = read_word(np);
        if (mlen > 0) {
            char *module = tt_strndup(np, mlen);
            add_import(arr, count, cap, from_file, module, NULL, line_num, "import");
            free(module);
        }
    }
}

/* Go: import "..." or import (...) block -- simplified single-line */
static void extract_go(const char *from_file, const char *line, int line_num,
                       tt_import_t **arr, int *count, int *cap)
{
    const char *p = skip_ws(line);

    /* Direct "import "..." */
    if (strncmp(p, "import ", 7) == 0) {
        const char *q = skip_ws(p + 7);
        /* Skip optional alias */
        if (*q != '"' && *q != '(') {
            while (*q && *q != '"' && *q != ' ' && *q != '\t') q++;
            q = skip_ws(q);
        }
        if (*q == '"') {
            char *spec = read_quoted(&q);
            if (spec) {
                add_import(arr, count, cap, from_file, spec, NULL, line_num, "import");
                free(spec);
            }
        }
        return;
    }

    /* Inside import block: line like "  \"fmt\"" */
    if (*p == '"') {
        char *spec = read_quoted(&p);
        if (spec) {
            add_import(arr, count, cap, from_file, spec, NULL, line_num, "import");
            free(spec);
        }
    }
}

/* Rust: use ...; mod ...; */
static void extract_rust(const char *from_file, const char *line, int line_num,
                         tt_import_t **arr, int *count, int *cap)
{
    const char *p = skip_ws(line);

    if (strncmp(p, "use ", 4) == 0) {
        const char *np = skip_ws(p + 4);
        size_t mlen = read_until(np, ';');
        if (mlen > 0) {
            /* Trim trailing whitespace */
            while (mlen > 0 && (np[mlen - 1] == ' ' || np[mlen - 1] == '\t')) mlen--;
            char *spec = tt_strndup(np, mlen);
            /* Try to extract last segment as symbol name */
            const char *last_colon = strrchr(spec, ':');
            char *sym = NULL;
            if (last_colon && last_colon[1] == ':') {
                const char *s = last_colon + 2;
                if (*s && *s != '{' && *s != '*')
                    sym = tt_strdup(s);
            }
            add_import(arr, count, cap, from_file, spec, sym, line_num, "use");
            free(sym);
            free(spec);
        }
        return;
    }

    if (strncmp(p, "mod ", 4) == 0) {
        const char *np = skip_ws(p + 4);
        size_t mlen = read_word(np);
        if (mlen > 0) {
            char *spec = tt_strndup(np, mlen);
            add_import(arr, count, cap, from_file, spec, NULL, line_num, "mod");
            free(spec);
        }
    }
}

/* C/C++: #include "..." or #include <...> */
static void extract_c(const char *from_file, const char *line, int line_num,
                      tt_import_t **arr, int *count, int *cap)
{
    const char *p = skip_ws(line);
    if (*p != '#') return;
    p = skip_ws(p + 1);

    if (strncmp(p, "include", 7) != 0) return;
    p = skip_ws(p + 7);

    if (*p == '"') {
        char *spec = read_quoted(&p);
        if (spec) {
            add_import(arr, count, cap, from_file, spec, NULL, line_num, "include");
            free(spec);
        }
    } else if (*p == '<') {
        p++;
        const char *start = p;
        while (*p && *p != '>' && *p != '\n') p++;
        if (*p == '>' && p > start) {
            char *spec = tt_strndup(start, (size_t)(p - start));
            add_import(arr, count, cap, from_file, spec, NULL, line_num, "include");
            free(spec);
        }
    }
}

/* Java/Kotlin/Scala: import ...; */
static void extract_java(const char *from_file, const char *line, int line_num,
                         tt_import_t **arr, int *count, int *cap)
{
    const char *p = skip_ws(line);
    if (strncmp(p, "import ", 7) != 0) return;
    p = skip_ws(p + 7);

    /* Skip "static" keyword in Java */
    if (strncmp(p, "static ", 7) == 0)
        p = skip_ws(p + 7);

    size_t mlen = read_until(p, ';');
    if (mlen > 0) {
        while (mlen > 0 && (p[mlen - 1] == ' ' || p[mlen - 1] == '\t')) mlen--;
        char *spec = tt_strndup(p, mlen);
        /* Last dotted segment is the symbol */
        const char *dot = strrchr(spec, '.');
        char *sym = NULL;
        if (dot && dot[1] && dot[1] != '*')
            sym = tt_strdup(dot + 1);
        add_import(arr, count, cap, from_file, spec, sym, line_num, "import");
        free(sym);
        free(spec);
    }
}

/* PHP: use ...; require...; include... */
static void extract_php(const char *from_file, const char *line, int line_num,
                        tt_import_t **arr, int *count, int *cap)
{
    const char *p = skip_ws(line);

    if (strncmp(p, "use ", 4) == 0) {
        p = skip_ws(p + 4);
        size_t mlen = read_until(p, ';');
        if (mlen > 0) {
            while (mlen > 0 && (p[mlen - 1] == ' ' || p[mlen - 1] == '\t')) mlen--;
            char *spec = tt_strndup(p, mlen);
            /* Last backslash segment is the symbol */
            const char *bs = strrchr(spec, '\\');
            char *sym = (bs && bs[1]) ? tt_strdup(bs + 1) : NULL;
            add_import(arr, count, cap, from_file, spec, sym, line_num, "use");
            free(sym);
            free(spec);
        }
        return;
    }

    /* require, require_once, include, include_once */
    const char *kw = NULL;
    const char *kw_type = NULL;
    if (strncmp(p, "require_once", 12) == 0) { kw = p + 12; kw_type = "require"; }
    else if (strncmp(p, "require", 7) == 0) { kw = p + 7; kw_type = "require"; }
    else if (strncmp(p, "include_once", 12) == 0) { kw = p + 12; kw_type = "include"; }
    else if (strncmp(p, "include", 7) == 0 && (p[7] == ' ' || p[7] == '(')) { kw = p + 7; kw_type = "include"; }

    if (kw && kw_type) {
        kw = skip_ws(kw);
        if (*kw == '(') kw = skip_ws(kw + 1);
        char *spec = read_quoted(&kw);
        if (spec) {
            add_import(arr, count, cap, from_file, spec, NULL, line_num, kw_type);
            free(spec);
        }
    }
}

/* Ruby: require '...', require_relative '...' */
static void extract_ruby(const char *from_file, const char *line, int line_num,
                         tt_import_t **arr, int *count, int *cap)
{
    const char *p = skip_ws(line);

    if (strncmp(p, "require_relative ", 17) == 0) {
        const char *q = skip_ws(p + 17);
        char *spec = read_quoted(&q);
        if (spec) {
            add_import(arr, count, cap, from_file, spec, NULL, line_num, "require");
            free(spec);
        }
        return;
    }

    if (strncmp(p, "require ", 8) == 0) {
        const char *q = skip_ws(p + 8);
        char *spec = read_quoted(&q);
        if (spec) {
            add_import(arr, count, cap, from_file, spec, NULL, line_num, "require");
            free(spec);
        }
    }
}

/* C#: using ...; */
static void extract_csharp(const char *from_file, const char *line, int line_num,
                           tt_import_t **arr, int *count, int *cap)
{
    const char *p = skip_ws(line);
    if (strncmp(p, "using ", 6) != 0) return;
    p = skip_ws(p + 6);

    /* Skip "static" */
    if (strncmp(p, "static ", 7) == 0)
        p = skip_ws(p + 7);

    /* Skip alias assignments like "using Alias = ..." */
    const char *eq = strchr(p, '=');
    const char *sc = strchr(p, ';');
    if (eq && sc && eq < sc) return;

    size_t mlen = read_until(p, ';');
    if (mlen > 0) {
        while (mlen > 0 && (p[mlen - 1] == ' ' || p[mlen - 1] == '\t')) mlen--;
        char *spec = tt_strndup(p, mlen);
        add_import(arr, count, cap, from_file, spec, NULL, line_num, "using");
        free(spec);
    }
}

/* Swift/Haskell: import ... */
static void extract_import_simple(const char *from_file, const char *line, int line_num,
                                  tt_import_t **arr, int *count, int *cap)
{
    const char *p = skip_ws(line);
    if (strncmp(p, "import ", 7) != 0) return;
    p = skip_ws(p + 7);

    /* Skip optional "qualified" (Haskell) */
    if (strncmp(p, "qualified ", 10) == 0)
        p = skip_ws(p + 10);

    size_t mlen = read_word(p);
    if (mlen > 0) {
        char *spec = tt_strndup(p, mlen);
        add_import(arr, count, cap, from_file, spec, NULL, line_num, "import");
        free(spec);
    }
}

/* ---- Language dispatch ---- */

typedef void (*lang_extractor_t)(const char *from_file, const char *line,
                                 int line_num, tt_import_t **arr,
                                 int *count, int *cap);

static lang_extractor_t get_extractor(const char *language)
{
    if (!language) return NULL;

    if (strcmp(language, "javascript") == 0 ||
        strcmp(language, "typescript") == 0 ||
        strcmp(language, "tsx") == 0 ||
        strcmp(language, "jsx") == 0 ||
        strcmp(language, "vue") == 0)
        return extract_js;

    if (strcmp(language, "python") == 0) return extract_python;
    if (strcmp(language, "go") == 0) return extract_go;
    if (strcmp(language, "rust") == 0) return extract_rust;

    if (strcmp(language, "c") == 0 ||
        strcmp(language, "c++") == 0 ||
        strcmp(language, "cpp") == 0 ||
        strcmp(language, "objectivec") == 0 ||
        strcmp(language, "objc") == 0 ||
        strcmp(language, "objcpp") == 0)
        return extract_c;

    if (strcmp(language, "java") == 0 ||
        strcmp(language, "kotlin") == 0 ||
        strcmp(language, "scala") == 0)
        return extract_java;

    if (strcmp(language, "php") == 0 ||
        strcmp(language, "blade") == 0)
        return extract_php;

    if (strcmp(language, "ruby") == 0) return extract_ruby;
    if (strcmp(language, "c#") == 0 ||
        strcmp(language, "csharp") == 0)
        return extract_csharp;

    if (strcmp(language, "swift") == 0 ||
        strcmp(language, "haskell") == 0)
        return extract_import_simple;

    return NULL;
}

/* ---- Public API ---- */

int tt_extract_imports_from_lines(const char *file_path, const char *language,
                                  const char **lines, int nlines,
                                  tt_import_t **out, int *out_count, int *cap)
{
    if (!out || !out_count || !cap) return -1;
    if (!file_path || !language || !lines) return 0;

    lang_extractor_t extractor = get_extractor(language);
    if (!extractor) return 0;

    for (int i = 0; i < nlines; i++)
    {
        extractor(file_path, lines[i], i + 1, out, out_count, cap);
    }

    return 0;
}

int tt_extract_imports(const char *project_root, const char *file_path,
                       const char *language, tt_import_t **out, int *out_count)
{
    if (!out || !out_count) return -1;
    *out = NULL;
    *out_count = 0;

    if (!project_root || !file_path || !language) return 0;

    lang_extractor_t extractor = get_extractor(language);
    if (!extractor) return 0;

    char *full = tt_path_join(project_root, file_path);
    if (!full) return -1;

    size_t clen = 0;
    char *content = tt_read_file(full, &clen);
    free(full);
    if (!content) return 0;

    int nlines = 0;
    char **lines = tt_str_split(content, '\n', &nlines);

    int cap = 0;
    for (int i = 0; i < nlines; i++) {
        extractor(file_path, lines[i], i + 1, out, out_count, &cap);
    }

    tt_str_split_free(lines);
    free(content);
    return 0;
}

void tt_import_free(tt_import_t *imp)
{
    if (!imp) return;
    free(imp->from_file);
    free(imp->to_specifier);
    free(imp->to_file);
    free(imp->symbol_name);
    free(imp->import_type);
    imp->from_file = NULL;
    imp->to_specifier = NULL;
    imp->to_file = NULL;
    imp->symbol_name = NULL;
    imp->import_type = NULL;
}

void tt_import_array_free(tt_import_t *imps, int count)
{
    if (!imps) return;
    for (int i = 0; i < count; i++)
        tt_import_free(&imps[i]);
    free(imps);
}
