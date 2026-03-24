/*
 * update_check.c -- Upstream version cache and update detection.
 *
 * Stores UPSTREAM_VERSION in ~/.cache/toktoken/, refreshed via curl
 * at most once every 12 hours. Designed to be zero-cost on the hot path
 * and silently degrade when offline.
 */

#include "update_check.h"
#include "version.h"
#include "platform.h"
#include "storage_paths.h"
#include "error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

#define TT_UPDATE_URL "https://github.com/mauriziofonte/toktoken/releases/latest/download/VERSION"
#define TT_UPDATE_CACHE_FILE "UPSTREAM_VERSION"
#define TT_UPDATE_MAX_AGE_SEC 43200  /* 12 hours */
#define TT_UPDATE_CURL_TIMEOUT_MS 6000

/* ===== Helpers ===== */

/*
 * trim_whitespace -- Strip leading/trailing whitespace and newlines in-place.
 */
static void trim_whitespace(char *s)
{
    if (!s) return;

    /* trim leading */
    char *start = s;
    while (*start && isspace((unsigned char)*start)) start++;

    if (start != s) {
        size_t len = strlen(start);
        memmove(s, start, len + 1);
    }

    /* trim trailing */
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

/*
 * cache_path -- Build full path to UPSTREAM_VERSION cache file.
 *
 * [caller-frees] Returns NULL on error.
 */
static char *cache_path(void)
{
    char *base = tt_storage_base_dir();
    if (!base) return NULL;
    char *path = tt_path_join(base, TT_UPDATE_CACHE_FILE);
    free(base);
    return path;
}

/*
 * fetch_upstream_version -- Download VERSION file from GitHub to cache.
 *
 * Runs curl with a 5-second timeout. On success, atomically replaces
 * the cache file. On failure, silently returns without modifying cache.
 */
static void fetch_upstream_version(const char *dest_path)
{
    char *curl = tt_which("curl");
    if (!curl) return;

    /* Build temp path for atomic write */
    size_t dlen = strlen(dest_path);
    char *tmp_path = malloc(dlen + 8);
    if (!tmp_path) {
        free(curl);
        return;
    }
    snprintf(tmp_path, dlen + 8, "%s.tmp", dest_path);

    /* Ensure parent directory exists */
    char *base = tt_storage_base_dir();
    if (base) {
        tt_mkdir_p(base);
        free(base);
    }

    const char *argv[] = {
        curl, "-fsSL", "--max-time", "5", "-o", tmp_path, TT_UPDATE_URL, NULL
    };

    tt_proc_result_t res = tt_proc_run(argv, NULL, TT_UPDATE_CURL_TIMEOUT_MS);

    if (res.exit_code == 0) {
        tt_rename_file(tmp_path, dest_path);
    } else {
        tt_remove_file(tmp_path);
    }

    tt_proc_result_free(&res);
    free(curl);
    free(tmp_path);
}

/*
 * read_cached_version -- Read and trim the cached UPSTREAM_VERSION file.
 *
 * [caller-frees] Returns NULL if file doesn't exist or is empty.
 */
static char *read_cached_version(const char *path)
{
    size_t len = 0;
    char *content = tt_read_file(path, &len);
    if (!content || len == 0) {
        free(content);
        return NULL;
    }
    trim_whitespace(content);
    if (content[0] == '\0') {
        free(content);
        return NULL;
    }
    return content;
}

/*
 * build_result -- Build update info from cached version string.
 */
static tt_update_info_t build_result(char *upstream)
{
    tt_update_info_t info;
    info.upstream_version = upstream;
    info.update_available = false;

    if (upstream && upstream[0]) {
        info.update_available = (tt_semver_compare(TT_VERSION, upstream) < 0);
    }

    return info;
}

/* ===== Public API ===== */

int tt_semver_compare(const char *a, const char *b)
{
    if (!a || !b) return 0;

    int a_major = 0, a_minor = 0, a_patch = 0;
    int b_major = 0, b_minor = 0, b_patch = 0;

    if (sscanf(a, "%d.%d.%d", &a_major, &a_minor, &a_patch) < 1) return 0;
    if (sscanf(b, "%d.%d.%d", &b_major, &b_minor, &b_patch) < 1) return 0;

    if (a_major != b_major) return a_major - b_major;
    if (a_minor != b_minor) return a_minor - b_minor;
    return a_patch - b_patch;
}

tt_update_info_t tt_update_check(void)
{
    tt_update_info_t empty = {NULL, false};

    char *path = cache_path();
    if (!path) return empty;

    /* Hot path: check cache age */
    time_t mtime = tt_file_mtime(path);
    time_t now = time(NULL);

    if (mtime > 0 && (now - mtime) < TT_UPDATE_MAX_AGE_SEC) {
        /* Cache is fresh */
        char *version = read_cached_version(path);
        free(path);
        return build_result(version);
    }

    /* Cold path: refresh cache */
    fetch_upstream_version(path);

    char *version = read_cached_version(path);
    free(path);
    return build_result(version);
}

tt_update_info_t tt_update_check_fresh(void)
{
    tt_update_info_t empty = {NULL, false};

    char *path = cache_path();
    if (!path) return empty;

    /* Always fetch, regardless of cache age */
    fetch_upstream_version(path);

    char *version = read_cached_version(path);
    free(path);
    return build_result(version);
}

void tt_update_info_free(tt_update_info_t *info)
{
    if (!info) return;
    free(info->upstream_version);
    info->upstream_version = NULL;
    info->update_available = false;
}

const char *tt_update_platform_binary_name(void)
{
#if defined(TT_PLATFORM_WINDOWS)
    return "toktoken-win-" TT_ARCH ".exe";
#elif defined(TT_PLATFORM_MACOS)
    return "toktoken-macos-" TT_ARCH;
#else
    return "toktoken-linux-" TT_ARCH;
#endif
}
