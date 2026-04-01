/*
 * config.c -- Configuration loading with merge hierarchy.
 */

#include "config.h"
#include "platform.h"
#include "str_util.h"

#include <cJSON.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- Defaults ---- */

#define DEFAULT_MAX_FILE_SIZE_KB 2048
#define DEFAULT_MAX_FILES 200000
#define DEFAULT_STALENESS_DAYS 7
#define DEFAULT_CTAGS_TIMEOUT 120
#define DEFAULT_LOG_LEVEL "info"

/* ---- Helpers ---- */

/* Add a pattern to the extra_ignore list if not already present */
static void add_ignore_pattern(tt_config_t *config, const char *pattern)
{
    if (!pattern || !pattern[0])
        return;

    /* Dedup */
    for (int i = 0; i < config->extra_ignore_count; i++)
    {
        if (strcmp(config->extra_ignore_patterns[i], pattern) == 0)
            return;
    }

    char **new_arr = realloc(config->extra_ignore_patterns,
                             (size_t)(config->extra_ignore_count + 1) * sizeof(char *));
    if (!new_arr)
        return;
    config->extra_ignore_patterns = new_arr;
    config->extra_ignore_patterns[config->extra_ignore_count] = tt_strdup(pattern);
    config->extra_ignore_count++;
}

/* Add a language to the languages list */
static void add_language(tt_config_t *config, const char *lang)
{
    if (!lang || !lang[0])
        return;

    char **new_arr = realloc(config->languages,
                             (size_t)(config->language_count + 1) * sizeof(char *));
    if (!new_arr)
        return;
    config->languages = new_arr;
    config->languages[config->language_count] = tt_strdup(lang);
    config->language_count++;
}

/* Free and reset the languages list */
static void clear_languages(tt_config_t *config)
{
    for (int i = 0; i < config->language_count; i++)
        free(config->languages[i]);
    free(config->languages);
    config->languages = NULL;
    config->language_count = 0;
}

/* Free and reset the extra_ignore list */
static void clear_extra_ignore(tt_config_t *config)
{
    for (int i = 0; i < config->extra_ignore_count; i++)
        free(config->extra_ignore_patterns[i]);
    free(config->extra_ignore_patterns);
    config->extra_ignore_patterns = NULL;
    config->extra_ignore_count = 0;
}

/* Add a directory to the include_dirs list if not already present */
static void add_include_dir(tt_config_t *config, const char *dir)
{
    if (!dir || !dir[0])
        return;

    /* Dedup */
    for (int i = 0; i < config->include_dir_count; i++)
    {
        if (strcmp(config->include_dirs[i], dir) == 0)
            return;
    }

    char **new_arr = realloc(config->include_dirs,
                             (size_t)(config->include_dir_count + 1) * sizeof(char *));
    if (!new_arr)
        return;
    config->include_dirs = new_arr;
    config->include_dirs[config->include_dir_count] = tt_strdup(dir);
    config->include_dir_count++;
}

/* Free and reset the include_dirs list */
static void clear_include_dirs(tt_config_t *config)
{
    for (int i = 0; i < config->include_dir_count; i++)
        free(config->include_dirs[i]);
    free(config->include_dirs);
    config->include_dirs = NULL;
    config->include_dir_count = 0;
}

/* Add an extension->language mapping to extra_extensions */
static void add_extra_extension(tt_config_t *config, const char *ext, const char *lang)
{
    if (!ext || !ext[0] || !lang || !lang[0])
        return;

    /* Dedup */
    for (int i = 0; i < config->extra_ext_count; i++)
    {
        if (strcmp(config->extra_ext_keys[i], ext) == 0)
        {
            free(config->extra_ext_languages[i]);
            config->extra_ext_languages[i] = tt_strdup(lang);
            return;
        }
    }

    size_t new_size = (size_t)(config->extra_ext_count + 1) * sizeof(char *);
    char **new_keys = realloc(config->extra_ext_keys, new_size);
    if (!new_keys)
        return;
    char **new_langs = realloc(config->extra_ext_languages, new_size);
    if (!new_langs)
    {
        /* Commit keys only — both arrays stay at consistent count */
        config->extra_ext_keys = new_keys;
        return;
    }
    config->extra_ext_keys = new_keys;
    config->extra_ext_languages = new_langs;
    config->extra_ext_keys[config->extra_ext_count] = tt_strdup(ext);
    config->extra_ext_languages[config->extra_ext_count] = tt_strdup(lang);
    config->extra_ext_count++;
}

