/*
 * test_output_fmt.c -- Unit tests for output_fmt module.
 */

#include "test_framework.h"
#include "output_fmt.h"

#include <stdlib.h>
#include <string.h>

/* ---- matchesPathFilters ---- */

TT_TEST(test_ofmt_no_filters_matches_everything)
{
    TT_ASSERT_TRUE(tt_matches_path_filters("src/Auth/Controller.php", NULL, NULL));
}

TT_TEST(test_ofmt_filter_includes_matching)
{
    TT_ASSERT_TRUE(tt_matches_path_filters("src/Auth/Controller.php", "Auth", NULL));
}

TT_TEST(test_ofmt_filter_excludes_non_matching)
{
    TT_ASSERT_FALSE(tt_matches_path_filters("src/Cache/Store.php", "Auth", NULL));
}

TT_TEST(test_ofmt_filter_case_insensitive)
{
    TT_ASSERT_TRUE(tt_matches_path_filters("src/AUTH/Controller.php", "auth", NULL));
}

TT_TEST(test_ofmt_filter_pipe_separated_or)
{
    TT_ASSERT_TRUE(tt_matches_path_filters("src/Auth/Controller.php", "Auth|Cache", NULL));
    TT_ASSERT_TRUE(tt_matches_path_filters("src/Cache/Store.php", "Auth|Cache", NULL));
    TT_ASSERT_FALSE(tt_matches_path_filters("src/Models/User.php", "Auth|Cache", NULL));
}

TT_TEST(test_ofmt_exclude_removes_matching)
{
    TT_ASSERT_FALSE(tt_matches_path_filters("vendor/autoload.php", NULL, "vendor"));
    TT_ASSERT_TRUE(tt_matches_path_filters("src/App.php", NULL, "vendor"));
}

TT_TEST(test_ofmt_exclude_pipe_separated_or)
{
    TT_ASSERT_FALSE(tt_matches_path_filters("vendor/package.php", NULL, "vendor|test"));
    TT_ASSERT_FALSE(tt_matches_path_filters("tests/Unit/FooTest.php", NULL, "vendor|test"));
    TT_ASSERT_TRUE(tt_matches_path_filters("src/App.php", NULL, "vendor|test"));
}

TT_TEST(test_ofmt_filter_and_exclude_combined)
{
    TT_ASSERT_TRUE(tt_matches_path_filters("src/App.php", "src", "test"));
    TT_ASSERT_FALSE(tt_matches_path_filters("src/tests/FooTest.php", "src", "test"));
    TT_ASSERT_FALSE(tt_matches_path_filters("lib/App.php", "src", "test"));
}

TT_TEST(test_ofmt_filter_empty_pipe_segments)
{
    TT_ASSERT_TRUE(tt_matches_path_filters("src/Auth/X.php", "Auth||Cache", NULL));
    TT_ASSERT_TRUE(tt_matches_path_filters("src/Cache/X.php", "Auth||Cache", NULL));
}

TT_TEST(test_ofmt_exclude_case_insensitive)
{
    TT_ASSERT_FALSE(tt_matches_path_filters("vendor/foo.php", NULL, "VENDOR"));
}

TT_TEST(test_ofmt_null_path_returns_false)
{
    TT_ASSERT_FALSE(tt_matches_path_filters(NULL, NULL, NULL));
}

/* ---- applyLimit ---- */

TT_TEST(test_ofmt_apply_limit_caps)
{
    int count = 10;
    tt_apply_limit(5, &count);
    TT_ASSERT_EQ_INT(5, count);
}

TT_TEST(test_ofmt_apply_limit_no_op_when_zero)
{
    int count = 10;
    tt_apply_limit(0, &count);
    TT_ASSERT_EQ_INT(10, count);
}

TT_TEST(test_ofmt_apply_limit_no_op_when_under)
{
    int count = 3;
    tt_apply_limit(10, &count);
    TT_ASSERT_EQ_INT(3, count);
}

TT_TEST(test_ofmt_apply_limit_null_count)
{
    /* Should not crash */
    tt_apply_limit(5, NULL);
}

/* ---------- tt_line_is_comment_or_blank (--no-comments) ---------- */

TT_TEST(test_comment_blank_lines)
{
    bool in_block = false;
    TT_ASSERT(tt_line_is_comment_or_blank("", &in_block), "empty is blank");
    TT_ASSERT(tt_line_is_comment_or_blank("   \t ", &in_block), "spaces are blank");
}

TT_TEST(test_comment_line_comments)
{
    bool in_block = false;
    TT_ASSERT(tt_line_is_comment_or_blank("// note", &in_block), "line comment");
    TT_ASSERT(tt_line_is_comment_or_blank("    // indented", &in_block), "indented");
    TT_ASSERT(!in_block, "line comment does not open a block");
}

TT_TEST(test_comment_block_spans_lines)
{
    bool in_block = false;
    TT_ASSERT(tt_line_is_comment_or_blank("/*", &in_block), "block start");
    TT_ASSERT(in_block, "state tracks the open block");
    TT_ASSERT(tt_line_is_comment_or_blank(" * continuation", &in_block), "body");
    TT_ASSERT(tt_line_is_comment_or_blank(" */", &in_block), "close marker");
    TT_ASSERT(!in_block, "state clears after the close marker");
    TT_ASSERT(!tt_line_is_comment_or_blank("int x;", &in_block), "code after block");
}

TT_TEST(test_comment_block_closes_on_one_line)
{
    bool in_block = false;
    TT_ASSERT(tt_line_is_comment_or_blank("/* short */", &in_block), "single line block");
    TT_ASSERT(!in_block, "state stays clear for a closed block");
}

TT_TEST(test_comment_code_lines_are_kept)
{
    bool in_block = false;
    TT_ASSERT(!tt_line_is_comment_or_blank("int x; /* trailing */", &in_block),
               "code with trailing comment");
    TT_ASSERT(!tt_line_is_comment_or_blank("    *p = 0;", &in_block),
               "dereference is not a comment");
    TT_ASSERT(!tt_line_is_comment_or_blank("#include <stdio.h>", &in_block),
               "preprocessor is not a comment");
}

void run_output_fmt_tests(void)
{
    TT_RUN(test_ofmt_no_filters_matches_everything);
    TT_RUN(test_ofmt_filter_includes_matching);
    TT_RUN(test_ofmt_filter_excludes_non_matching);
    TT_RUN(test_ofmt_filter_case_insensitive);
    TT_RUN(test_ofmt_filter_pipe_separated_or);
    TT_RUN(test_ofmt_exclude_removes_matching);
    TT_RUN(test_ofmt_exclude_pipe_separated_or);
    TT_RUN(test_ofmt_filter_and_exclude_combined);
    TT_RUN(test_ofmt_filter_empty_pipe_segments);
    TT_RUN(test_ofmt_exclude_case_insensitive);
    TT_RUN(test_ofmt_null_path_returns_false);
    TT_RUN(test_ofmt_apply_limit_caps);
    TT_RUN(test_ofmt_apply_limit_no_op_when_zero);
    TT_RUN(test_ofmt_apply_limit_no_op_when_under);
    TT_RUN(test_ofmt_apply_limit_null_count);
    TT_RUN(test_comment_blank_lines);
    TT_RUN(test_comment_line_comments);
    TT_RUN(test_comment_block_spans_lines);
    TT_RUN(test_comment_block_closes_on_one_line);
    TT_RUN(test_comment_code_lines_are_kept);
}
