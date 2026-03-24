/*
 * test_update_check.c -- Unit tests for update_check and self-update modules.
 *
 * Covers: semver comparison, platform binary name, self-exe path detection,
 * atomic rename edge cases, chmod, SHA256 verification, version cache I/O,
 * and graceful degradation on errors.
 */

#include "test_framework.h"
#include "update_check.h"
#include "platform.h"
#include "sha256_util.h"
#include "storage_paths.h"
#include "version.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>

/* ================================================================
 * tt_semver_compare
 * ================================================================ */

TT_TEST(test_semver_equal)
{
    TT_ASSERT_EQ_INT(0, tt_semver_compare("1.0.0", "1.0.0"));
    TT_ASSERT_EQ_INT(0, tt_semver_compare("0.0.0", "0.0.0"));
    TT_ASSERT_EQ_INT(0, tt_semver_compare("99.99.99", "99.99.99"));
    TT_ASSERT_EQ_INT(0, tt_semver_compare("10.20.30", "10.20.30"));
}

TT_TEST(test_semver_less)
{
    TT_ASSERT_TRUE(tt_semver_compare("1.0.0", "1.0.1") < 0);
    TT_ASSERT_TRUE(tt_semver_compare("1.0.0", "1.1.0") < 0);
    TT_ASSERT_TRUE(tt_semver_compare("1.0.0", "2.0.0") < 0);
    TT_ASSERT_TRUE(tt_semver_compare("1.9.9", "2.0.0") < 0);
    TT_ASSERT_TRUE(tt_semver_compare("0.1.0", "0.2.0") < 0);
    TT_ASSERT_TRUE(tt_semver_compare("0.0.1", "0.0.2") < 0);
}

TT_TEST(test_semver_greater)
{
    TT_ASSERT_TRUE(tt_semver_compare("1.0.1", "1.0.0") > 0);
    TT_ASSERT_TRUE(tt_semver_compare("1.1.0", "1.0.0") > 0);
    TT_ASSERT_TRUE(tt_semver_compare("2.0.0", "1.0.0") > 0);
    TT_ASSERT_TRUE(tt_semver_compare("2.0.0", "1.9.9") > 0);
    TT_ASSERT_TRUE(tt_semver_compare("10.0.0", "9.99.99") > 0);
}

TT_TEST(test_semver_malformed)
{
    /* Malformed input returns 0 (treated as equal = safe, no false positive) */
    TT_ASSERT_EQ_INT(0, tt_semver_compare(NULL, "1.0.0"));
    TT_ASSERT_EQ_INT(0, tt_semver_compare("1.0.0", NULL));
    TT_ASSERT_EQ_INT(0, tt_semver_compare(NULL, NULL));
    TT_ASSERT_EQ_INT(0, tt_semver_compare("", "1.0.0"));
    TT_ASSERT_EQ_INT(0, tt_semver_compare("abc", "1.0.0"));
    TT_ASSERT_EQ_INT(0, tt_semver_compare("not.a.version", "1.0.0"));
    TT_ASSERT_EQ_INT(0, tt_semver_compare("", ""));
}

TT_TEST(test_semver_partial)
{
    /* Missing components default to 0 via sscanf behavior */
    TT_ASSERT_EQ_INT(0, tt_semver_compare("1", "1.0.0"));
    TT_ASSERT_TRUE(tt_semver_compare("1", "2.0.0") < 0);
    TT_ASSERT_EQ_INT(0, tt_semver_compare("1.0", "1.0.0"));
    TT_ASSERT_TRUE(tt_semver_compare("1.0", "1.1.0") < 0);
}

TT_TEST(test_semver_with_whitespace_prefix)
{
    /* Version strings from VERSION file may have whitespace/newlines.
     * The cache reader trims them, but semver_compare should handle
     * a leading space gracefully (sscanf skips whitespace on %d). */
    TT_ASSERT_EQ_INT(0, tt_semver_compare(" 1.0.0", "1.0.0"));
}

TT_TEST(test_semver_large_numbers)
{
    /* Versions with large component numbers */
    TT_ASSERT_TRUE(tt_semver_compare("1.0.0", "1.0.999") < 0);
    TT_ASSERT_TRUE(tt_semver_compare("100.200.300", "100.200.301") < 0);
    TT_ASSERT_EQ_INT(0, tt_semver_compare("100.200.300", "100.200.300"));
}

