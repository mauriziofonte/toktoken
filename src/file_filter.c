/*
 * file_filter.c -- File discovery with filtering, gitignore, and security.
 */

#include "file_filter.h"
#include "secret_patterns.h"
#include "path_validator.h"
#include "platform.h"
#include "error.h"
#include "str_util.h"
#include "fast_hash.h"
#include "diagnostic.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <dirent.h>
#include <signal.h>

/* Global interrupt flag (defined in cmd_index.c). */
extern volatile sig_atomic_t tt_interrupted;

/* ===== Constants (exact copies from FileFilter.php) ===== */

/* 33 directories to always skip */
static const char *SKIP_DIRS[] = {
    /* VCS */
    ".git", ".svn", ".hg",
    /* Dependencies */
    "node_modules", "vendor", "bower_components",
    /* Build output (general) */
    "dist", "build",
    /* Build output (JS/TS frameworks) */
    ".next", ".nuxt", ".svelte-kit", ".output",
    ".turbo", ".parcel-cache", ".angular", ".docusaurus", ".expo", ".wireit",
    "storybook-static",
    /* Build output (JVM) */
    ".gradle",
    /* Build output (Xcode/Swift) */
    "DerivedData", ".build",
    /* Caches */
    ".cache", "__pycache__", ".tox", ".mypy_cache",
    /* Virtual environments */
    ".venv", "venv",
    /* IDE */
    ".idea", ".vscode",
    /* Coverage */
    "coverage", ".nyc_output", "__coverage__",
    NULL};

/* 67 source extensions */
static const char *SOURCE_EXTENSIONS[] = {
    "php", "phtml", "inc",                         /* PHP */
    "js", "jsx", "mjs", "cjs",                     /* JavaScript */
    "ts", "tsx", "mts",                            /* TypeScript */
    "vue",                                         /* Vue */
    "py", "pyw",                                   /* Python */
    "go",                                          /* Go */
    "rs",                                          /* Rust */
    "java",                                        /* Java */
    "c", "h",                                      /* C/C++ */
    "cpp", "cxx", "cc", "hpp", "hxx", "hh",        /* C/C++ */
    "cs",                                          /* C# */
    "rb",                                          /* Ruby */
    "kt", "kts",                                   /* Kotlin */
    "swift",                                       /* Swift */
    "dart",                                        /* Dart */
    "lua",                                         /* Lua */
    "pl", "pm",                                    /* Perl */
    "sh", "bash", "zsh",                           /* Shell scripts */
    "sql",                                         /* SQL */
    "r",                                           /* R language */
    "scala",                                       /* Scala */
    "ex", "exs",                                   /* Elixir */
    "erl", "hrl",                                  /* Erlang */
    "f90", "f95", "f03", "f08", "f", "for", "fpp", /* Fortran */
    "hs",                                          /* Haskell */
    "ml", "mli",                                   /* OCaml */
    "vim",                                         /* Vimscript */
    "el",                                          /* Emacs Lisp */
    "clj", "cljs", "cljc",                         /* Clojure */
    "groovy",                                      /* Groovy (including Gradle scripts) */
    "v", "sv",                                     /* Verilog/SystemVerilog */
    "nix",                                         /* Nix */
    "gleam",                                       /* Gleam */
    "ejs",                                         /* EJS templates (JavaScript) */
    "m", "mm",                                     /* Objective-C */
    "proto",                                       /* Protocol Buffers */
    "sc",                                          /* Scala script */
    "lhs",                                         /* Literate Haskell */
    "gradle",                                      /* Gradle/Groovy */
    "css",                                         /* CSS */
    "toml",                                        /* TOML */
    "tf", "hcl", "tfvars",                         /* HCL/Terraform */
    "graphql", "gql",                              /* GraphQL */
    "jl",                                          /* Julia */
    "gd",                                          /* GDScript */
    "verse",                                       /* Verse (UEFN) */
    "html", "htm",                                 /* HTML */
    "xml", "xul",                                  /* XML/XUL */
    "ahk",                                         /* AutoHotkey */
    "yaml", "yml",                                 /* YAML (for OpenAPI specs) */
    "asm", "s",                                    /* Assembly (GAS/NASM/etc.) */
    "md", "markdown", "mdx",                       /* Markdown */
    NULL};

/* Extensions whose ctags output is not useful for code navigation.
 * Removed from source_ext when smart_filter is active (default). */
static const char *NOCODE_EXTENSIONS[] = {
    "css", "scss", "less", "sass", /* Stylesheets */
    "html", "htm",                 /* Markup */
    "svg",                         /* Vector graphics (XML) */
    "toml",                        /* Config */
    "graphql", "gql",              /* Schema definition */
    "xml", "xul",                  /* XML/XUL (structural, not code) */
    "yaml", "yml",                 /* YAML (config/specs) */
    NULL};

/* Package manager manifests. A non-root subdirectory containing one of these
 * is likely vendored third-party code and is pruned when smart_filter is on. */
