/*
 * test_path_validator.c -- Unit tests for path_validator module.
 */

#include "test_framework.h"
#include "test_helpers.h"
#include "path_validator.h"
#include "platform.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#ifndef TT_PLATFORM_WINDOWS
#include <unistd.h>
#endif

TT_TEST(test_pv_validate_same_directory)
{
    TT_ASSERT_TRUE(tt_path_validate("/tmp", "/tmp"));
}

TT_TEST(test_pv_validate_inside_root)
{
    char *tmpdir = tt_test_tmpdir();
    TT_ASSERT_NOT_NULL(tmpdir);

    TT_ASSERT_TRUE(tt_path_validate(tmpdir, "/tmp"));

    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_pv_validate_outside_root)
{
    TT_ASSERT_FALSE(tt_path_validate("/etc", "/tmp"));
}

TT_TEST(test_pv_validate_nonexistent_path)
{
    TT_ASSERT_FALSE(tt_path_validate("/nonexistent_path_xyz_test", "/tmp"));
}

TT_TEST(test_pv_validate_traversal_attempt)
{
    TT_ASSERT_FALSE(tt_path_validate("/tmp/../etc/passwd", "/tmp"));
}

TT_TEST(test_pv_validate_subdirectory)
{
    char *tmpdir = tt_test_tmpdir();
    TT_ASSERT_NOT_NULL(tmpdir);

    char subdir[512];
    snprintf(subdir, sizeof(subdir), "%s/sub", tmpdir);
    tt_mkdir_p(subdir);

    TT_ASSERT_TRUE(tt_path_validate(subdir, "/tmp"));

    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_pv_validate_both_nonexistent)
{
    TT_ASSERT_FALSE(tt_path_validate("/nonexistent_a", "/nonexistent_b"));
}

TT_TEST(test_pv_validate_root_nonexistent)
{
    TT_ASSERT_FALSE(tt_path_validate("/tmp", "/nonexistent_root_test"));
}

TT_TEST(test_pv_validate_null_path)
{
    TT_ASSERT_FALSE(tt_path_validate(NULL, "/tmp"));
}

TT_TEST(test_pv_validate_null_root)
{
    TT_ASSERT_FALSE(tt_path_validate("/tmp", NULL));
}

TT_TEST(test_pv_symlink_escape_regular_file)
{
    char *tmpdir = tt_test_tmpdir();
    TT_ASSERT_NOT_NULL(tmpdir);

    tt_test_write_file(tmpdir, "regular.txt", "test");
    char path[512];
    snprintf(path, sizeof(path), "%s/regular.txt", tmpdir);

    TT_ASSERT_FALSE(tt_is_symlink_escape(path, tmpdir));

    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_pv_symlink_escape_nonexistent)
{
    TT_ASSERT_FALSE(tt_is_symlink_escape("/nonexistent_xyz", "/tmp"));
}

#ifndef TT_PLATFORM_WINDOWS
TT_TEST(test_pv_symlink_inside_root)
{
    char *tmpdir = tt_test_tmpdir();
    TT_ASSERT_NOT_NULL(tmpdir);

    tt_test_write_file(tmpdir, "target.txt", "test");

    char target[512], link_path[512];
    snprintf(target, sizeof(target), "%s/target.txt", tmpdir);
    snprintf(link_path, sizeof(link_path), "%s/link.txt", tmpdir);

    if (symlink(target, link_path) != 0) {
        /* Cannot create symlinks -- skip */
        tt_test_rmdir(tmpdir);
        free(tmpdir);
        return;
    }

    TT_ASSERT_FALSE(tt_is_symlink_escape(link_path, tmpdir));

    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_pv_symlink_outside_root)
{
    char *tmpdir = tt_test_tmpdir();
    char *otherdir = tt_test_tmpdir();
    TT_ASSERT_NOT_NULL(tmpdir);
    TT_ASSERT_NOT_NULL(otherdir);

    tt_test_write_file(otherdir, "outside.txt", "test");

    char target[512], link_path[512];
    snprintf(target, sizeof(target), "%s/outside.txt", otherdir);
    snprintf(link_path, sizeof(link_path), "%s/escape.txt", tmpdir);

    if (symlink(target, link_path) != 0) {
        /* Cannot create symlinks -- skip */
        tt_test_rmdir(tmpdir);
        tt_test_rmdir(otherdir);
        free(tmpdir);
        free(otherdir);
        return;
    }

    TT_ASSERT_TRUE(tt_is_symlink_escape(link_path, tmpdir));

    tt_test_rmdir(tmpdir);
    tt_test_rmdir(otherdir);
    free(tmpdir);
    free(otherdir);
}
#endif /* !TT_PLATFORM_WINDOWS */

void run_path_validator_tests(void)
{
    TT_RUN(test_pv_validate_same_directory);
    TT_RUN(test_pv_validate_inside_root);
    TT_RUN(test_pv_validate_outside_root);
    TT_RUN(test_pv_validate_nonexistent_path);
    TT_RUN(test_pv_validate_traversal_attempt);
    TT_RUN(test_pv_validate_subdirectory);
    TT_RUN(test_pv_validate_both_nonexistent);
    TT_RUN(test_pv_validate_root_nonexistent);
    TT_RUN(test_pv_validate_null_path);
    TT_RUN(test_pv_validate_null_root);
    TT_RUN(test_pv_symlink_escape_regular_file);
    TT_RUN(test_pv_symlink_escape_nonexistent);
#ifndef TT_PLATFORM_WINDOWS
    TT_RUN(test_pv_symlink_inside_root);
    TT_RUN(test_pv_symlink_outside_root);
#endif
}