TT_TEST(test_semver_negative_numbers)
{
    /* Negative numbers are technically malformed semver, but sscanf handles them.
     * Just verify no crash. */
    int result = tt_semver_compare("-1.0.0", "1.0.0");
    TT_ASSERT_TRUE(result < 0);
}

/* ================================================================
 * tt_update_platform_binary_name
 * ================================================================ */

TT_TEST(test_platform_binary_name_format)
{
    const char *name = tt_update_platform_binary_name();
    TT_ASSERT_NOT_NULL(name);
    TT_ASSERT_TRUE(strlen(name) > 10);

    /* Must start with "toktoken-" */
    TT_ASSERT_TRUE(strncmp(name, "toktoken-", 9) == 0);

    /* Must contain a platform identifier */
    TT_ASSERT_TRUE(
        strstr(name, "linux") != NULL ||
        strstr(name, "macos") != NULL ||
        strstr(name, "win") != NULL);

    /* Must contain an architecture */
    TT_ASSERT_TRUE(
        strstr(name, "x86_64") != NULL ||
        strstr(name, "aarch64") != NULL ||
        strstr(name, "armv7") != NULL);
}

TT_TEST(test_platform_binary_name_stable)
{
    /* Calling it twice returns the same pointer (static string) */
    const char *a = tt_update_platform_binary_name();
    const char *b = tt_update_platform_binary_name();
    TT_ASSERT_TRUE(a == b);
}

/* ================================================================
 * tt_self_exe_path
 * ================================================================ */

TT_TEST(test_self_exe_path_returns_valid)
{
    char *path = tt_self_exe_path();
#ifndef TT_PLATFORM_WINDOWS
    TT_ASSERT_NOT_NULL(path);
    if (path) {
        TT_ASSERT_TRUE(strlen(path) > 0);
        TT_ASSERT_TRUE(tt_file_exists(path));
        TT_ASSERT_TRUE(tt_is_file(path));
        /* Should be an absolute path */
        TT_ASSERT_TRUE(path[0] == '/');
    }
#endif
    free(path);
}

TT_TEST(test_self_exe_path_is_absolute)
{
    char *path = tt_self_exe_path();
    if (path) {
        TT_ASSERT_TRUE(tt_path_is_absolute(path));
        free(path);
    }
}

/* ================================================================
 * tt_rename_file edge cases
 * ================================================================ */

TT_TEST(test_rename_basic)
{
    const char *src = "/tmp/tt_rename_basic_src";
    const char *dst = "/tmp/tt_rename_basic_dst";

    tt_write_file(src, "data", 4);
    TT_ASSERT_TRUE(tt_file_exists(src));

    int rc = tt_rename_file(src, dst);
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_FALSE(tt_file_exists(src));
    TT_ASSERT_TRUE(tt_file_exists(dst));

    size_t len = 0;
    char *content = tt_read_file(dst, &len);
    TT_ASSERT_EQ_INT(4, (int)len);
    TT_ASSERT_EQ_STR("data", content);
    free(content);
    tt_remove_file(dst);
}

TT_TEST(test_rename_overwrite_existing)
{
    const char *src = "/tmp/tt_rename_ow_src";
    const char *dst = "/tmp/tt_rename_ow_dst";

    tt_write_file(dst, "old_content", 11);
    tt_write_file(src, "new_content", 11);

    int rc = tt_rename_file(src, dst);
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_FALSE(tt_file_exists(src));

    size_t len = 0;
    char *content = tt_read_file(dst, &len);
    TT_ASSERT_EQ_STR("new_content", content);
    free(content);
    tt_remove_file(dst);
}

TT_TEST(test_rename_null_args)
{
    TT_ASSERT_EQ_INT(-1, tt_rename_file(NULL, "/tmp/dst"));
    TT_ASSERT_EQ_INT(-1, tt_rename_file("/tmp/src", NULL));
    TT_ASSERT_EQ_INT(-1, tt_rename_file(NULL, NULL));
}

TT_TEST(test_rename_nonexistent_source)
{
    /* Renaming a file that doesn't exist should fail */
    int rc = tt_rename_file("/tmp/tt_does_not_exist_82719", "/tmp/tt_rename_dst_x");
    TT_ASSERT_EQ_INT(-1, rc);
}