static const char *VENDOR_MANIFESTS[] = {
    "composer.json",
    "package.json",
    "setup.py",
    "pyproject.toml",
    "Cargo.toml",
    "go.mod",
    "pom.xml",
    "build.gradle",
    "Gemfile",
    NULL};

/* 65 binary extensions */
static const char *BINARY_EXTENSIONS[] = {
    /* Executables/libraries */
    "exe", "dll", "so", "dylib", "a", "o", "obj", "lib",
    /* JVM */
    "class", "jar", "war", "ear",
    /* Python bytecode */
    "pyc", "pyo", "pyd",
    /* WebAssembly */
    "wasm",
    /* Images */
    "png", "jpg", "jpeg", "gif", "bmp", "ico", "svg", "webp", "avif",
    /* Audio/Video */
    "mp3", "mp4", "avi", "mov", "mkv", "flac", "wav", "ogg", "webm",
    /* Archives */
    "zip", "tar", "gz", "bz2", "xz", "rar", "7z", "zst",
    /* Documents */
    "pdf", "doc", "docx", "xls", "xlsx", "ppt", "pptx",
    /* Fonts */
    "ttf", "otf", "woff", "woff2", "eot",
    /* Database */
    "db", "sqlite", "sqlite3",
    /* Raw binary */
    "bin", "dat", "img", "iso",
    /* Lock/minified/maps (overlap with SKIP_FILE_PATTERNS) */
    "lock",
    "min.js", "min.css",
    "map",
    NULL};

/* 13 skip file patterns */
static const char *SKIP_FILE_PATTERNS[] = {
    "package-lock.json",
    "yarn.lock",
    "pnpm-lock.yaml",
    "composer.lock",
    "Gemfile.lock",
    "Cargo.lock",
    "poetry.lock",
    "go.sum",
    "*.min.js",
    "*.min.css",
    "*.bundle.js",
    "*.chunk.js",
    "*.map",
    NULL};

/* ===== Hashmap initialization for O(1) lookups ===== */

static tt_hashmap_t *build_set(const char **items)
{
    /* Count items */
    int n = 0;
    for (const char **p = items; *p; p++)
        n++;

    tt_hashmap_t *m = tt_hashmap_new((size_t)n * 2);
    if (!m)
        return NULL;

    for (const char **p = items; *p; p++)
    {
        tt_hashmap_set(m, *p, (void *)(uintptr_t)1);
    }
    return m;
}

/* ===== Gitignore rules ===== */

void tt_gitignore_rules_free(tt_gitignore_rules_t *rules)
{
    if (!rules)
        return;
    for (int i = 0; i < rules->count; i++)
        free(rules->patterns[i]);
    free(rules->patterns);
    free(rules);
}

static int free_rules_cb(const char *key, void *value, void *userdata)
{
    (void)key;
    (void)userdata;
    tt_gitignore_rules_free((tt_gitignore_rules_t *)value);
    return 0;
}

/* ===== File filter init/free ===== */

int tt_file_filter_init(tt_file_filter_t *ff, int max_file_size_kb,
                        const char **extra_ignore, bool smart_filter)
{
    if (!ff)
        return -1;
    memset(ff, 0, sizeof(*ff));

    ff->rules = tt_hashmap_new(32);
    ff->source_ext = build_set(SOURCE_EXTENSIONS);
    ff->binary_ext = build_set(BINARY_EXTENSIONS);
    ff->skip_dirs = build_set(SKIP_DIRS);

    if (!ff->rules || !ff->source_ext || !ff->binary_ext || !ff->skip_dirs)
    {
        tt_file_filter_free(ff);
        return -1;
    }

    ff->max_file_size_bytes = max_file_size_kb * 1024;
    ff->extra_ignore = extra_ignore;
    ff->smart_filter = smart_filter;
    ff->workspace_dirs = NULL; /* populated later by tt_discover_paths */

    /* Remove nocode extensions when smart filter is active */
    if (smart_filter)
    {
        for (const char **p = NOCODE_EXTENSIONS; *p; p++)
        {
            tt_hashmap_remove(ff->source_ext, *p);
        }
    }

    return 0;
}

void tt_file_filter_free(tt_file_filter_t *ff)
{
    if (!ff)
        return;

    if (ff->rules)
    {
        tt_hashmap_iter(ff->rules, free_rules_cb, NULL);
        tt_hashmap_free(ff->rules);
        ff->rules = NULL;
    }
    if (ff->source_ext)
    {
        tt_hashmap_free(ff->source_ext);
        ff->source_ext = NULL;
    }
    if (ff->binary_ext)
    {
        tt_hashmap_free(ff->binary_ext);
        ff->binary_ext = NULL;
    }
    if (ff->skip_dirs)
    {
        tt_hashmap_free(ff->skip_dirs);
        ff->skip_dirs = NULL;
    }
    if (ff->workspace_dirs)
    {
        tt_hashmap_free(ff->workspace_dirs);
        ff->workspace_dirs = NULL;
    }
}

/* ===== Gitignore loading ===== */

