/*
 * test_edge_cases.c -- Edge case tests for robustness.
 *
 * Tests for unreadable files, binary files, long paths,
 * non-UTF8 filenames, and permission issues.
 *
 * Ref: PRD Phase 12.10
 */

#include "test_framework.h"
#include "text_search.h"
#include "test_helpers.h"
#include "file_filter.h"
#include "platform.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int dp_has_path(const tt_discovered_paths_t *dp, const char *substr)
{
    for (int i = 0; i < dp->count; i++) {
        if (strstr(dp->paths[i], substr)) return 1;
    }
    return 0;
}

/* --- Binary file exclusion --- */

TT_TEST(test_edge_binary_file_excluded)
{
    char *tmpdir = tt_test_tmpdir();
    if (!tmpdir) return;

    /* Create a binary file (ELF magic) */
    tt_test_write_file(tmpdir, "binary.dat",
                        "\x7f" "ELF\x00\x00\x00\x00");
    /* Create a normal source file */
    tt_test_write_file(tmpdir, "hello.php",
                        "<?php echo 'hello';");

    tt_file_filter_t ff;
    tt_file_filter_init(&ff, 0, NULL, false, NULL);
    tt_discovered_paths_t dp = {0};
    tt_discover_paths(tmpdir, &ff, &dp);

    TT_ASSERT(dp_has_path(&dp, "hello.php"), "should discover .php file");
    TT_ASSERT(!dp_has_path(&dp, "binary.dat"), "should exclude binary file");

    tt_discovered_paths_free(&dp);
    tt_file_filter_free(&ff);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

/* --- Unreadable file does not crash indexing --- */

#ifndef TT_PLATFORM_WINDOWS
TT_TEST(test_edge_unreadable_file_skipped)
{
    if (getuid() == 0) return; /* Skip if root */

    char *tmpdir = tt_test_tmpdir();
    if (!tmpdir) return;

    tt_test_write_file(tmpdir, "readable.php",
                        "<?php class Foo {}");
    tt_test_write_file(tmpdir, "noperm.php",
                        "<?php class Bar {}");

    /* Remove read permission */
    char path[512];
    snprintf(path, sizeof(path), "%s/noperm.php", tmpdir);
    chmod(path, 0000);

    tt_file_filter_t ff;
    tt_file_filter_init(&ff, 0, NULL, false, NULL);
    tt_discovered_paths_t dp = {0};
    int rc = tt_discover_paths(tmpdir, &ff, &dp);

    /* Should not crash */
    TT_ASSERT_EQ_INT(0, rc);

    tt_discovered_paths_free(&dp);
    tt_file_filter_free(&ff);

    /* Restore permissions for cleanup */
    chmod(path, 0644);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}
#endif /* !TT_PLATFORM_WINDOWS */

/* --- Long path handling --- */

TT_TEST(test_edge_long_path_no_crash)
{
    char *tmpdir = tt_test_tmpdir();
    if (!tmpdir) return;

    /* Build a deeply nested path */
    char nested[512];
    snprintf(nested, sizeof(nested),
             "a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p/deep.php");
    tt_test_write_file(tmpdir, nested, "<?php class Deep {}");

    tt_file_filter_t ff;
    tt_file_filter_init(&ff, 0, NULL, false, NULL);
    tt_discovered_paths_t dp = {0};
    int rc = tt_discover_paths(tmpdir, &ff, &dp);

    TT_ASSERT_EQ_INT(0, rc);
    TT_ASSERT(dp_has_path(&dp, "deep.php"), "should discover deeply nested file");

    tt_discovered_paths_free(&dp);
    tt_file_filter_free(&ff);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

/* --- Empty file does not crash --- */

TT_TEST(test_edge_empty_file_no_crash)
{
    char *tmpdir = tt_test_tmpdir();
    if (!tmpdir) return;

    tt_test_write_file(tmpdir, "empty.php", "");

    tt_file_filter_t ff;
    tt_file_filter_init(&ff, 0, NULL, false, NULL);
    tt_discovered_paths_t dp = {0};
    int rc = tt_discover_paths(tmpdir, &ff, &dp);

    TT_ASSERT_EQ_INT(0, rc);
    /* Empty file may or may not be discovered, but no crash */

    tt_discovered_paths_free(&dp);
    tt_file_filter_free(&ff);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

/* --- Non-UTF8 filename handling --- */

TT_TEST(test_edge_non_utf8_filename_no_crash)
{
    char *tmpdir = tt_test_tmpdir();
    if (!tmpdir) return;

    /* Create file with non-UTF8 bytes in name */
    char badname[64];
    snprintf(badname, sizeof(badname), "file_\xff\xfe.php");
    tt_test_write_file(tmpdir, badname, "<?php class Bad {}");

    tt_file_filter_t ff;
    tt_file_filter_init(&ff, 0, NULL, false, NULL);
    tt_discovered_paths_t dp = {0};
    int rc = tt_discover_paths(tmpdir, &ff, &dp);

    /* Must not crash regardless of whether file is included */
    TT_ASSERT_EQ_INT(0, rc);

    tt_discovered_paths_free(&dp);
    tt_file_filter_free(&ff);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

/* --- Image file excluded --- */

TT_TEST(test_edge_image_file_excluded)
{
    char *tmpdir = tt_test_tmpdir();
    if (!tmpdir) return;

    /* PNG header bytes */
    tt_test_write_file(tmpdir, "logo.png",
                        "\x89PNG\r\n\x1a\n");
    tt_test_write_file(tmpdir, "app.js",
                        "function app() {}");

    tt_file_filter_t ff;
    tt_file_filter_init(&ff, 0, NULL, false, NULL);
    tt_discovered_paths_t dp = {0};
    tt_discover_paths(tmpdir, &ff, &dp);

    TT_ASSERT(dp_has_path(&dp, "app.js"), "should discover .js file");
    TT_ASSERT(!dp_has_path(&dp, "logo.png"), "should exclude .png image");

    tt_discovered_paths_free(&dp);
    tt_file_filter_free(&ff);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

/* ---- ReDoS protection tests ---- */

TT_TEST(test_regex_validate_safe_patterns)
{
    TT_ASSERT_NULL(tt_regex_validate("hello"));
    TT_ASSERT_NULL(tt_regex_validate("foo|bar"));
    TT_ASSERT_NULL(tt_regex_validate("[a-z]+"));
    TT_ASSERT_NULL(tt_regex_validate("\\d{3}-\\d{4}"));
    TT_ASSERT_NULL(tt_regex_validate("(foo)(bar)"));
    TT_ASSERT_NULL(tt_regex_validate("a{1,3}"));
}

TT_TEST(test_regex_validate_nested_quantifiers)
{
    /* (a+)+ is classic ReDoS */
    TT_ASSERT_NOT_NULL(tt_regex_validate("(a+)+"));
    /* (a+)* is also dangerous */
    TT_ASSERT_NOT_NULL(tt_regex_validate("(a+)*"));
    /* (a*)+  */
    TT_ASSERT_NOT_NULL(tt_regex_validate("(a*)+"));
    /* (?:a+){2,} */
    TT_ASSERT_NOT_NULL(tt_regex_validate("(?:a+){2,}"));
}

TT_TEST(test_regex_validate_length_limit)
{
    /* 200 chars should be OK */
    char buf[256];
    memset(buf, 'a', 200);
    buf[200] = '\0';
    TT_ASSERT_NULL(tt_regex_validate(buf));

    /* 201 chars should be rejected */
    memset(buf, 'a', 201);
    buf[201] = '\0';
    TT_ASSERT_NOT_NULL(tt_regex_validate(buf));
}

TT_TEST(test_regex_validate_empty)
{
    TT_ASSERT_NOT_NULL(tt_regex_validate(""));
    TT_ASSERT_NOT_NULL(tt_regex_validate(NULL));
}

void run_edge_case_tests(void)
{
    TT_RUN(test_edge_binary_file_excluded);
#ifndef TT_PLATFORM_WINDOWS
    TT_RUN(test_edge_unreadable_file_skipped);
#endif
    TT_RUN(test_edge_long_path_no_crash);
    TT_RUN(test_edge_empty_file_no_crash);
    TT_RUN(test_edge_non_utf8_filename_no_crash);
    TT_RUN(test_edge_image_file_excluded);
    TT_RUN(test_regex_validate_safe_patterns);
    TT_RUN(test_regex_validate_nested_quantifiers);
    TT_RUN(test_regex_validate_length_limit);
    TT_RUN(test_regex_validate_empty);
}