TT_TEST(test_rename_to_nonexistent_dir)
{
    /* Renaming to a path where the parent dir doesn't exist should fail */
    const char *src = "/tmp/tt_rename_nodir_src";
    tt_write_file(src, "x", 1);

    int rc = tt_rename_file(src, "/tmp/tt_nonexistent_dir_82719/file");
    TT_ASSERT_EQ_INT(-1, rc);

    /* Source should still exist */
    TT_ASSERT_TRUE(tt_file_exists(src));
    tt_remove_file(src);
}

TT_TEST(test_rename_empty_file)
{
    const char *src = "/tmp/tt_rename_empty_src";
    const char *dst = "/tmp/tt_rename_empty_dst";

    tt_write_file(src, "", 0);
    TT_ASSERT_TRUE(tt_file_exists(src));

    int rc = tt_rename_file(src, dst);
    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT_TRUE(tt_file_exists(dst));

    int64_t sz = tt_file_size(dst);
    TT_ASSERT_EQ_INT(0, (int)sz);

    tt_remove_file(dst);
}

/* ================================================================
 * tt_file_set_executable
 * ================================================================ */

TT_TEST(test_file_set_executable_basic)
{
#ifndef TT_PLATFORM_WINDOWS
    const char *path = "/tmp/tt_chmod_test";
    tt_write_file(path, "#!/bin/sh\necho hi\n", 18);

    int rc = tt_file_set_executable(path);
    TT_ASSERT_EQ_INT(0, rc);

    /* Verify mode bits via stat */
    struct stat st;
    TT_ASSERT_EQ_INT(0, stat(path, &st));
    TT_ASSERT_TRUE((st.st_mode & S_IXUSR) != 0);
    TT_ASSERT_TRUE((st.st_mode & S_IXGRP) != 0);
    TT_ASSERT_TRUE((st.st_mode & S_IXOTH) != 0);

    tt_remove_file(path);
#endif
}

TT_TEST(test_file_set_executable_null)
{
    int rc = tt_file_set_executable(NULL);
    TT_ASSERT_EQ_INT(-1, rc);
}

TT_TEST(test_file_set_executable_nonexistent)
{
#ifndef TT_PLATFORM_WINDOWS
    int rc = tt_file_set_executable("/tmp/tt_chmod_nonexistent_82719");
    TT_ASSERT_EQ_INT(-1, rc);
#endif
}

/* ================================================================
 * Version cache I/O (unit-testable parts)
 * ================================================================ */

TT_TEST(test_update_check_no_crash_without_network)
{
    /* tt_update_check should never crash, even without network.
     * It may return {NULL, false} or a cached value. */
    tt_update_info_t info = tt_update_check();
    /* Just verify it didn't crash and the struct is valid */
    if (info.upstream_version) {
        TT_ASSERT_TRUE(strlen(info.upstream_version) > 0);
    }
    /* update_available must be false if upstream == current */
    if (info.upstream_version &&
        tt_semver_compare(TT_VERSION, info.upstream_version) >= 0) {
        TT_ASSERT_FALSE(info.update_available);
    }
    tt_update_info_free(&info);
}

TT_TEST(test_update_info_free_null)
{
    /* Should not crash on NULL */
    tt_update_info_free(NULL);

    /* Should not crash on zeroed struct */
    tt_update_info_t info = {NULL, false};
    tt_update_info_free(&info);
    TT_ASSERT_NULL(info.upstream_version);
    TT_ASSERT_FALSE(info.update_available);
}

TT_TEST(test_update_info_free_double)
{
    /* Double free safety: after free, upstream_version is NULL */
    tt_update_info_t info;
    info.upstream_version = strdup("1.0.0");
    info.update_available = true;

    tt_update_info_free(&info);
    TT_ASSERT_NULL(info.upstream_version);
    TT_ASSERT_FALSE(info.update_available);

    /* Second free should be safe (NULL pointer) */
    tt_update_info_free(&info);
    TT_ASSERT_NULL(info.upstream_version);
}

TT_TEST(test_build_result_with_newer_version)
{
    /* Simulate what build_result does: if upstream > current, update_available = true.
     * We test this indirectly through the semver compare. */
    TT_ASSERT_TRUE(tt_semver_compare("1.0.0", "99.0.0") < 0);
}