/*
 * Parse lines from an ignore file into an array of patterns.
 * Skips empty lines, comments (#), and negation (!).
 */
static int parse_ignore_lines(const char *content, char ***out_patterns, int *out_count)
{
    tt_array_t patterns;
    tt_array_init(&patterns);

    const char *p = content;
    while (*p)
    {
        /* Find end of line */
        const char *eol = strchr(p, '\n');
        size_t line_len = eol ? (size_t)(eol - p) : strlen(p);

        /* Copy line for trimming */
        char *line = tt_strndup(p, line_len);
        if (!line)
            break;
        tt_str_trim(line);

        /* Skip empty, comment, negation */
        if (*line && line[0] != '#' && line[0] != '!')
        {
            tt_array_push(&patterns, line);
        }
        else
        {
            free(line);
        }

        p = eol ? eol + 1 : p + line_len;
    }

    *out_count = (int)patterns.len;
    *out_patterns = (char **)patterns.items;
    return 0;
}

int tt_file_filter_load_gitignore(tt_file_filter_t *ff, const char *dir,
                                  const char *root)
{
    if (!ff || !dir || !root)
        return -1;

    char *gi_path = tt_path_join(dir, ".gitignore");
    if (!gi_path)
        return -1;

    size_t flen = 0;
    char *content = tt_read_file(gi_path, &flen);
    free(gi_path);

    if (!content)
        return 0; /* File not found is not an error */

    char **patterns = NULL;
    int count = 0;
    parse_ignore_lines(content, &patterns, &count);
    free(content);

    if (count == 0)
    {
        free(patterns);
        return 0;
    }

    /* Compute relative directory key */
    char *rel_dir;
    if (strcmp(dir, root) == 0)
    {
        rel_dir = tt_strdup("");
    }
    else
    {
        size_t rlen = strlen(root);
        if (strncmp(dir, root, rlen) == 0 && dir[rlen] == '/')
        {
            rel_dir = tt_strdup(dir + rlen + 1);
        }
        else
        {
            rel_dir = tt_strdup("");
        }
    }
    if (!rel_dir)
    {
        for (int i = 0; i < count; i++)
            free(patterns[i]);
        free(patterns);
        return -1;
    }

    /* Check if rules already exist for this directory */
    tt_gitignore_rules_t *existing = tt_hashmap_get(ff->rules, rel_dir);
    if (existing)
    {
        /* Append new patterns to existing */
        char **merged = realloc(existing->patterns,
                                sizeof(char *) * (size_t)(existing->count + count));
        if (!merged)
        {
            for (int i = 0; i < count; i++)
                free(patterns[i]);
            free(patterns);
            free(rel_dir);
            return -1;
        }
        for (int i = 0; i < count; i++)
            merged[existing->count + i] = patterns[i];
        existing->patterns = merged;
        existing->count += count;
        free(patterns);
    }
    else
    {
        tt_gitignore_rules_t *rules = malloc(sizeof(tt_gitignore_rules_t));
        if (!rules)
        {
            for (int i = 0; i < count; i++)
                free(patterns[i]);
            free(patterns);
            free(rel_dir);
            return -1;
        }
        rules->patterns = patterns;
        rules->count = count;
        tt_hashmap_set(ff->rules, rel_dir, rules);
    }

    free(rel_dir);
    return 0;
}

int tt_file_filter_load_ignorefile(tt_file_filter_t *ff, const char *filepath,
                                   const char *root)
{
    if (!ff || !filepath || !root)
        return -1;

    size_t flen = 0;
    char *content = tt_read_file(filepath, &flen);
    if (!content)
        return 0; /* File not found is not an error */

    char **patterns = NULL;
    int count = 0;
    parse_ignore_lines(content, &patterns, &count);
    free(content);

    if (count == 0)
    {
        free(patterns);
        return 0;
    }

    /* Merge into root bucket "" */
    tt_gitignore_rules_t *existing = tt_hashmap_get(ff->rules, "");
    if (existing)
    {
        char **merged = realloc(existing->patterns,
                                sizeof(char *) * (size_t)(existing->count + count));
        if (!merged)
        {
            for (int i = 0; i < count; i++)
                free(patterns[i]);
            free(patterns);
            return -1;
        }
        for (int i = 0; i < count; i++)
            merged[existing->count + i] = patterns[i];
        existing->patterns = merged;
        existing->count += count;
        free(patterns);
    }
    else
    {
        tt_gitignore_rules_t *rules = malloc(sizeof(tt_gitignore_rules_t));
        if (!rules)
        {
            for (int i = 0; i < count; i++)
                free(patterns[i]);
            free(patterns);
            return -1;
        }
        rules->patterns = patterns;
        rules->count = count;
        tt_hashmap_set(ff->rules, "", rules);
    }

    return 0;
}

/* ===== Gitignore matching ===== */

/*
 * Check if rel_path matches a single gitignore pattern with the given rule_dir.
 * Uses the algorithm from FileFilter.php isGitignored().
 */
