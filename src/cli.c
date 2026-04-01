/*
 * cli.c -- CLI argument parsing and project path resolution.
 *
 * Supports:
 *   --long-form flags
 *   -x short flags
 *   -abc aggregated boolean short flags
 *   -l VALUE / --limit VALUE (value flags)
 */

#include "cli.h"
#include "platform.h"
#include "str_util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- Flag descriptor table ---- */

typedef enum
{
    FLAG_BOOL,
    FLAG_VALUE_STR,
    FLAG_VALUE_INT,
    FLAG_VALUE_INT_CLAMP,      /* int with min value */
    FLAG_REPEATABLE,           /* repeatable --ignore pattern */
    FLAG_REPEATABLE_INCLUDE,   /* repeatable --include pattern */
} flag_type_t;

typedef struct
{
    char short_flag;      /* single char, or 0 if no short form */
    const char *long_flag; /* without "--" prefix */
    flag_type_t type;
    size_t offset;        /* offsetof into tt_cli_opts_t */
    int min_value;        /* for FLAG_VALUE_INT_CLAMP */
} flag_def_t;

#define BOOL_FLAG(s, l, field) \
    { s, l, FLAG_BOOL, offsetof(tt_cli_opts_t, field), 0 }
#define STR_FLAG(s, l, field) \
    { s, l, FLAG_VALUE_STR, offsetof(tt_cli_opts_t, field), 0 }
#define INT_FLAG(s, l, field) \
    { s, l, FLAG_VALUE_INT, offsetof(tt_cli_opts_t, field), 0 }
#define INT_CLAMP_FLAG(s, l, field, min) \
    { s, l, FLAG_VALUE_INT_CLAMP, offsetof(tt_cli_opts_t, field), min }
#define REPEAT_FLAG(s, l) \
    { s, l, FLAG_REPEATABLE, 0, 0 }
#define REPEAT_INCLUDE_FLAG(s, l) \
    { s, l, FLAG_REPEATABLE_INCLUDE, 0, 0 }

static const flag_def_t flag_table[] = {
    /* Boolean flags */
    BOOL_FLAG('v', "version",        version),
    BOOL_FLAG('h', "help",           help),
    BOOL_FLAG('c', "compact",        compact),
    BOOL_FLAG('u', "unique",         unique),
    BOOL_FLAG( 0,  "no-sig",         no_sig),
    BOOL_FLAG( 0,  "no-summary",     no_summary),
    BOOL_FLAG('n', "count",          count),
    BOOL_FLAG('r', "regex",          regex),
    BOOL_FLAG('s', "case-sensitive", case_sensitive),
    BOOL_FLAG( 0,  "verify",         verify),
    BOOL_FLAG( 0,  "has-importers",  has_importers),
    BOOL_FLAG( 0,  "include-callers", include_callers),
    STR_FLAG ( 0,  "detail",          detail),
    INT_FLAG ( 0,  "token-budget",    token_budget),
    BOOL_FLAG( 0,  "check",          check),
    BOOL_FLAG( 0,  "exclude-tests",  exclude_tests),
    BOOL_FLAG( 0,  "cross-dir",      cross_dir),
    INT_FLAG ( 0,  "min-length",     min_length),
    BOOL_FLAG('D', "debug",          debug),
    BOOL_FLAG('X', "diagnostic",     diagnostic),
    BOOL_FLAG('a', "all",            all),
    BOOL_FLAG( 0,  "confirm",        confirm),
    BOOL_FLAG('F', "force",          force),
    BOOL_FLAG('f', "full",           full),
    BOOL_FLAG( 0,  "full-clone",     full_clone),
    BOOL_FLAG( 0,  "update-only",    update_only),

    /* Value flags (string) */
    STR_FLAG('p', "path",       path),
    STR_FLAG('o', "format",     format),
    STR_FLAG( 0,  "filter",     filter),
    STR_FLAG('e', "exclude",    exclude),
    STR_FLAG( 0,  "search",     search),
    STR_FLAG('k', "kind",       kind),
    STR_FLAG('L', "language",   language),
    STR_FLAG( 0,  "file",       file_glob),
    STR_FLAG('S', "sort",       sort),
    STR_FLAG('g', "group-by",   group_by),
    STR_FLAG( 0,  "lines",      lines),
    STR_FLAG( 0,  "languages",  languages),
    STR_FLAG( 0,  "scope-imports-of",   scope_imports_of),
    STR_FLAG( 0,  "scope-importers-of", scope_importers_of),
    STR_FLAG('b', "branch",     branch),

    /* Value flags (int) */
    INT_FLAG('l', "limit",      limit),
    INT_FLAG('C', "context",    context),
    INT_FLAG('d', "depth",      depth),
    INT_FLAG('m', "max-files",  max_files),
    INT_FLAG( 0,  "max",        max),
    INT_CLAMP_FLAG('t', "truncate", truncate_width, 20),

    /* Repeatable */
    REPEAT_FLAG('i', "ignore"),
    REPEAT_INCLUDE_FLAG('I', "include"),

    /* sentinel */
    { 0, NULL, FLAG_BOOL, 0, 0 }
};