TT_TEST(test_build_result_same_version)
{
    /* If upstream == current, update_available should be false */
    TT_ASSERT_FALSE(tt_semver_compare(TT_VERSION, TT_VERSION) < 0);
}

TT_TEST(test_build_result_older_upstream)
{
    /* If upstream < current (e.g., downgrade scenario), update_available = false */
    TT_ASSERT_FALSE(tt_semver_compare("2.0.0", "1.0.0") < 0);
}

/* ================================================================
 * Cache file simulation (write, read back, validate)
 * ================================================================ */

TT_TEST(test_cache_file_with_trailing_newline)
{
    /* VERSION files typically have a trailing newline: "1.2.3\n" */
    const char *path = "/tmp/tt_test_version_nl";
    tt_write_file(path, "1.2.3\n", 6);

    size_t len = 0;
    char *content = tt_read_file(path, &len);
    TT_ASSERT_NOT_NULL(content);

    /* Simulate what read_cached_version does */
    /* trim trailing newline/space */
    while (len > 0 && (content[len - 1] == '\n' || content[len - 1] == '\r' ||
                        content[len - 1] == ' ')) {
        content[--len] = '\0';
    }
    TT_ASSERT_EQ_STR("1.2.3", content);
    free(content);
    tt_remove_file(path);
}

TT_TEST(test_cache_file_with_crlf)
{
    /* Windows-style line endings */
    const char *path = "/tmp/tt_test_version_crlf";
    tt_write_file(path, "1.2.3\r\n", 7);

    size_t len = 0;
    char *content = tt_read_file(path, &len);
    TT_ASSERT_NOT_NULL(content);
    while (len > 0 && (content[len - 1] == '\n' || content[len - 1] == '\r' ||
                        content[len - 1] == ' ')) {
        content[--len] = '\0';
    }
    TT_ASSERT_EQ_STR("1.2.3", content);
    free(content);
    tt_remove_file(path);
}

TT_TEST(test_cache_file_empty)
{
    /* Empty file should be treated as "no version available" */
    const char *path = "/tmp/tt_test_version_empty";
    tt_write_file(path, "", 0);

    size_t len = 0;
    char *content = tt_read_file(path, &len);
    /* tt_read_file may return NULL for empty file, or content with len=0 */
    TT_ASSERT_TRUE(content == NULL || len == 0);
    free(content);
    tt_remove_file(path);
}

TT_TEST(test_cache_file_garbage)
{
    /* Garbage content should not cause update_available to be true */
    const char *garbage = "not-a-version-string!!!\n<html>404</html>\n";
    /* tt_semver_compare("1.0.0", "not-a-version-string!!!") should return 0 */
    TT_ASSERT_EQ_INT(0, tt_semver_compare("1.0.0", garbage));
    /* So update_available would be false (compare >= 0) */
    TT_ASSERT_FALSE(tt_semver_compare(TT_VERSION, garbage) < 0);
}

TT_TEST(test_cache_file_html_404)
{
    /* If GitHub returns a 404 HTML page instead of the VERSION file,
     * curl -f should fail (exit code 22), but if somehow we get HTML... */
    const char *html = "<html><body>Not Found</body></html>";
    TT_ASSERT_EQ_INT(0, tt_semver_compare(TT_VERSION, html));
    TT_ASSERT_FALSE(tt_semver_compare(TT_VERSION, html) < 0);
}

/* ================================================================
 * SHA256 verification (testing the hash infrastructure)
 * ================================================================ */

TT_TEST(test_sha256_file_known_content)
{
    const char *path = "/tmp/tt_test_sha256_content";
    tt_write_file(path, "toktoken", 8);

    char *hash = tt_sha256_file(path);
    TT_ASSERT_NOT_NULL(hash);
    if (hash) {
        /* Must be 64 hex characters */
        TT_ASSERT_EQ_INT(64, (int)strlen(hash));
        /* All characters must be hex */
        for (int i = 0; i < 64; i++) {
            char c = hash[i];
            TT_ASSERT_TRUE(
                (c >= '0' && c <= '9') ||
                (c >= 'a' && c <= 'f') ||
                (c >= 'A' && c <= 'F'));
        }
    }
    free(hash);
    tt_remove_file(path);
}