static bool match_gitignore_pattern(const char *pattern, const char *path_from_rule)
{
    if (!pattern || !*pattern || !path_from_rule)
        return false;

    /* Strip trailing '/' from pattern */
    size_t plen = strlen(pattern);
    char pat_buf[1024];
    if (plen >= sizeof(pat_buf))
        return false;
    memcpy(pat_buf, pattern, plen + 1);
    while (plen > 0 && pat_buf[plen - 1] == '/')
    {
        pat_buf[--plen] = '\0';
    }

    int flags = TT_FNM_CASEFOLD;

    if (pat_buf[0] == '/')
    {
        /* Anchored pattern */
        const char *anchored = pat_buf + 1;
        int pflags = flags | TT_FNM_PATHNAME;

        if (tt_fnmatch_ex(anchored, path_from_rule, pflags))
            return true;

        /* Also try anchored + slash-double-star */
        char with_star[1040];
        snprintf(with_star, sizeof(with_star), "%s/**", anchored);
        if (tt_fnmatch_ex(with_star, path_from_rule, pflags))
            return true;
    }
    else
    {
        /* Unanchored pattern */
        int pflags = flags | TT_FNM_PATHNAME;

        /* Match against full relative path */
        if (tt_fnmatch_ex(pat_buf, path_from_rule, pflags))
            return true;

        /* Match against basename only (without FNM_PATHNAME) */
        const char *base = tt_path_basename(path_from_rule);
        if (tt_fnmatch_ex(pat_buf, base, flags))
            return true;

        /* Match with double-star-slash prefix */
        char with_prefix[1040];
        snprintf(with_prefix, sizeof(with_prefix), "**/%s", pat_buf);
        if (tt_fnmatch_ex(with_prefix, path_from_rule, pflags))
            return true;
    }

    return false;
}

typedef struct
{
    const char *rel_path;
    bool ignored;
} gitignore_check_t;

static int gitignore_check_cb(const char *key, void *value, void *userdata)
{
    gitignore_check_t *ctx = userdata;
    tt_gitignore_rules_t *rules = value;
    const char *rule_dir = key;
    const char *rel_path = ctx->rel_path;

    /* Compute pathFromRule */
    const char *path_from_rule = NULL;
    char *allocated = NULL;

    if (rule_dir[0] == '\0')
    {
        /* Root bucket */
        path_from_rule = rel_path;
    }
    else
    {
        /* Check if rel_path starts with rule_dir + "/" */
        size_t dlen = strlen(rule_dir);
        if (strncmp(rel_path, rule_dir, dlen) == 0 && rel_path[dlen] == '/')
        {
            path_from_rule = rel_path + dlen + 1;
        }
        else
        {
            /* This rule_dir does not apply */
            return 0;
        }
    }

    for (int i = 0; i < rules->count; i++)
    {
        if (match_gitignore_pattern(rules->patterns[i], path_from_rule))
        {
            ctx->ignored = true;
            free(allocated);
            return 1; /* Stop iteration */
        }
    }

    free(allocated);
    return 0;
}

bool tt_file_filter_is_gitignored(const tt_file_filter_t *ff, const char *rel_path)
{
    if (!ff || !ff->rules || !rel_path)
        return false;

    gitignore_check_t ctx = {.rel_path = rel_path, .ignored = false};
    tt_hashmap_iter(ff->rules, gitignore_check_cb, &ctx);
    return ctx.ignored;
}

/* ===== Segment-aware skip dir check ===== */

/*
 * Check if rel_path contains a SKIP_DIRS segment.
 * "build" matches "build/foo.c" (starts with "build/")
 * "build" matches "src/build/foo.c" (contains "/build/")
 * "build" does NOT match "rebuild/foo.c"
 */
static bool has_skip_dir_segment(const tt_file_filter_t *ff, const char *rel_path)
{
    /* Check each SKIP_DIRS against the path segments */
    for (const char **sd = SKIP_DIRS; *sd; sd++)
    {
        size_t dlen = strlen(*sd);

        /* starts_with(relPath, dir + "/") */
        if (strncmp(rel_path, *sd, dlen) == 0 && rel_path[dlen] == '/')
            return true;

        /* contains(relPath, "/" + dir + "/") */
        char needle[256];
        if (dlen + 2 < sizeof(needle))
        {
            needle[0] = '/';
            memcpy(needle + 1, *sd, dlen);
            needle[dlen + 1] = '/';
            needle[dlen + 2] = '\0';
            if (strstr(rel_path, needle))
                return true;
        }
    }
    return false;
}

/* ===== Extension extraction (lowercase) ===== */

static const char *get_ext_lower(const char *basename, char *buf, size_t buf_size)
{
    const char *dot = strrchr(basename, '.');
    if (!dot || dot == basename)
        return NULL;

    const char *ext = dot + 1;
    size_t elen = strlen(ext);
    if (elen == 0 || elen >= buf_size)
        return NULL;

    for (size_t i = 0; i <= elen; i++)
        buf[i] = (char)tolower((unsigned char)ext[i]);

    return buf;
}

