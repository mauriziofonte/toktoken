/*
 * file_filter.h -- File discovery with filtering, gitignore, and security.
 *
 * Implements the full pipeline from FileFilter.php:
 * directory pruning, source extension check, binary detection,
 * secret patterns, skip file patterns, size/content limits,
 * gitignore/toktokenignore parsing, and SHA-256 hashing.
 */

#ifndef TT_FILE_FILTER_H
#define TT_FILE_FILTER_H

#include "hashmap.h"

#include <stdbool.h>
#include <stdint.h>

/* ---- Gitignore rules for a single directory ---- */

typedef struct
{
    char **patterns; /* [owns] Array of pattern strings */
    int count;
} tt_gitignore_rules_t;

/*
 * tt_gitignore_rules_free -- Free a rules struct (patterns + strings).
 */
void tt_gitignore_rules_free(tt_gitignore_rules_t *rules);

/* ---- File filter state ---- */

typedef struct
{
    tt_hashmap_t *rules;      /* dir_relative -> tt_gitignore_rules_t* */
    tt_hashmap_t *source_ext; /* extension (lowercase) -> (void*)1 */
    tt_hashmap_t *binary_ext; /* extension (lowercase) -> (void*)1 */
    tt_hashmap_t *skip_dirs;  /* directory name -> (void*)1 */
    int max_file_size_bytes;
    const char **extra_ignore; /* [borrows] NULL-terminated, caller-owned */
    bool smart_filter;         /* exclude nocode extensions + prune vendor dirs */
    tt_hashmap_t *workspace_dirs; /* [owns] relative workspace member paths (monorepo) */
    tt_hashmap_t *include_dirs;   /* [owns] force-included dirs (override skip_dirs) */
} tt_file_filter_t;

/*
 * tt_file_filter_init -- Initialize the file filter.
 *
 * max_file_size_kb: maximum file size in kilobytes (0 = no limit).
 * extra_ignore: NULL-terminated array of additional ignore patterns, or NULL.
 * smart_filter: when true, exclude non-code extensions (CSS, HTML, etc.) and
 *               prune subdirectories containing package manager manifests.
 * include_dirs: NULL-terminated array of directory names to force-include even
 *               if in SKIP_DIRS (e.g. "vendor"). VCS dirs (.git, .svn, .hg) are
 *               always blocked. Pass NULL for default behavior.
 * Returns 0 on success, -1 on error.
 */
int tt_file_filter_init(tt_file_filter_t *ff, int max_file_size_kb,
                        const char **extra_ignore, bool smart_filter,
                        const char **include_dirs);

/*
 * tt_file_filter_add_extensions -- Add extra source extensions at runtime.
 *
 * Adds each key to the source_ext hashmap so files with these extensions
 * pass the source extension check. Keys should be lowercase, without dot.
 */
void tt_file_filter_add_extensions(tt_file_filter_t *ff,
                                   const char **ext_keys, int count);

/*
 * tt_file_filter_free -- Free all internal state.
 */
void tt_file_filter_free(tt_file_filter_t *ff);

/*
 * tt_file_filter_load_gitignore -- Load .gitignore rules from a directory.
 *
 * Reads dir/.gitignore and stores patterns keyed by relative directory.
 * dir: absolute path to the directory containing .gitignore.
 * root: absolute path to the project root.
 * Returns 0 on success, -1 on error (file not found is not an error -- returns 0).
 */
int tt_file_filter_load_gitignore(tt_file_filter_t *ff, const char *dir,
                                  const char *root);

/*
 * tt_file_filter_load_ignorefile -- Load a custom ignore file (e.g. .toktokenignore).
 *
 * Rules are merged into the root bucket ("").
 * filepath: absolute path to the ignore file.
 * root: absolute path to the project root.
 * Returns 0 on success, -1 on error.
 */
int tt_file_filter_load_ignorefile(tt_file_filter_t *ff, const char *filepath,
                                   const char *root);

/*
 * tt_file_filter_is_gitignored -- Check if a relative path is gitignored.
 *
 * rel_path: path relative to the project root.
 */
bool tt_file_filter_is_gitignored(const tt_file_filter_t *ff, const char *rel_path);

/* ---- Discovery result ---- */

typedef struct
{
    char **paths;     /* [owns] Relative paths, sorted alphabetically */
    int64_t *sizes;   /* [owns] File sizes in bytes, parallel to paths */
    int count;
} tt_discovered_paths_t;

/*
 * tt_discover_paths -- Discover all indexable source files (paths only).
 *
 * Lightweight path-only discovery. Skips B7 (size check), B8 (binary check,
 * content read, hash). Discovery is fast and I/O-light; heavy checks
 * are deferred to workers.
 * Returns 0 on success, -1 on error.
 */
int tt_discover_paths(const char *root, tt_file_filter_t *ff,
                      tt_discovered_paths_t *out);

/*
 * tt_discovered_paths_free -- Free path-only discovery results.
 */
void tt_discovered_paths_free(tt_discovered_paths_t *dp);

#endif /* TT_FILE_FILTER_H */