/* ---- Helpers ---- */

/* Check if arg looks like a flag (starts with "-" and is not just "-"). */
static bool is_flag(const char *arg)
{
    return arg && arg[0] == '-' && arg[1] != '\0';
}

/* Consume the next argv element as the flag's value.
 * Returns the value or NULL if missing. */
static const char *consume_value(int *i, int argc, char *argv[])
{
    if (*i + 1 < argc && !is_flag(argv[*i + 1]))
    {
        (*i)++;
        return argv[*i];
    }
    /* Allow values that look like negative numbers */
    if (*i + 1 < argc && argv[*i + 1][0] == '-' &&
        argv[*i + 1][1] >= '0' && argv[*i + 1][1] <= '9')
    {
        (*i)++;
        return argv[*i];
    }
    return NULL;
}

/* Find flag definition by long name (without "--" prefix). */
static const flag_def_t *find_long(const char *name)
{
    for (int j = 0; flag_table[j].long_flag; j++)
    {
        if (strcmp(flag_table[j].long_flag, name) == 0)
            return &flag_table[j];
    }
    return NULL;
}

/* Find flag definition by short character. */
static const flag_def_t *find_short(char ch)
{
    for (int j = 0; flag_table[j].long_flag; j++)
    {
        if (flag_table[j].short_flag == ch)
            return &flag_table[j];
    }
    return NULL;
}

/* Apply a boolean flag to opts. */
static void apply_bool(tt_cli_opts_t *opts, const flag_def_t *def)
{
    *(bool *)((char *)opts + def->offset) = true;
}

/* Append a value to a repeatable array (ignore or include). */
static void append_repeatable(const char ***arr, int *arr_count, int *arr_cap,
                              const char *value)
{
    if (*arr_count >= *arr_cap)
    {
        int new_cap = *arr_cap ? *arr_cap * 2 : 8;
        const char **tmp = realloc(*arr, (size_t)new_cap * sizeof(const char *));
        if (!tmp) return;
        *arr = tmp;
        *arr_cap = new_cap;
    }
    (*arr)[(*arr_count)++] = value;
}

/* Apply a value flag (string or int) to opts. Returns 0 on success, -1 on error. */
static int apply_value(tt_cli_opts_t *opts, const flag_def_t *def,
                       int *i, int argc, char *argv[],
                       const char ***ign, int *ign_cap,
                       const char ***incl, int *incl_cap)
{
    if (def->type == FLAG_REPEATABLE)
    {
        const char *v = consume_value(i, argc, argv);
        if (v)
            append_repeatable(ign, &opts->ignore_count, ign_cap, v);
        return 0;
    }

    if (def->type == FLAG_REPEATABLE_INCLUDE)
    {
        const char *v = consume_value(i, argc, argv);
        if (v)
            append_repeatable(incl, &opts->include_count, incl_cap, v);
        return 0;
    }

    const char *v = consume_value(i, argc, argv);
    if (!v)
        return 0; /* missing value is silently ignored */

    if (def->type == FLAG_VALUE_STR)
    {
        *(const char **)((char *)opts + def->offset) = v;
    }
    else if (def->type == FLAG_VALUE_INT)
    {
        *(int *)((char *)opts + def->offset) = atoi(v);
    }
    else if (def->type == FLAG_VALUE_INT_CLAMP)
    {
        int val = atoi(v);
        if (val < def->min_value)
            val = def->min_value;
        *(int *)((char *)opts + def->offset) = val;
    }

    return 0;
}

/* ---- Public API ---- */

