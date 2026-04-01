/*
 * path_validator.c -- Path validation and symlink escape detection.
 */

#include "path_validator.h"
#include "platform.h"
#include "str_util.h"

#include <stdlib.h>
#include <string.h>

bool tt_path_validate(const char *path, const char *root)
{
    if (!path || !root)
        return false;

    /* Require both to exist.  POSIX realpath() enforces this implicitly
     * (returns NULL for non-existent paths), but Windows GetFullPathNameW
     * resolves even non-existent paths textually. */
    if (!tt_file_exists(path) && !tt_is_dir(path))
        return false;
    if (!tt_is_dir(root))
        return false;

    char *resolved_path = tt_realpath(path);
    char *resolved_root = tt_realpath(root);

    if (!resolved_path || !resolved_root)
    {
        free(resolved_path);
        free(resolved_root);
        return false;
    }

    bool valid = false;

    if (strcmp(resolved_path, resolved_root) == 0)
    {
        valid = true;
    }
    else
    {
        /* Check resolved_path starts with resolved_root + separator */
        size_t rlen = strlen(resolved_root);
        if (strncmp(resolved_path, resolved_root, rlen) == 0 &&
            (resolved_path[rlen] == '/' || resolved_path[rlen] == '\\'))
        {
            valid = true;
        }
    }

    free(resolved_path);
    free(resolved_root);
    return valid;
}

bool tt_is_symlink_escape(const char *path, const char *root)
{
    if (!path || !root)
        return false;

    /* Not a symlink? Not an escape. */
    if (!tt_is_symlink(path))
        return false;

    /* Resolve symlink target */
    char *resolved = tt_realpath(path);
    if (!resolved)
    {
        /* Target does not exist -- treat as escape */
        return true;
    }

    /* Check if resolved target is within root */
    bool inside = tt_path_validate(resolved, root);
    free(resolved);
    return !inside;
}