/* ===== Workspace detection (monorepo awareness) ===== */

/*
 * add_workspace_dir -- Register a relative directory as a workspace member.
 *
 * If the glob pattern contains a trailing slash-star, we expand it by listing
 * subdirectories. Literal paths are added directly if they exist.
 * All paths stored are relative to project root, without trailing slash.
 */
static void add_workspace_glob(tt_hashmap_t *ws, const char *root,
                               const char *pattern)
{
    if (!ws || !root || !pattern || !pattern[0])
        return;

    /* Strip leading "./" */
    if (pattern[0] == '.' && pattern[1] == '/')
        pattern += 2;

    size_t plen = strlen(pattern);

    /* Strip trailing "/" */
    char pat_clean[1024];
    if (plen >= sizeof(pat_clean))
        return;
    memcpy(pat_clean, pattern, plen + 1);
    while (plen > 0 && pat_clean[plen - 1] == '/')
        pat_clean[--plen] = '\0';

    /* Check if pattern ends with a slash-star (one-level glob) */
    if (plen >= 2 && pat_clean[plen - 2] == '/' && pat_clean[plen - 1] == '*')
    {
        /* Expand: list subdirectories of the parent */
        pat_clean[plen - 2] = '\0'; // "packages/*" -> "packages"
        char *parent = tt_path_join(root, pat_clean);
        if (!parent)
            return;

        DIR *d = opendir(parent);
        if (d)
        {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL)
            {
                if (ent->d_name[0] == '.')
                    continue;

                /* Build absolute path to check it's a directory */
                char *child_abs = tt_path_join(parent, ent->d_name);
                if (!child_abs)
                    continue;
                bool is_dir = tt_is_dir(child_abs);
                free(child_abs);

                if (is_dir)
                {
                    /* Store relative path: "packages/react" */
                    char rel[2048];
                    snprintf(rel, sizeof(rel), "%s/%s", pat_clean, ent->d_name);
                    tt_hashmap_set(ws, rel, (void *)1);
                }
            }
            closedir(d);
        }
        free(parent);
    }
    else
    {
        /* Literal path -- add if it's a real directory */
        char *abs = tt_path_join(root, pat_clean);
        if (abs)
        {
            if (tt_is_dir(abs))
                tt_hashmap_set(ws, pat_clean, (void *)1);
            free(abs);
        }
    }
}

/*
 * detect_workspaces_npm -- Parse root package.json for "workspaces" field.
 *
 * Handles both array form and object form { "packages": [...] }.
 * Uses minimal string scanning (no cJSON dependency in this module).
 */
static void detect_workspaces_npm(tt_hashmap_t *ws, const char *root)
{
    char *pkg_path = tt_path_join(root, "package.json");
    if (!pkg_path)
        return;

    size_t flen = 0;
    char *content = tt_read_file(pkg_path, &flen);
    free(pkg_path);
    if (!content)
        return;

    /*
     * Find "workspaces" key. We look for the pattern "workspaces" followed
     * by a colon and then either an array [...] or object { "packages": [...] }.
     * In both cases, extract string values from the first [...] we find.
     */
    const char *ws_key = strstr(content, "\"workspaces\"");
    if (!ws_key)
    {
        free(content);
        return;
    }

    /* Find the opening '[' (could be nested in an object) */
    const char *bracket = strchr(ws_key, '[');
    if (!bracket)
    {
        free(content);
        return;
    }

    /* Extract each "..." string until ']' */
    const char *p = bracket + 1;
    while (*p && *p != ']')
    {
        if (*p == '"')
        {
            p++;
            const char *start = p;
            while (*p && *p != '"')
                p++;
            if (*p == '"')
            {
                size_t slen = (size_t)(p - start);
                char val[1024];
                if (slen < sizeof(val))
                {
                    memcpy(val, start, slen);
                    val[slen] = '\0';
                    add_workspace_glob(ws, root, val);
                }
                p++;
            }
        }
        else
        {
            p++;
        }
    }

    free(content);
}

/*
 * detect_workspaces_cargo -- Parse root Cargo.toml for [workspace] members.
 *
 * Looks for lines like: members = [ "crates/\*", "lib/\*" ]
 * Handles multi-line arrays.
 */
static void detect_workspaces_cargo(tt_hashmap_t *ws, const char *root)
{
    char *toml_path = tt_path_join(root, "Cargo.toml");
    if (!toml_path)
        return;

    size_t flen = 0;
    char *content = tt_read_file(toml_path, &flen);
    free(toml_path);
    if (!content)
        return;

    /* Find [workspace] section */
    const char *section = strstr(content, "[workspace]");
    if (!section)
    {
        free(content);
        return;
    }

    /* Find "members" key after [workspace] */
    const char *members = strstr(section, "members");
    if (!members)
    {
        free(content);
        return;
    }

    /* Find the '[' that starts the array */
    const char *bracket = strchr(members, '[');
    if (!bracket)
    {
        free(content);
        return;
    }

    /* Extract each "..." value until ']' */
    const char *p = bracket + 1;
    while (*p && *p != ']')
    {
        if (*p == '"')
        {
            p++;
            const char *start = p;
            while (*p && *p != '"')
                p++;
            if (*p == '"')
            {
                size_t slen = (size_t)(p - start);
                char val[1024];
                if (slen < sizeof(val))
                {
                    memcpy(val, start, slen);
                    val[slen] = '\0';
                    add_workspace_glob(ws, root, val);
                }
                p++;
            }
        }
        else
        {
            p++;
        }
    }

    free(content);
}

