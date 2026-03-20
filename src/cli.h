/*
 * cli.h -- CLI argument parsing and project path resolution.
 *
 * Generic parser for all TokToken commands. Uses manual argv scanning
 * for maximum portability (no getopt dependency).
 */

#ifndef TT_CLI_H
#define TT_CLI_H

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    /* Positional arguments (after command name) */
    const char **positional;
    int positional_count;

    /* Global flags */
    const char *path;    /* --path or first positional (default ".") */
    const char *format;  /* --format (default "json") */
    const char *filter;  /* --filter (pipe-separated, case-insensitive) */
    const char *exclude; /* --exclude (pipe-separated, case-insensitive) */
    int limit;           /* --limit (default 0 = no limit) */
    int truncate_width;  /* --truncate (default 120, min 20) */
    bool compact;        /* --compact */
    bool version;        /* --version */
    bool help;           /* --help */

    /* Command-specific flags */
    const char *search;    /* --search (alias for query positional) */
    const char *kind;      /* --kind (comma-separated) */
    const char *language;  /* --language */
    const char *file_glob; /* --file (glob pattern) */
    int max;               /* --max (default depends on command) */
    bool unique;           /* --unique */
    bool no_sig;           /* --no-sig */
    bool no_summary;       /* --no-summary */
    bool count;            /* --count */
    bool regex;            /* --regex */
    const char *sort;      /* --sort (default "score") */
    bool case_sensitive;   /* --case-sensitive */
    const char *group_by;  /* --group-by */
    int context;           /* --context / -C */
    bool verify;           /* --verify */
    const char *lines;     /* --lines "START-END" */
    int depth;             /* --depth */
    int max_files;         /* --max-files */
    bool all;              /* --all */
    bool confirm;          /* --confirm */
    bool force;            /* --force (re-clone for index:github) */
    bool full;             /* --full (disable smart filter, index everything) */
    bool full_clone;       /* --full-clone (disable shallow clone) */
    bool update_only;      /* --update-only (pull without re-index) */
    const char *branch;    /* --branch (for index:github) */

    /* --ignore (repeatable) */
    const char **ignore_patterns;
    int ignore_count;

    /* --languages (comma-separated, index:create only) */
    const char *languages;

    /* --has-importers (find:importers enrichment) */
    bool has_importers;

    /* --include-callers (inspect:bundle enrichment) */
    bool include_callers;

    /* --detail compact|standard|full (search:symbols) */
    const char *detail;

    /* --token-budget N (search:symbols) */
    int token_budget;

    /* --exclude-tests (find:dead) */
    bool exclude_tests;

    /* --cross-dir (inspect:cycles — show only cross-directory cycles) */
    bool cross_dir;

    /* --min-length N (inspect:cycles — minimum cycle length) */
    int min_length;

    /* --scope-imports-of / --scope-importers-of (search:symbols) */
    const char *scope_imports_of;
    const char *scope_importers_of;

    /* --debug (search:symbols score breakdown) */
    bool debug;

    /* --diagnostic (structured JSONL diagnostics on stderr) */
    bool diagnostic;

    /* Progress callback (set by MCP layer, NULL for CLI) */
    void (*progress_cb)(void *ctx, int64_t done, int64_t total);
    void *progress_ctx;
} tt_cli_opts_t;

/*
 * tt_cli_parse -- Parse command-line arguments into opts.
 *
 * argv[0] is the program name, argv[1] is the command name.
 * This function parses argv[2..argc-1].
 *
 * Returns 0 on success, -1 on error (unknown flag, missing value).
 */
int tt_cli_parse(tt_cli_opts_t *opts, int argc, char *argv[]);

/*
 * tt_cli_opts_free -- Free dynamically allocated fields.
 */
void tt_cli_opts_free(tt_cli_opts_t *opts);

/*
 * tt_resolve_project_path -- Resolve project path.
 *
 * Logic:
 *   1. "." -> getcwd()
 *   2. realpath() -> use canonical path
 *   3. If realpath fails -> use original path
 *
 * [caller-frees]
 */
char *tt_resolve_project_path(const char *path);

#endif /* TT_CLI_H */
