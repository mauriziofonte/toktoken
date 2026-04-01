/*
 * config.h -- Configuration loading with merge hierarchy.
 *
 * Merge order (highest priority last):
 *   1. Hardcoded defaults
 *   2. ~/.toktoken.json (global) -- all sections
 *   3. {project}/.toktoken.json (project) -- only "index" section
 *   4. Environment variables (TOKTOKEN_EXTRA_IGNORE, TOKTOKEN_STALENESS_DAYS,
 *      TOKTOKEN_EXTRA_EXTENSIONS)
 *
 * Invalid JSON or missing files are silently ignored.
 */

#ifndef TT_CONFIG_H
#define TT_CONFIG_H

#include <stdbool.h>

typedef struct
{
    /* index section */
    int max_file_size_kb;         /* default: 1024 */
    int max_files;                /* default: 200000 */
    int staleness_days;           /* default: 7 */
    int ctags_timeout_seconds;    /* default: 120 */
    char **extra_ignore_patterns; /* [owns] default: empty */
    int extra_ignore_count;
    char **languages; /* [owns] default: empty (= all) */
    int language_count;
    char **extra_ext_keys;      /* [owns] extension strings (no dot) */
    char **extra_ext_languages; /* [owns] corresponding language strings */
    int extra_ext_count;

    char **include_dirs; /* [owns] dirs to force-include from SKIP_DIRS */
    int include_dir_count;

    int workers;       /* default: 0 = auto (tt_cpu_count) */
    bool smart_filter; /* default: true */

    /* logging section */
    char *log_level; /* [owns] default: "info" */
} tt_config_t;

/*
 * tt_config_load -- Load configuration for a project.
 *
 * Applies the full merge hierarchy: defaults -> global -> project -> env.
 * Returns 0 on success (always succeeds, missing/invalid files are ignored).
 */
int tt_config_load(tt_config_t *config, const char *project_path);

/*
 * tt_config_free -- Free all dynamically allocated fields.
 */
void tt_config_free(tt_config_t *config);

#endif /* TT_CONFIG_H */
