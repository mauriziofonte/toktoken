/*
 * storage_paths.c -- Compute storage paths for TokToken databases.
 */

#include "storage_paths.h"
#include "platform.h"
#include "fast_hash.h"
#include "error.h"
#include "str_util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Resolve home directory robustly, with fallback chain. */
static char *resolve_home(void)
{
    const char *home = tt_home_dir();
    if (home && home[0] != '\0' && tt_is_dir(home))
    {
        return tt_strdup(home);
    }

    /* Fallback: USERPROFILE (Windows) */
    const char *profile = getenv("USERPROFILE");
    if (profile && profile[0] != '\0' && tt_is_dir(profile))
    {
        return tt_strdup(profile);
    }

    /* Last resort: temp directory */
#ifdef TT_PLATFORM_WINDOWS
    const char *tmp = getenv("TEMP");
    if (!tmp || tmp[0] == '\0')
        tmp = getenv("TMP");
    if (!tmp || tmp[0] == '\0')
        tmp = "C:\\Temp";
#else
    const char *tmp = getenv("TMPDIR");
    if (!tmp || tmp[0] == '\0')
        tmp = "/tmp";
#endif
    return tt_strdup(tmp);
}

char *tt_storage_base_dir(void)
{
    char *home = resolve_home();
    if (!home)
    {
        tt_error_set("storage_paths: cannot resolve home directory");
        return NULL;
    }

    char *cache = tt_path_join(home, ".cache");
    free(home);
    if (!cache)
    {
        tt_error_set("storage_paths: path_join failed");
        return NULL;
    }

    char *new_path = tt_path_join(cache, "toktoken");
    char *old_path = tt_path_join(cache, ".toktoken");
    free(cache);

    if (!new_path || !old_path)
    {
        free(new_path);
        free(old_path);
        tt_error_set("storage_paths: path_join failed");
        return NULL;
    }

    /* Fast path: new directory already exists */
    if (tt_is_dir(new_path))
    {
        free(old_path);
        return new_path;
    }

    /* Migration: old directory exists, new doesn't → atomic rename */
    if (tt_is_dir(old_path))
    {
        if (rename(old_path, new_path) == 0)
        {
            /* Migration succeeded */
            free(old_path);
            return new_path;
        }
        /* rename failed — check if another process migrated concurrently */
        if (tt_is_dir(new_path))
        {
            free(old_path);
            return new_path;
        }
        /* rename failed and new still doesn't exist — another process
         * is actively using old_path. Degrade gracefully: use old. */
        free(new_path);
        return old_path;
    }

    /* Fresh install: neither exists → use new name */
    free(old_path);
    return new_path;
}

char *tt_storage_projects_dir(void)
{
    char *base = tt_storage_base_dir();
    if (!base)
        return NULL;

    char *projects = tt_path_join(base, "projects");
    free(base);
    return projects;
}

char *tt_storage_project_dir(const char *project_path)
{
    if (!project_path || project_path[0] == '\0')
    {
        tt_error_set("storage_paths: project_path is empty");
        return NULL;
    }

    /* Try to resolve the canonical path; fall back to the original. */
    char *canonical = tt_realpath(project_path);
    const char *hash_input = canonical ? canonical : project_path;

    char *hex = tt_fast_hash_hex(hash_input, strlen(hash_input));
    free(canonical);

    if (!hex)
    {
        tt_error_set("storage_paths: hash failed");
        return NULL;
    }

    /* First 12 hex characters */
    char hash12[13];
    memcpy(hash12, hex, 12);
    hash12[12] = '\0';
    free(hex);

    char *projects = tt_storage_projects_dir();
    if (!projects)
        return NULL;

    char *dir = tt_path_join(projects, hash12);
    free(projects);

    return dir;
}

char *tt_storage_db_path(const char *project_path)
{
    char *dir = tt_storage_project_dir(project_path);
    if (!dir)
        return NULL;

    char *db = tt_path_join(dir, "db.sqlite");
    free(dir);

    return db;
}

char *tt_storage_logs_dir(void)
{
    char *base = tt_storage_base_dir();
    if (!base)
        return NULL;

    char *logs = tt_path_join(base, "logs");
    free(base);
    return logs;
}