/*
 * detect_workspaces_go -- Parse go.work for use (...) directives.
 *
 * Format:
 *   use (
 *       .
 *       ./staging/src/k8s.io/api
 *       ./staging/src/k8s.io/apimachinery
 *   )
 */
static void detect_workspaces_go(tt_hashmap_t *ws, const char *root)
{
    char *gowork_path = tt_path_join(root, "go.work");
    if (!gowork_path)
        return;

    size_t flen = 0;
    char *content = tt_read_file(gowork_path, &flen);
    free(gowork_path);
    if (!content)
        return;

    /* Find "use (" */
    const char *use = strstr(content, "use");
    while (use)
    {
        /* Skip to the '(' */
        const char *paren = strchr(use, '(');
        if (!paren)
            break;

        const char *p = paren + 1;
        while (*p && *p != ')')
        {
            /* Skip whitespace */
            while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
                p++;
            if (*p == ')' || !*p)
                break;

            /* Skip comment lines */
            if (*p == '/' && *(p + 1) == '/')
            {
                while (*p && *p != '\n')
                    p++;
                continue;
            }

            /* Read the path token (until whitespace or ')') */
            const char *start = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != ')')
                p++;

            size_t slen = (size_t)(p - start);
            if (slen > 0 && slen < 1024)
            {
                char val[1024];
                memcpy(val, start, slen);
                val[slen] = '\0';
                /* Skip "." (root itself) */
                if (strcmp(val, ".") != 0)
                    add_workspace_glob(ws, root, val);
            }
        }

        /* Look for another "use" block */
        if (*p == ')')
            use = strstr(p + 1, "use");
        else
            break;
    }

    free(content);
}

/*
 * detect_workspaces -- Detect monorepo workspace members from root manifests.
 *
 * Populates ff->workspace_dirs with relative paths of first-party workspace
 * packages. These directories are exempted from the vendor manifest pruning
 * in the smart filter, preventing false-positive exclusion of monorepo code.
 */
static void detect_workspaces(tt_file_filter_t *ff, const char *root)
{
    if (!ff || !root)
        return;

    tt_hashmap_t *ws = tt_hashmap_new(64);
    if (!ws)
        return;

    detect_workspaces_npm(ws, root);
    detect_workspaces_cargo(ws, root);
    detect_workspaces_go(ws, root);

    ff->workspace_dirs = ws;
}

/*
 * is_under_workspace -- Check if a relative directory path is a workspace
 * member or is nested inside one.
 *
 * "packages/react" matches both "packages/react" and "packages/react/src".
 */
static bool is_under_workspace(const tt_hashmap_t *ws, const char *rel_dir)
{
    if (!ws || !rel_dir)
        return false;

    /* Direct match */
    if (tt_hashmap_has(ws, rel_dir))
        return true;

    /* Check if any workspace dir is a prefix of rel_dir */
    /* Walk up the path: "packages/react/src" -> check "packages/react" -> check "packages" */
    char buf[2048];
    size_t len = strlen(rel_dir);
    if (len >= sizeof(buf))
        return false;
    memcpy(buf, rel_dir, len + 1);

    while (len > 0)
    {
        /* Find last '/' */
        char *last_slash = strrchr(buf, '/');
        if (!last_slash)
            break;
        *last_slash = '\0';
        len = (size_t)(last_slash - buf);

        if (tt_hashmap_has(ws, buf))
            return true;
    }

    return false;
}

/* ===== Discovery ===== */

/*
 * is_source_tree_path -- Check if a relative path is under a conventional
 * source-code directory.  Monorepo packages living under src/, lib/,
 * packages/, apps/, internal/, modules/, or crates/ contain their own
 * package-manager manifests (composer.json, package.json, Cargo.toml, ...)
 * but must NOT be treated as vendored third-party code.
 */
static const char *SOURCE_TREE_DIRS[] = {
    "src", "lib", "packages", "apps", "internal", "modules", "crates", NULL};

static bool is_source_tree_path(const char *rel_path)
{
    for (const char **d = SOURCE_TREE_DIRS; *d; d++)
    {
        size_t dlen = strlen(*d);

        /* starts with "src/" etc. */
        if (strncmp(rel_path, *d, dlen) == 0 && rel_path[dlen] == '/')
            return true;

        /* contains "/src/" etc. */
        char needle[64];
        if (dlen + 2 < sizeof(needle))
        {
            needle[0] = '/';
            memcpy(needle + 1, *d, dlen);
            needle[dlen + 1] = '/';
            needle[dlen + 2] = '\0';
            if (strstr(rel_path, needle))
                return true;
        }
    }
    return false;
}