/* Free and reset the extra_extensions list */
static void clear_extra_extensions(tt_config_t *config)
{
    for (int i = 0; i < config->extra_ext_count; i++)
    {
        free(config->extra_ext_keys[i]);
        free(config->extra_ext_languages[i]);
    }
    free(config->extra_ext_keys);
    free(config->extra_ext_languages);
    config->extra_ext_keys = NULL;
    config->extra_ext_languages = NULL;
    config->extra_ext_count = 0;
}

/* Merge the "index" section from a cJSON object into config */
static void merge_index_section(tt_config_t *config, const cJSON *index_obj)
{
    if (!cJSON_IsObject(index_obj))
        return;

    const cJSON *item;

    item = cJSON_GetObjectItemCaseSensitive(index_obj, "max_file_size_kb");
    if (cJSON_IsNumber(item))
        config->max_file_size_kb = item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(index_obj, "max_files");
    if (cJSON_IsNumber(item))
        config->max_files = item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(index_obj, "staleness_days");
    if (cJSON_IsNumber(item))
        config->staleness_days = item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(index_obj, "ctags_timeout_seconds");
    if (cJSON_IsNumber(item))
        config->ctags_timeout_seconds = item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(index_obj, "extra_ignore_patterns");
    if (cJSON_IsArray(item))
    {
        clear_extra_ignore(config);
        const cJSON *el;
        cJSON_ArrayForEach(el, item)
        {
            if (cJSON_IsString(el) && el->valuestring[0])
                add_ignore_pattern(config, el->valuestring);
        }
    }

    item = cJSON_GetObjectItemCaseSensitive(index_obj, "languages");
    if (cJSON_IsArray(item))
    {
        clear_languages(config);
        const cJSON *el;
        cJSON_ArrayForEach(el, item)
        {
            if (cJSON_IsString(el) && el->valuestring[0])
                add_language(config, el->valuestring);
        }
    }

    item = cJSON_GetObjectItemCaseSensitive(index_obj, "include_dirs");
    if (cJSON_IsArray(item))
    {
        clear_include_dirs(config);
        const cJSON *el;
        cJSON_ArrayForEach(el, item)
        {
            if (cJSON_IsString(el) && el->valuestring[0])
                add_include_dir(config, el->valuestring);
        }
    }

    item = cJSON_GetObjectItemCaseSensitive(index_obj, "workers");
    if (cJSON_IsNumber(item) && item->valueint >= 0)
        config->workers = item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(index_obj, "smart_filter");
    if (cJSON_IsBool(item))
        config->smart_filter = cJSON_IsTrue(item);

    /* extra_extensions: {"ext": "language", ...} */
    item = cJSON_GetObjectItemCaseSensitive(index_obj, "extra_extensions");
    if (cJSON_IsObject(item))
    {
        const cJSON *el;
        cJSON_ArrayForEach(el, item)
        {
            if (cJSON_IsString(el) && el->string && el->string[0] &&
                el->valuestring && el->valuestring[0])
                add_extra_extension(config, el->string, el->valuestring);
        }
    }
}

/* Merge the "logging" section from a cJSON object into config */
static void merge_logging_section(tt_config_t *config, const cJSON *logging_obj)
{
    if (!cJSON_IsObject(logging_obj))
        return;

    const cJSON *item = cJSON_GetObjectItemCaseSensitive(logging_obj, "level");
    if (cJSON_IsString(item) && item->valuestring[0])
    {
        free(config->log_level);
        config->log_level = tt_strdup(item->valuestring);
    }
}

/* Merge from a JSON file. If project_scope is true, only "index" section. */
static void merge_from_file(tt_config_t *config, const char *path, bool project_scope)
{
    size_t len = 0;
    char *content = tt_read_file(path, &len);
    if (!content)
        return;

    cJSON *root = cJSON_Parse(content);
    free(content);
    if (!root || !cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        return;
    }

    /* Index section (always allowed) */
    const cJSON *index_obj = cJSON_GetObjectItemCaseSensitive(root, "index");
    if (index_obj)
        merge_index_section(config, index_obj);

    /* Other sections only for global config */
    if (!project_scope)
    {
        const cJSON *logging_obj = cJSON_GetObjectItemCaseSensitive(root, "logging");
        if (logging_obj)
            merge_logging_section(config, logging_obj);
    }

    cJSON_Delete(root);
}

/*
 * Get global config path: HOME/.toktoken.json
 * Falls back to USERPROFILE on Windows. Returns NULL if no home found.
 */
static char *get_global_config_path(void)
{
    const char *home = getenv("HOME");
    if (!home || !home[0])
        home = getenv("USERPROFILE");
    if (!home || !home[0])
        return NULL;

    return tt_path_join(home, ".toktoken.json");
}

