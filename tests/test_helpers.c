/*
 * test_helpers.c -- Implementation of test utility functions.
 */

#include "test_helpers.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

const char *tt_test_fixtures_dir(void)
{
    static char buf[1024];
    static int resolved = 0;

    if (resolved) return buf[0] ? buf : NULL;
    resolved = 1;

    const char *candidates[] = {
        "tests/fixtures",
        "../tests/fixtures",
        "../../tests/fixtures",
    };

    for (int i = 0; i < 3; i++) {
        struct stat st;
        if (stat(candidates[i], &st) == 0 && S_ISDIR(st.st_mode)) {
            /* Verify directory has actual fixtures (not an empty build artifact) */
            char sentinel[2048];
            snprintf(sentinel, sizeof(sentinel), "%s/sample.blade.php", candidates[i]);
            if (stat(sentinel, &st) != 0) continue;

            char *rp = tt_realpath(candidates[i]);
            if (rp) {
                snprintf(buf, sizeof(buf), "%s", rp);
                free(rp);
                return buf;
            }
        }
    }

    buf[0] = '\0';
    return NULL;
}

const char *tt_test_fixture(const char *relative_path)
{
    static char buf[2048];
    const char *base = tt_test_fixtures_dir();
    if (!base) return NULL;
    snprintf(buf, sizeof(buf), "%s/%s", base, relative_path);
    return buf;
}

char *tt_test_tmpdir(void)
{
    /*
     * Cross-platform temp directory creation.
     * Uses pid + clock + monotonic counter for a unique suffix.
     * The counter guarantees uniqueness even when clock() returns
     * the same value for consecutive calls (low resolution on Windows).
     */
    static int counter = 0;
    const char *base;
#ifdef TT_PLATFORM_WINDOWS
    base = getenv("TEMP");
    if (!base) base = getenv("TMP");
    if (!base) base = "C:\\Temp";
#else
    base = "/tmp";
#endif

    char path[512];
    snprintf(path, sizeof(path), "%s/tt_test_%d_%lu_%d",
             base, tt_getpid(), (unsigned long)clock(), counter++);

    if (tt_mkdir_p(path) != 0) return NULL;
    return strdup(path);
}

int tt_test_rmdir(const char *path)
{
    if (!path) return -1;
    return tt_remove_dir_recursive(path);
}

/* Ensure parent directories exist for a file path. */
static int ensure_parent_dir(const char *filepath)
{
    char *tmp = strdup(filepath);
    if (!tmp) return -1;

    /* Find last separator (works with both / and \) */
    char *last_sep = NULL;
    for (char *p = tmp; *p; p++) {
        if (*p == '/' || *p == '\\') last_sep = p;
    }
    if (!last_sep) { free(tmp); return 0; } /* no directory component */

    *last_sep = '\0';
    int rc = tt_mkdir_p(tmp);
    free(tmp);
    return rc;
}

int tt_test_write_file(const char *dir, const char *relative_path,
                        const char *content)
{
    char full[2048];
    snprintf(full, sizeof(full), "%s/%s", dir, relative_path);

    if (ensure_parent_dir(full) < 0) return -1;

    FILE *f = fopen(full, "w");
    if (!f) return -1;

    if (content && content[0]) {
        fputs(content, f);
    }
    fclose(f);
    return 0;
}
