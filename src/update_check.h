/*
 * update_check.h -- Upstream version cache and update detection.
 *
 * Caches the latest upstream version in ~/.cache/toktoken/UPSTREAM_VERSION.
 * Refreshes at most once every 12 hours via curl (non-blocking on failure).
 */

#ifndef TT_UPDATE_CHECK_H
#define TT_UPDATE_CHECK_H

#include <stdbool.h>

/*
 * Result of an update check.
 */
typedef struct {
    char *upstream_version;   /* [owns] e.g. "1.1.0", or NULL if unknown */
    bool update_available;    /* true if upstream > current */
} tt_update_info_t;

/*
 * tt_update_check -- Check for available updates (fast, cached).
 *
 * Hot path (~0ms): stat() + read cached file.
 * Cold path (once per 12h): curl with 5s timeout.
 * Never fails or blocks on network errors.
 *
 * Caller must call tt_update_info_free().
 */
tt_update_info_t tt_update_check(void);

/*
 * tt_update_check_fresh -- Force-fetch upstream version (ignores cache age).
 *
 * Used by --self-update to confirm before downloading.
 * Caller must call tt_update_info_free().
 */
tt_update_info_t tt_update_check_fresh(void);

/*
 * tt_update_info_free -- Free update info resources.
 */
void tt_update_info_free(tt_update_info_t *info);

/*
 * tt_update_platform_binary_name -- Binary asset name for this platform.
 *
 * e.g. "toktoken-linux-x86_64", "toktoken-macos-aarch64", "toktoken-win-x86_64.exe"
 *
 * [borrows] Returns static string. Do not free.
 */
const char *tt_update_platform_binary_name(void);

/*
 * tt_semver_compare -- Compare two "MAJOR.MINOR.PATCH" version strings.
 *
 * Returns: negative if a < b, 0 if equal, positive if a > b.
 * Malformed input returns 0 (safe: treated as equal).
 */
int tt_semver_compare(const char *a, const char *b);

#endif /* TT_UPDATE_CHECK_H */