/* Apply environment variable overrides */
static void apply_env_overrides(tt_config_t *config)
{
    /* TOKTOKEN_EXTRA_IGNORE: JSON array or comma-separated */
    const char *extra = getenv("TOKTOKEN_EXTRA_IGNORE");
    if (extra && extra[0])
    {
        /* Try JSON array first */
        cJSON *arr = cJSON_Parse(extra);
        if (arr && cJSON_IsArray(arr))
        {
            const cJSON *el;
            cJSON_ArrayForEach(el, arr)
            {
                if (cJSON_IsString(el) && el->valuestring[0])
                    add_ignore_pattern(config, el->valuestring);
            }
            cJSON_Delete(arr);
        }
        else
        {
            cJSON_Delete(arr);
            /* Split on comma, trim, filter empty */
            int count = 0;
            char **parts = tt_str_split(extra, ',', &count);
            if (parts)
            {
                for (int i = 0; i < count; i++)
                {
                    char *trimmed = tt_str_trim(parts[i]);
                    if (trimmed && trimmed[0])
                        add_ignore_pattern(config, trimmed);
                }
                tt_str_split_free(parts);
            }
        }
    }

    /* TOKTOKEN_STALENESS_DAYS: int, min 1 */
    const char *staleness = getenv("TOKTOKEN_STALENESS_DAYS");
    if (staleness && staleness[0])
    {
        int val = atoi(staleness);
        if (val < 1)
            val = 1;
        config->staleness_days = val;
    }

    /* TOKTOKEN_INCLUDE_DIRS: JSON array or comma-separated */
    const char *include = getenv("TOKTOKEN_INCLUDE_DIRS");
    if (include && include[0])
    {
        cJSON *arr = cJSON_Parse(include);
        if (arr && cJSON_IsArray(arr))
        {
            const cJSON *el;
            cJSON_ArrayForEach(el, arr)
            {
                if (cJSON_IsString(el) && el->valuestring[0])
                    add_include_dir(config, el->valuestring);
            }
            cJSON_Delete(arr);
        }
        else
        {
            cJSON_Delete(arr);
            int count = 0;
            char **parts = tt_str_split(include, ',', &count);
            if (parts)
            {
                for (int i = 0; i < count; i++)
                {
                    char *trimmed = tt_str_trim(parts[i]);
                    if (trimmed && trimmed[0])
                        add_include_dir(config, trimmed);
                }
                tt_str_split_free(parts);
            }
        }
    }

    /* TOKTOKEN_EXTRA_EXTENSIONS: "ext1:lang1,ext2:lang2" */
    const char *extra_ext = getenv("TOKTOKEN_EXTRA_EXTENSIONS");
    if (extra_ext && extra_ext[0])
    {
        int count = 0;
        char **parts = tt_str_split(extra_ext, ',', &count);
        if (parts)
        {
            for (int i = 0; i < count; i++)
            {
                char *trimmed = tt_str_trim(parts[i]);
                if (!trimmed || !trimmed[0])
                    continue;
                /* Split on ':' -> ext:lang */
                char *colon = strchr(trimmed, ':');
                if (colon && colon != trimmed)
                {
                    *colon = '\0';
                    const char *ext = trimmed;
                    const char *lang = colon + 1;
                    if (ext[0] && lang[0])
                        add_extra_extension(config, ext, lang);
                }
            }
            tt_str_split_free(parts);
        }
    }
}

/* ---- Public API ---- */

int tt_config_load(tt_config_t *config, const char *project_path)
{
    /* Step 1: Set defaults */
    memset(config, 0, sizeof(*config));
    config->max_file_size_kb = DEFAULT_MAX_FILE_SIZE_KB;
    config->max_files = DEFAULT_MAX_FILES;
    config->staleness_days = DEFAULT_STALENESS_DAYS;
    config->ctags_timeout_seconds = DEFAULT_CTAGS_TIMEOUT;
    config->log_level = tt_strdup(DEFAULT_LOG_LEVEL);
    config->smart_filter = true;

    /* Step 2: Global config (~/.toktoken.json) */
    char *global_path = get_global_config_path();
    if (global_path)
    {
        if (tt_file_exists(global_path))
            merge_from_file(config, global_path, false);
        free(global_path);
    }

    /* Step 3: Project config ({project}/.toktoken.json) */
    if (project_path && project_path[0])
    {
        char *project_config = tt_path_join(project_path, ".toktoken.json");
        if (project_config)
        {
            if (tt_file_exists(project_config))
                merge_from_file(config, project_config, true);
            free(project_config);
        }
    }

    /* Step 4: Environment variables (highest priority) */
    apply_env_overrides(config);

    return 0;
}

void tt_config_free(tt_config_t *config)
{
    clear_extra_ignore(config);
    clear_languages(config);
    clear_extra_extensions(config);
    clear_include_dirs(config);
    free(config->log_level);
    config->log_level = NULL;
}