int tt_cli_parse(tt_cli_opts_t *opts, int argc, char *argv[])
{
    memset(opts, 0, sizeof(*opts));
    opts->truncate_width = 120;

    /* Temporary storage for positional args, ignore and include patterns */
    const char **pos = NULL;
    int pos_cap = 0;
    const char **ign = NULL;
    int ign_cap = 0;
    const char **incl = NULL;
    int incl_cap = 0;

    /* Skip argv[0] (program) and argv[1] (command) */
    int start = 2;
    if (argc < 2)
        start = argc;

    for (int i = start; i < argc; i++)
    {
        const char *arg = argv[i];

        /* --- Long flags: --something --- */
        if (arg[0] == '-' && arg[1] == '-')
        {
            const char *name = arg + 2;
            const flag_def_t *def = find_long(name);
            if (!def)
            {
                fprintf(stderr, "Unknown option: %s\n", arg);
                free(pos);
                free(ign);
                free(incl);
                return -1;
            }

            if (def->type == FLAG_BOOL)
                apply_bool(opts, def);
            else
                apply_value(opts, def, &i, argc, argv, &ign, &ign_cap, &incl, &incl_cap);

            continue;
        }

        /* --- Short flags: -x or aggregated -abc --- */
        if (arg[0] == '-' && arg[1] != '\0')
        {
            const char *p = arg + 1;

            while (*p)
            {
                const flag_def_t *def = find_short(*p);
                if (!def)
                {
                    fprintf(stderr, "Unknown option: -%c\n", *p);
                    free(pos);
                    free(ign);
                    free(incl);
                    return -1;
                }

                if (def->type == FLAG_BOOL)
                {
                    apply_bool(opts, def);
                    p++;
                }
                else
                {
                    /* Value flag: rest of string is the value, or next argv */
                    p++;
                    if (*p)
                    {
                        /* -l10 or -mVALUE: rest of string is the value */
                        if (def->type == FLAG_VALUE_STR)
                        {
                            *(const char **)((char *)opts + def->offset) = p;
                        }
                        else if (def->type == FLAG_VALUE_INT)
                        {
                            *(int *)((char *)opts + def->offset) = atoi(p);
                        }
                        else if (def->type == FLAG_VALUE_INT_CLAMP)
                        {
                            int val = atoi(p);
                            if (val < def->min_value)
                                val = def->min_value;
                            *(int *)((char *)opts + def->offset) = val;
                        }
                        else if (def->type == FLAG_REPEATABLE)
                        {
                            append_repeatable(&ign, &opts->ignore_count, &ign_cap, p);
                        }
                        else if (def->type == FLAG_REPEATABLE_INCLUDE)
                        {
                            append_repeatable(&incl, &opts->include_count, &incl_cap, p);
                        }
                    }
                    else
                    {
                        /* -l VALUE: consume next argv */
                        apply_value(opts, def, &i, argc, argv, &ign, &ign_cap, &incl, &incl_cap);
                    }
                    break; /* value flag ends aggregation */
                }
            }
            continue;
        }

        /* --- Positional argument --- */
        if (opts->positional_count >= pos_cap)
        {
            int new_cap = pos_cap ? pos_cap * 2 : 8;
            const char **tmp = realloc(pos, (size_t)new_cap * sizeof(const char *));
            if (tmp) { pos = tmp; pos_cap = new_cap; }
        }
        pos[opts->positional_count++] = arg;
    }

    opts->positional = pos;
    opts->ignore_patterns = ign;
    opts->include_patterns = incl;
    return 0;
}

void tt_cli_opts_free(tt_cli_opts_t *opts)
{
    free((void *)opts->positional);
    free((void *)opts->ignore_patterns);
    free((void *)opts->include_patterns);
    memset(opts, 0, sizeof(*opts));
}

char *tt_resolve_project_path(const char *path)
{
    if (!path || !path[0])
        path = ".";

    /* "." -> getcwd */
    const char *effective = path;
    char *cwd = NULL;
    if (strcmp(path, ".") == 0)
    {
        cwd = tt_getcwd();
        if (cwd)
            effective = cwd;
    }

    /* Try realpath */
    char *resolved = tt_realpath(effective);
    if (resolved)
    {
        free(cwd);
        return resolved;
    }

    /* Fallback: use original/cwd path */
    if (cwd)
        return cwd;
    return tt_strdup(path);
}