/*
 * has_vendor_manifest -- Check if a directory contains a package manager manifest.
 *
 * Uses a single opendir()/readdir() scan instead of N stat() calls per directory.
 * Matches entry names against the VENDOR_MANIFESTS list (typically 9 entries).
 */
static bool has_vendor_manifest(const char *dir_path)
{
    DIR *d = opendir(dir_path);
    if (!d)
        return false;

    bool found = false;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL)
    {
        for (const char **m = VENDOR_MANIFESTS; *m; m++)
        {
            if (strcmp(ent->d_name, *m) == 0)
            {
                found = true;
                goto done;
            }
        }
    }
done:
    closedir(d);
    return found;
}

/* ===== Path-only discovery (lightweight) ===== */

typedef struct
{
    const char *root;
    tt_file_filter_t *ff;
    tt_array_t paths;
    /* Diagnostic counters (zero-cost when diagnostic disabled) */
    int diag_dirs_visited;
    int diag_dirs_pruned_skipdir;
    int diag_dirs_pruned_vendor;
    int diag_dirs_pruned_gitignore;
    int diag_dirs_pruned_ignore;
    int diag_files_visited;
    int diag_files_rejected_ext;
    int diag_files_rejected_binary;
    int diag_files_rejected_secret;
    int diag_files_rejected_pattern;
    int diag_files_rejected_gitignore;
    int diag_files_rejected_ignore;
    int diag_files_rejected_other;
    int diag_files_accepted;
} discover_paths_ctx_t;

/*
 * Walk callback for path-only discovery.
 *
 * Skips B7 (size) and B8 (binary/hash), which are deferred to the worker.
 */
static int discover_paths_walk_cb(const char *dir, const char *name,
                                  bool is_dir, bool is_symlink, void *userdata)
{
    discover_paths_ctx_t *ctx = userdata;
    const char *root = ctx->root;
    tt_file_filter_t *ff = ctx->ff;

    if (tt_interrupted)
        return -1;

    /* Build full path */
    size_t dlen = strlen(dir);
    size_t nlen = strlen(name);
    char *full = malloc(dlen + 1 + nlen + 1);
    if (!full)
        return -1;
    memcpy(full, dir, dlen);
    full[dlen] = '/';
    memcpy(full + dlen + 1, name, nlen);
    full[dlen + 1 + nlen] = '\0';

    /* Compute relative path */
    size_t rlen = strlen(root);
    const char *rel;
    if (strncmp(full, root, rlen) == 0 && full[rlen] == '/')
    {
        rel = full + rlen + 1;
    }
    else
    {
        free(full);
        return 0;
    }

    if (is_dir)
    {
        ctx->diag_dirs_visited++;

        /* A1: SKIP_DIRS check */
        if (tt_hashmap_has(ff->skip_dirs, name))
        {
            ctx->diag_dirs_pruned_skipdir++;
            free(full);
            return 1;
        }

        /* A1b: Vendor manifest detection (smart_filter only) */
        if (ff->smart_filter && has_vendor_manifest(full) &&
            !is_under_workspace(ff->workspace_dirs, rel) &&
            !is_source_tree_path(rel))
        {
            ctx->diag_dirs_pruned_vendor++;
            free(full);
            return 1;
        }

        /* A2: gitignore check on directory */
        if (tt_file_filter_is_gitignored(ff, rel))
        {
            ctx->diag_dirs_pruned_gitignore++;
            free(full);
            return 1;
        }

        /* A3: extra_ignore check */
        if (ff->extra_ignore)
        {
            for (const char **ei = ff->extra_ignore; *ei; ei++)
            {
                if (tt_fnmatch(*ei, rel, true) || tt_fnmatch(*ei, name, true))
                {
                    ctx->diag_dirs_pruned_ignore++;
                    free(full);
                    return 1;
                }
            }
        }

        /* A4: Load .gitignore from accepted directories */
        tt_file_filter_load_gitignore(ff, full, root);

        free(full);
        return 0;
    }

    /* Phase B: File filtering (simplified — no B7/B8) */
    ctx->diag_files_visited++;

    char ext_buf[64];

    /* B1: Segment-aware skip dir */
    if (has_skip_dir_segment(ff, rel))
    {
        ctx->diag_files_rejected_other++;
        goto skip;
    }

    /* B2: Symlink escape */
    if (is_symlink && tt_is_symlink_escape(full, root))
    {
        ctx->diag_files_rejected_other++;
        goto skip;
    }

    /* B3: Source extension check */
    {
        const char *ext = get_ext_lower(name, ext_buf, sizeof(ext_buf));
        if (!ext || !tt_hashmap_has(ff->source_ext, ext))
        {
            ctx->diag_files_rejected_ext++;
            goto skip;
        }
    }

    /* B4: Binary extension check */
    {
        const char *ext = get_ext_lower(name, ext_buf, sizeof(ext_buf));
        if (ext && tt_hashmap_has(ff->binary_ext, ext))
        {
            ctx->diag_files_rejected_binary++;
            goto skip;
        }
    }

    /* B5: Secret detection */
    if (tt_is_secret(name))
    {
        ctx->diag_files_rejected_secret++;
        goto skip;
    }

    /* B6: Skip file pattern */
    for (const char **sp = SKIP_FILE_PATTERNS; *sp; sp++)
    {
        if (tt_fnmatch(*sp, name, true))
        {
            ctx->diag_files_rejected_pattern++;
            goto skip;
        }
    }

    /* B9: gitignore check */
    if (tt_file_filter_is_gitignored(ff, rel))
    {
        ctx->diag_files_rejected_gitignore++;
        goto skip;
    }

    /* B10: extra_ignore check */
    if (ff->extra_ignore)
    {
        for (const char **ei = ff->extra_ignore; *ei; ei++)
        {
            if (tt_fnmatch(*ei, rel, true))
            {
                ctx->diag_files_rejected_ignore++;
                free(full);
                return 0;
            }
        }
    }

    /* File accepted */
    ctx->diag_files_accepted++;
    tt_array_push(&ctx->paths, tt_strdup(rel));

    free(full);
    return 0;

skip:
    free(full);
    return 0;
}