TT_TEST(test_sha256_file_nonexistent)
{
    char *hash = tt_sha256_file("/tmp/tt_sha256_nonexistent_82719");
    TT_ASSERT_NULL(hash);
}

TT_TEST(test_sha256_file_consistency)
{
    /* Same content should produce same hash */
    const char *path1 = "/tmp/tt_sha256_c1";
    const char *path2 = "/tmp/tt_sha256_c2";
    tt_write_file(path1, "same content", 12);
    tt_write_file(path2, "same content", 12);

    char *h1 = tt_sha256_file(path1);
    char *h2 = tt_sha256_file(path2);
    TT_ASSERT_NOT_NULL(h1);
    TT_ASSERT_NOT_NULL(h2);
    TT_ASSERT_EQ_STR(h1, h2);

    free(h1);
    free(h2);
    tt_remove_file(path1);
    tt_remove_file(path2);
}

TT_TEST(test_sha256_file_different_content)
{
    /* Different content should produce different hash */
    const char *path1 = "/tmp/tt_sha256_d1";
    const char *path2 = "/tmp/tt_sha256_d2";
    tt_write_file(path1, "content A", 9);
    tt_write_file(path2, "content B", 9);

    char *h1 = tt_sha256_file(path1);
    char *h2 = tt_sha256_file(path2);
    TT_ASSERT_NOT_NULL(h1);
    TT_ASSERT_NOT_NULL(h2);
    TT_ASSERT_TRUE(strcmp(h1, h2) != 0);

    free(h1);
    free(h2);
    tt_remove_file(path1);
    tt_remove_file(path2);
}

/* ================================================================
 * Storage paths (cache directory exists)
 * ================================================================ */

TT_TEST(test_storage_base_dir_exists)
{
    /* tt_storage_base_dir should return a non-NULL path */
    char *base = tt_storage_base_dir();
    TT_ASSERT_NOT_NULL(base);
    if (base) {
        TT_ASSERT_TRUE(strlen(base) > 0);
        TT_ASSERT_STR_CONTAINS(base, "toktoken");
    }
    free(base);
}

/* ================================================================
 * Runner
 * ================================================================ */

void run_update_check_tests(void)
{
    /* Semver comparison */
    TT_RUN(test_semver_equal);
    TT_RUN(test_semver_less);
    TT_RUN(test_semver_greater);
    TT_RUN(test_semver_malformed);
    TT_RUN(test_semver_partial);
    TT_RUN(test_semver_with_whitespace_prefix);
    TT_RUN(test_semver_large_numbers);
    TT_RUN(test_semver_negative_numbers);

    /* Platform binary name */
    TT_RUN(test_platform_binary_name_format);
    TT_RUN(test_platform_binary_name_stable);

    /* Self-exe path */
    TT_RUN(test_self_exe_path_returns_valid);
    TT_RUN(test_self_exe_path_is_absolute);

    /* Atomic rename */
    TT_RUN(test_rename_basic);
    TT_RUN(test_rename_overwrite_existing);
    TT_RUN(test_rename_null_args);
    TT_RUN(test_rename_nonexistent_source);
    TT_RUN(test_rename_to_nonexistent_dir);
    TT_RUN(test_rename_empty_file);

    /* chmod */
    TT_RUN(test_file_set_executable_basic);
    TT_RUN(test_file_set_executable_null);
    TT_RUN(test_file_set_executable_nonexistent);

    /* Update check resilience */
    TT_RUN(test_update_check_no_crash_without_network);
    TT_RUN(test_update_info_free_null);
    TT_RUN(test_update_info_free_double);
    TT_RUN(test_build_result_with_newer_version);
    TT_RUN(test_build_result_same_version);
    TT_RUN(test_build_result_older_upstream);

    /* Cache file edge cases */
    TT_RUN(test_cache_file_with_trailing_newline);
    TT_RUN(test_cache_file_with_crlf);
    TT_RUN(test_cache_file_empty);
    TT_RUN(test_cache_file_garbage);
    TT_RUN(test_cache_file_html_404);

    /* SHA256 verification */
    TT_RUN(test_sha256_file_known_content);
    TT_RUN(test_sha256_file_nonexistent);
    TT_RUN(test_sha256_file_consistency);
    TT_RUN(test_sha256_file_different_content);

    /* Storage paths */
    TT_RUN(test_storage_base_dir_exists);
}