/* Comparator for qsort: sort strings by path. */
static int cmp_str_by_path(const void *a, const void *b)
{
    const char *pa = *(const char *const *)a;
    const char *pb = *(const char *const *)b;
    return strcmp(pa, pb);
}

int tt_discover_paths(const char *root, tt_file_filter_t *ff,
                      tt_discovered_paths_t *out)
{
    if (!root || !ff || !out)
        return -1;
    memset(out, 0, sizeof(*out));

    discover_paths_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.root = root;
    ctx.ff = ff;
    tt_array_init(&ctx.paths);

    /* Load root .gitignore first */
    tt_file_filter_load_gitignore(ff, root, root);

    /* Load .toktokenignore from root */
    {
        char *tti_path = tt_path_join(root, ".toktokenignore");
        if (tti_path)
        {
            tt_file_filter_load_ignorefile(ff, tti_path, root);
            free(tti_path);
        }
    }

    /* Detect workspace members so they survive vendor manifest pruning */
    if (ff->smart_filter)
        detect_workspaces(ff, root);

    /* Walk directory tree */
    int rc = tt_walk_dir(root, discover_paths_walk_cb, &ctx);
    if (rc < 0)
    {
        tt_array_free_items(&ctx.paths);
        return -1;
    }

    int n = (int)ctx.paths.len;

    /* Sort results alphabetically */
    if (n > 1)
    {
        qsort(ctx.paths.items, (size_t)n, sizeof(char *), cmp_str_by_path);
    }

    out->paths = (char **)ctx.paths.items;
    out->count = n;
    ctx.paths.items = NULL;

    /* Collect file sizes for load balancing */
    out->sizes = calloc((size_t)(n > 0 ? n : 1), sizeof(int64_t));
    if (out->sizes)
    {
        for (int i = 0; i < n; i++)
        {
            char *full = tt_path_join(root, out->paths[i]);
            if (full)
            {
                out->sizes[i] = tt_file_size(full);
                if (out->sizes[i] < 0)
                    out->sizes[i] = 0;
                free(full);
            }
        }
    }

    /* Emit filter stats diagnostic */
    TT_DIAG("discovery", "filter_stats",
            "\"dirs\":%d,\"dirs_pruned_skipdir\":%d,"
            "\"dirs_pruned_vendor\":%d,\"dirs_pruned_gitignore\":%d,"
            "\"dirs_pruned_ignore\":%d,"
            "\"files_visited\":%d,\"accepted\":%d,"
            "\"rej_ext\":%d,\"rej_binary\":%d,"
            "\"rej_secret\":%d,\"rej_pattern\":%d,"
            "\"rej_gitignore\":%d,\"rej_ignore\":%d,\"rej_other\":%d",
            ctx.diag_dirs_visited, ctx.diag_dirs_pruned_skipdir,
            ctx.diag_dirs_pruned_vendor, ctx.diag_dirs_pruned_gitignore,
            ctx.diag_dirs_pruned_ignore,
            ctx.diag_files_visited, ctx.diag_files_accepted,
            ctx.diag_files_rejected_ext, ctx.diag_files_rejected_binary,
            ctx.diag_files_rejected_secret, ctx.diag_files_rejected_pattern,
            ctx.diag_files_rejected_gitignore, ctx.diag_files_rejected_ignore,
            ctx.diag_files_rejected_other);

    return 0;
}

void tt_discovered_paths_free(tt_discovered_paths_t *dp)
{
    if (!dp)
        return;
    for (int i = 0; i < dp->count; i++)
        free(dp->paths[i]);
    free(dp->paths);
    free(dp->sizes);
    dp->paths = NULL;
    dp->sizes = NULL;
    dp->count = 0;
}
