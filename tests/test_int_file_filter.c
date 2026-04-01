/*
 * test_int_file_filter.c -- Integration tests for file_filter module.
 */

#include "test_framework.h"
#include "test_helpers.h"
#include "file_filter.h"
#include "secret_patterns.h"
#include "path_validator.h"

#include "platform.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int paths_contain(const tt_discovered_paths_t *dp, const char *path)
{
    for (int i = 0; i < dp->count; i++) {
        if (strcmp(dp->paths[i], path) == 0)
            return 1;
    }
    return 0;
}

static int paths_any_prefix(const tt_discovered_paths_t *dp, const char *prefix)
{
    size_t plen = strlen(prefix);
    for (int i = 0; i < dp->count; i++) {
        if (strncmp(dp->paths[i], prefix, plen) == 0)
            return 1;
    }
    return 0;
}

TT_TEST(test_int_ff_discover_includes_source)
{
    char *tmpdir = tt_test_tmpdir();
    TT_ASSERT_NOT_NULL(tmpdir);

    tt_test_write_file(tmpdir, "src/App.php", "<?php class App {}");
    tt_test_write_file(tmpdir, "src/index.js", "export default {};");

    tt_file_filter_t ff;
    tt_file_filter_init(&ff, 500, NULL, false, NULL);
    tt_discovered_paths_t dp = {0};
    tt_discover_paths(tmpdir, &ff, &dp);

    TT_ASSERT(paths_contain(&dp, "src/App.php"), "should contain src/App.php");
    TT_ASSERT(paths_contain(&dp, "src/index.js"), "should contain src/index.js");

    tt_discovered_paths_free(&dp);
    tt_file_filter_free(&ff);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_int_ff_discover_excludes_vendor)
{
    char *tmpdir = tt_test_tmpdir();
    TT_ASSERT_NOT_NULL(tmpdir);

    tt_test_write_file(tmpdir, "src/App.php", "<?php class App {}");
    tt_test_write_file(tmpdir, "vendor/autoload.php", "<?php // autoloader");

    tt_file_filter_t ff;
    tt_file_filter_init(&ff, 500, NULL, false, NULL);
    tt_discovered_paths_t dp = {0};
    tt_discover_paths(tmpdir, &ff, &dp);

    TT_ASSERT(!paths_any_prefix(&dp, "vendor/"), "should not contain vendor files");

    tt_discovered_paths_free(&dp);
    tt_file_filter_free(&ff);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_int_ff_discover_excludes_git)
{
    char *tmpdir = tt_test_tmpdir();
    TT_ASSERT_NOT_NULL(tmpdir);

    tt_test_write_file(tmpdir, "src/App.php", "<?php class App {}");
    tt_test_write_file(tmpdir, ".git/config", "[core]");

    tt_file_filter_t ff;
    tt_file_filter_init(&ff, 500, NULL, false, NULL);
    tt_discovered_paths_t dp = {0};
    tt_discover_paths(tmpdir, &ff, &dp);

    TT_ASSERT(!paths_any_prefix(&dp, ".git/"), "should not contain .git files");

    tt_discovered_paths_free(&dp);
    tt_file_filter_free(&ff);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_int_ff_discover_excludes_secrets)
{
    char *tmpdir = tt_test_tmpdir();
    TT_ASSERT_NOT_NULL(tmpdir);

    tt_test_write_file(tmpdir, "src/App.php", "<?php class App {}");
    tt_test_write_file(tmpdir, ".env", "SECRET=value");

    tt_file_filter_t ff;
    tt_file_filter_init(&ff, 500, NULL, false, NULL);
    tt_discovered_paths_t dp = {0};
    tt_discover_paths(tmpdir, &ff, &dp);

    TT_ASSERT(!paths_contain(&dp, ".env"), "should not contain .env");

    tt_discovered_paths_free(&dp);
    tt_file_filter_free(&ff);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_int_ff_toktokenignore_support)
{
    char *tmpdir = tt_test_tmpdir();
    TT_ASSERT_NOT_NULL(tmpdir);

    tt_test_write_file(tmpdir, "src/App.php", "<?php class App {}");
    tt_test_write_file(tmpdir, "src/index.js", "export default {};");
    tt_test_write_file(tmpdir, ".toktokenignore", "src/index.js\n");

    tt_file_filter_t ff;
    tt_file_filter_init(&ff, 500, NULL, false, NULL);

    char ignore_path[512];
    snprintf(ignore_path, sizeof(ignore_path), "%s/.toktokenignore", tmpdir);
    tt_file_filter_load_ignorefile(&ff, ignore_path, tmpdir);

    tt_discovered_paths_t dp = {0};
    tt_discover_paths(tmpdir, &ff, &dp);

    TT_ASSERT(paths_contain(&dp, "src/App.php"), "should contain src/App.php");
    TT_ASSERT(!paths_contain(&dp, "src/index.js"), "should not contain ignored src/index.js");

    tt_discovered_paths_free(&dp);
    tt_file_filter_free(&ff);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

/* ---- Legacy edge-case tests ---- */

TT_TEST(test_int_ff_edge_binary)
{
    char *tmpdir = tt_test_tmpdir();
    TT_ASSERT_NOT_NULL(tmpdir);

    /* Write a binary file with a null byte at position 50 */
    char binpath[2048];
    snprintf(binpath, sizeof(binpath), "%s/src/binary.c", tmpdir);
    tt_mkdir_p(tmpdir);
    {
        char parent[2048];
        snprintf(parent, sizeof(parent), "%s/src", tmpdir);
        tt_mkdir_p(parent);
    }
    {
        char data[128];
        memset(data, 'A', sizeof(data));
        data[50] = '\0'; /* null byte makes it binary */
        tt_write_file(binpath, data, sizeof(data));
    }
    tt_test_write_file(tmpdir, "src/normal.c", "int main() { return 0; }");

    tt_file_filter_t ff;
    tt_file_filter_init(&ff, 0, NULL, false, NULL);
    tt_discovered_paths_t dp = {0};
    tt_discover_paths(tmpdir, &ff, &dp);

    /* Discovery no longer checks binary content; both files are discovered.
     * Binary check is deferred to the worker. */
    TT_ASSERT_EQ_INT(dp.count, 2);

    tt_discovered_paths_free(&dp);
    tt_file_filter_free(&ff);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_int_ff_edge_empty)
{
    char *tmpdir = tt_test_tmpdir();
    TT_ASSERT_NOT_NULL(tmpdir);

    tt_test_write_file(tmpdir, "src/empty.c", "");

    tt_file_filter_t ff;
    tt_file_filter_init(&ff, 0, NULL, false, NULL);
    tt_discovered_paths_t dp = {0};
    tt_discover_paths(tmpdir, &ff, &dp);

    TT_ASSERT_EQ_INT(dp.count, 1);

    tt_discovered_paths_free(&dp);
    tt_file_filter_free(&ff);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_int_ff_edge_size_limit)
{
    char *tmpdir = tt_test_tmpdir();
    TT_ASSERT_NOT_NULL(tmpdir);

    /* Create a file larger than 1 KB */
    char big[2048];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    tt_test_write_file(tmpdir, "src/big.c", big);
    tt_test_write_file(tmpdir, "src/small.c", "int x;");

    tt_file_filter_t ff;
    tt_file_filter_init(&ff, 1, NULL, false, NULL); /* 1 KB limit */
    tt_discovered_paths_t dp = {0};
    tt_discover_paths(tmpdir, &ff, &dp);

    /* Discovery no longer checks file size; both files are discovered.
     * Size check is deferred to the worker. */
    TT_ASSERT_EQ_INT(dp.count, 2);

    tt_discovered_paths_free(&dp);
    tt_file_filter_free(&ff);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_int_ff_edge_mixed_case_ext)
{
    char *tmpdir = tt_test_tmpdir();
    TT_ASSERT_NOT_NULL(tmpdir);

    tt_test_write_file(tmpdir, "src/App.PHP", "<?php ?>");
    tt_test_write_file(tmpdir, "src/helper.Js", "function(){}");

    tt_file_filter_t ff;
    tt_file_filter_init(&ff, 0, NULL, false, NULL);
    tt_discovered_paths_t dp = {0};
    tt_discover_paths(tmpdir, &ff, &dp);

    TT_ASSERT_EQ_INT(dp.count, 2);

    tt_discovered_paths_free(&dp);
    tt_file_filter_free(&ff);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_int_ff_edge_rebuild_dir)
{
    char *tmpdir = tt_test_tmpdir();
    TT_ASSERT_NOT_NULL(tmpdir);

    /* "rebuild" is NOT a SKIP_DIR, "build" IS */
    tt_test_write_file(tmpdir, "rebuild/app.c", "int main() {}");
    tt_test_write_file(tmpdir, "build/app.c", "int main() {}");

    tt_file_filter_t ff;
    tt_file_filter_init(&ff, 0, NULL, false, NULL);
    tt_discovered_paths_t dp = {0};
    tt_discover_paths(tmpdir, &ff, &dp);

    TT_ASSERT_EQ_INT(dp.count, 1);
    if (dp.count == 1) {
        TT_ASSERT_EQ_STR(dp.paths[0], "rebuild/app.c");
    }

    tt_discovered_paths_free(&dp);
    tt_file_filter_free(&ff);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_int_ff_segment_matching)
{
    char *tmpdir = tt_test_tmpdir();
    TT_ASSERT_NOT_NULL(tmpdir);

    /* build/foo.c -> skipped (build is SKIP_DIR) */
    tt_test_write_file(tmpdir, "build/foo.c", "int x;");
    /* src/build/foo.c -> skipped (segment build in path) */
    tt_test_write_file(tmpdir, "src/build/foo.c", "int x;");
    /* rebuild/foo.c -> NOT skipped */
    tt_test_write_file(tmpdir, "rebuild/foo.c", "int x;");
    /* src/rebuilder/foo.c -> NOT skipped */
    tt_test_write_file(tmpdir, "src/rebuilder/foo.c", "int x;");

    tt_file_filter_t ff;
    tt_file_filter_init(&ff, 0, NULL, false, NULL);
    tt_discovered_paths_t dp = {0};
    tt_discover_paths(tmpdir, &ff, &dp);

    TT_ASSERT_EQ_INT(dp.count, 2);

    int found_rebuild = 0, found_rebuilder = 0;
    for (int i = 0; i < dp.count; i++) {
        if (strcmp(dp.paths[i], "rebuild/foo.c") == 0) found_rebuild = 1;
        if (strcmp(dp.paths[i], "src/rebuilder/foo.c") == 0) found_rebuilder = 1;
    }
    TT_ASSERT(found_rebuild, "should contain rebuild/foo.c");
    TT_ASSERT(found_rebuilder, "should contain src/rebuilder/foo.c");

    tt_discovered_paths_free(&dp);
    tt_file_filter_free(&ff);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_int_ff_fnmatch_ex)
{
    /* Basic matching */
    TT_ASSERT(tt_fnmatch_ex("*.txt", "hello.txt", 0),
              "*.txt should match hello.txt");
    TT_ASSERT(!tt_fnmatch_ex("*.txt", "hello.log", 0),
              "*.txt should NOT match hello.log");

    /* FNM_PATHNAME: * does not match / */
    TT_ASSERT(!tt_fnmatch_ex("*.c", "src/foo.c", TT_FNM_PATHNAME),
              "*.c with PATHNAME should NOT match src/foo.c");
    TT_ASSERT(tt_fnmatch_ex("*.c", "foo.c", TT_FNM_PATHNAME),
              "*.c with PATHNAME should match foo.c");

    /* ** globstar */
    TT_ASSERT(tt_fnmatch_ex("**/*.c", "src/foo.c", TT_FNM_PATHNAME),
              "**/*.c should match src/foo.c");
    TT_ASSERT(tt_fnmatch_ex("**/*.c", "src/deep/foo.c", TT_FNM_PATHNAME),
              "**/*.c should match src/deep/foo.c");
    TT_ASSERT(tt_fnmatch_ex("**/*.c", "foo.c", TT_FNM_PATHNAME),
              "**/*.c should match foo.c (zero segments)");

    /* FNM_CASEFOLD */
    TT_ASSERT(tt_fnmatch_ex("*.PHP", "test.php", TT_FNM_CASEFOLD),
              "*.PHP with CASEFOLD should match test.php");
    TT_ASSERT(!tt_fnmatch_ex("*.PHP", "test.php", 0),
              "*.PHP without CASEFOLD should NOT match test.php");

    /* Combined */
    TT_ASSERT(tt_fnmatch_ex("src/**/*.js", "src/deep/app.js", TT_FNM_PATHNAME),
              "src/**/*.js should match src/deep/app.js");

    /* ? wildcard */
    TT_ASSERT(tt_fnmatch_ex("file?.txt", "file1.txt", 0),
              "file?.txt should match file1.txt");
    TT_ASSERT(!tt_fnmatch_ex("file?.txt", "file12.txt", 0),
              "file?.txt should NOT match file12.txt");

    /* Character class */
    TT_ASSERT(tt_fnmatch_ex("[abc].txt", "a.txt", 0),
              "[abc].txt should match a.txt");
    TT_ASSERT(!tt_fnmatch_ex("[abc].txt", "d.txt", 0),
              "[abc].txt should NOT match d.txt");
}

TT_TEST(test_int_ff_gitignore_parsing)
{
    char *tmpdir = tt_test_tmpdir();
    TT_ASSERT_NOT_NULL(tmpdir);

    /* Root .gitignore */
    tt_test_write_file(tmpdir, ".gitignore", "*.log\n/build\ntmp/\n");
    /* Nested .gitignore */
    tt_test_write_file(tmpdir, "src/.gitignore", "*.generated.php\n");

    tt_file_filter_t ff;
    tt_file_filter_init(&ff, 0, NULL, false, NULL);

    /* Load root .gitignore */
    tt_file_filter_load_gitignore(&ff, tmpdir, tmpdir);

    /* Load nested .gitignore */
    {
        char src_dir[2048];
        snprintf(src_dir, sizeof(src_dir), "%s/src", tmpdir);
        tt_file_filter_load_gitignore(&ff, src_dir, tmpdir);
    }

    /* Simple pattern: *.log matches debug.log */
    TT_ASSERT(tt_file_filter_is_gitignored(&ff, "debug.log"),
              "*.log should match debug.log");

    /* Unanchored *.log matches nested path */
    TT_ASSERT(tt_file_filter_is_gitignored(&ff, "src/deep/file.log"),
              "*.log should match src/deep/file.log");

    /* Anchored /build matches build/ but not src/build/ */
    TT_ASSERT(tt_file_filter_is_gitignored(&ff, "build"),
              "/build should match build");
    TT_ASSERT(tt_file_filter_is_gitignored(&ff, "build/output.js"),
              "/build should match build/output.js");
    TT_ASSERT(!tt_file_filter_is_gitignored(&ff, "src/build/output.js"),
              "/build should NOT match src/build/output.js");

    /* tmp/ matches as directory pattern */
    TT_ASSERT(tt_file_filter_is_gitignored(&ff, "tmp"),
              "tmp/ should match tmp");

    /* Nested .gitignore: *.generated.php applies in src/ */
    TT_ASSERT(tt_file_filter_is_gitignored(&ff, "src/Model.generated.php"),
              "*.generated.php should match in src/");

    /* But NOT at root */
    TT_ASSERT(!tt_file_filter_is_gitignored(&ff, "Model.generated.php"),
              "*.generated.php should NOT match at root (src/ only rule)");

    tt_file_filter_free(&ff);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_int_ff_extra_ignore)
{
    char *tmpdir = tt_test_tmpdir();
    TT_ASSERT_NOT_NULL(tmpdir);

    tt_test_write_file(tmpdir, "src/app.php", "<?php ?>");
    tt_test_write_file(tmpdir, "staging/deploy.php", "<?php ?>");

    const char *extra[] = { "staging/*", NULL };

    tt_file_filter_t ff;
    tt_file_filter_init(&ff, 0, extra, false, NULL);
    tt_discovered_paths_t dp = {0};
    tt_discover_paths(tmpdir, &ff, &dp);

    TT_ASSERT_EQ_INT(dp.count, 1);
    if (dp.count == 1) {
        TT_ASSERT_EQ_STR(dp.paths[0], "src/app.php");
    }

    tt_discovered_paths_free(&dp);
    tt_file_filter_free(&ff);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_int_ff_skip_file_patterns)
{
    char *tmpdir = tt_test_tmpdir();
    TT_ASSERT_NOT_NULL(tmpdir);

    tt_test_write_file(tmpdir, "src/app.js", "var x = 1;");
    tt_test_write_file(tmpdir, "src/app.min.js", "var x=1;");
    tt_test_write_file(tmpdir, "src/style.min.css", "body{}");
    tt_test_write_file(tmpdir, "src/app.bundle.js", "var y=2;");
    tt_test_write_file(tmpdir, "package-lock.json", "{}");

    tt_file_filter_t ff;
    tt_file_filter_init(&ff, 0, NULL, false, NULL);
    tt_discovered_paths_t dp = {0};
    tt_discover_paths(tmpdir, &ff, &dp);

    /* Only src/app.js should survive:
     * - app.min.js: SKIP_FILE_PATTERNS *.min.js
     * - style.min.css: not a SOURCE_EXTENSION
     * - app.bundle.js: SKIP_FILE_PATTERNS *.bundle.js
     * - package-lock.json: not a SOURCE_EXTENSION */
    TT_ASSERT_EQ_INT(dp.count, 1);
    if (dp.count == 1) {
        TT_ASSERT_EQ_STR(dp.paths[0], "src/app.js");
    }

    tt_discovered_paths_free(&dp);
    tt_file_filter_free(&ff);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_int_ff_path_validation)
{
    char *tmpdir = tt_test_tmpdir();
    TT_ASSERT_NOT_NULL(tmpdir);

    tt_test_write_file(tmpdir, "inside/file.txt", "data");

    char inside_path[2048];
    snprintf(inside_path, sizeof(inside_path), "%s/inside/file.txt", tmpdir);

    /* Path inside root -> valid */
    TT_ASSERT(tt_path_validate(inside_path, tmpdir),
              "path inside root should be valid");

    /* Root itself -> valid */
    TT_ASSERT(tt_path_validate(tmpdir, tmpdir),
              "root itself should be valid");

    /* Path outside root -> invalid */
    TT_ASSERT(!tt_path_validate("/tmp", tmpdir),
              "/tmp should not be valid under tmpdir");

#ifndef TT_PLATFORM_WINDOWS
    /* Symlink inside root -> NOT escape */
    {
        char target[2048], link_path[2048];
        snprintf(target, sizeof(target), "%s/inside/file.txt", tmpdir);
        snprintf(link_path, sizeof(link_path), "%s/inside/link.txt", tmpdir);
        if (symlink(target, link_path) != 0) {
            TT_ASSERT(0, "symlink() failed for inside link");
            return;
        }

        TT_ASSERT(!tt_is_symlink_escape(link_path, tmpdir),
                  "symlink inside root should NOT be escape");
    }

    /* Symlink outside root -> escape */
    {
        char link_path[2048];
        snprintf(link_path, sizeof(link_path), "%s/inside/outside_link.txt", tmpdir);
        if (symlink("/etc/hosts", link_path) != 0) {
            TT_ASSERT(0, "symlink() failed for outside link");
            return;
        }

        TT_ASSERT(tt_is_symlink_escape(link_path, tmpdir),
                  "symlink to /etc/hosts should be escape");
    }
#endif /* !TT_PLATFORM_WINDOWS */

    /* Non-symlink -> NOT escape */
    TT_ASSERT(!tt_is_symlink_escape(inside_path, tmpdir),
              "regular file should NOT be symlink escape");

    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

static void test_int_ff_smart_filter_prunes_vendor_subdir(void)
{
    char *tmpdir = tt_test_tmpdir();
    TT_ASSERT(tmpdir != NULL, "tmpdir");

    /* Create structure:
     *   root/
     *     main.c
     *     vendor_lib/
     *       package.json    (vendor marker)
     *       lib.js
     */
    tt_test_write_file(tmpdir, "main.c", "int main() { return 0; }");
    tt_test_write_file(tmpdir, "vendor_lib/package.json", "{\"name\": \"vendor\"}");
    tt_test_write_file(tmpdir, "vendor_lib/lib.js", "function vendor() {}");

    /* With smart_filter=true, vendor_lib/ should be pruned */
    {
        tt_file_filter_t ff;
        tt_file_filter_init(&ff, 0, NULL, true, NULL);
        tt_discovered_paths_t result;
        int rc = tt_discover_paths(tmpdir, &ff, &result);
        TT_ASSERT(rc == 0, "discover smart_filter=true");
        TT_ASSERT(result.count == 1, "only main.c with smart filter");
        TT_ASSERT(strcmp(result.paths[0], "main.c") == 0, "found main.c");
        tt_discovered_paths_free(&result);
        tt_file_filter_free(&ff);
    }

    /* With smart_filter=false, vendor_lib/lib.js should be included */
    {
        tt_file_filter_t ff;
        tt_file_filter_init(&ff, 0, NULL, false, NULL);
        tt_discovered_paths_t result;
        int rc = tt_discover_paths(tmpdir, &ff, &result);
        TT_ASSERT(rc == 0, "discover smart_filter=false");
        TT_ASSERT(result.count == 2, "main.c + lib.js without smart filter");
        tt_discovered_paths_free(&result);
        tt_file_filter_free(&ff);
    }

    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

/* ---- Markdown smart filter tests ---- */

TT_TEST(test_int_ff_markdown_discovered_without_smart_filter)
{
    char *tmpdir = tt_test_tmpdir();
    tt_test_write_file(tmpdir, "docs/guide.md", "# Hello\n## World\n");
    tt_test_write_file(tmpdir, "src/main.php", "<?php\n");

    tt_file_filter_t ff;
    tt_file_filter_init(&ff, 0, NULL, false, NULL);
    tt_discovered_paths_t result;
    int rc = tt_discover_paths(tmpdir, &ff, &result);
    TT_ASSERT(rc == 0, "discover smart_filter=false");
    TT_ASSERT(paths_contain(&result, "docs/guide.md"),
              ".md discovered without smart filter");
    TT_ASSERT(paths_contain(&result, "src/main.php"),
              ".php discovered without smart filter");
    tt_discovered_paths_free(&result);
    tt_file_filter_free(&ff);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_int_ff_markdown_included_with_smart_filter)
{
    char *tmpdir = tt_test_tmpdir();
    tt_test_write_file(tmpdir, "docs/guide.md", "# Hello\n");
    tt_test_write_file(tmpdir, "docs/notes.markdown", "# Notes\n");
    tt_test_write_file(tmpdir, "src/main.php", "<?php\n");

    tt_file_filter_t ff;
    tt_file_filter_init(&ff, 0, NULL, true, NULL);
    tt_discovered_paths_t result;
    int rc = tt_discover_paths(tmpdir, &ff, &result);
    TT_ASSERT(rc == 0, "discover smart_filter=true");
    TT_ASSERT(paths_contain(&result, "docs/guide.md"),
              ".md included with smart filter");
    TT_ASSERT(paths_contain(&result, "docs/notes.markdown"),
              ".markdown included with smart filter");
    TT_ASSERT(paths_contain(&result, "src/main.php"),
              ".php included with smart filter");
    tt_discovered_paths_free(&result);
    tt_file_filter_free(&ff);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_int_ff_mdx_included_with_smart_filter)
{
    char *tmpdir = tt_test_tmpdir();
    tt_test_write_file(tmpdir, "docs/Button.mdx", "# Button\n");
    tt_test_write_file(tmpdir, "src/index.js", "export default {};\n");

    tt_file_filter_t ff;
    tt_file_filter_init(&ff, 0, NULL, true, NULL);
    tt_discovered_paths_t result;
    int rc = tt_discover_paths(tmpdir, &ff, &result);
    TT_ASSERT(rc == 0, "discover smart_filter=true");
    TT_ASSERT(paths_contain(&result, "docs/Button.mdx"),
              ".mdx included with smart filter");
    TT_ASSERT(paths_contain(&result, "src/index.js"),
              ".js included with smart filter");
    tt_discovered_paths_free(&result);
    tt_file_filter_free(&ff);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

/* ===== --include (force-include skipped directories) ===== */

TT_TEST(test_int_ff_include_overrides_skip_dirs)
{
    char *tmpdir = tt_test_tmpdir();
    tt_test_write_file(tmpdir, "vendor/autoload.php", "<?php\n");
    tt_test_write_file(tmpdir, "src/App.php", "<?php class App {}\n");

    const char *incl[] = {"vendor", NULL};
    tt_file_filter_t ff;
    tt_file_filter_init(&ff, 0, NULL, false, incl);
    tt_discovered_paths_t dp = {0};
    int rc = tt_discover_paths(tmpdir, &ff, &dp);
    TT_ASSERT(rc == 0, "discover with include_dirs");
    TT_ASSERT(paths_contain(&dp, "vendor/autoload.php"),
              "vendor/autoload.php discovered when vendor is included");
    TT_ASSERT(paths_contain(&dp, "src/App.php"),
              "src/App.php still discovered");
    tt_discovered_paths_free(&dp);
    tt_file_filter_free(&ff);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_int_ff_include_preserves_other_skip_dirs)
{
    char *tmpdir = tt_test_tmpdir();
    tt_test_write_file(tmpdir, "vendor/autoload.php", "<?php\n");
    tt_test_write_file(tmpdir, "node_modules/foo/index.js", "export {};\n");
    tt_test_write_file(tmpdir, "src/main.js", "console.log('hi');\n");

    const char *incl[] = {"vendor", NULL};
    tt_file_filter_t ff;
    tt_file_filter_init(&ff, 0, NULL, false, incl);
    tt_discovered_paths_t dp = {0};
    tt_discover_paths(tmpdir, &ff, &dp);
    TT_ASSERT(paths_contain(&dp, "vendor/autoload.php"),
              "vendor included");
    TT_ASSERT(!paths_any_prefix(&dp, "node_modules/"),
              "node_modules still excluded");
    tt_discovered_paths_free(&dp);
    tt_file_filter_free(&ff);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_int_ff_include_git_blocked)
{
    char *tmpdir = tt_test_tmpdir();
    tt_test_write_file(tmpdir, ".git/config", "[core]\n");
    tt_test_write_file(tmpdir, "src/main.c", "int main() {}\n");

    const char *incl[] = {".git", NULL};
    tt_file_filter_t ff;
    tt_file_filter_init(&ff, 0, NULL, false, incl);
    tt_discovered_paths_t dp = {0};
    tt_discover_paths(tmpdir, &ff, &dp);
    TT_ASSERT(!paths_any_prefix(&dp, ".git/"),
              ".git blocked from inclusion");
    tt_discovered_paths_free(&dp);
    tt_file_filter_free(&ff);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_int_ff_include_segment_matching)
{
    char *tmpdir = tt_test_tmpdir();
    tt_test_write_file(tmpdir, "vendor/package/src/Foo.php", "<?php class Foo {}\n");

    const char *incl[] = {"vendor", NULL};
    tt_file_filter_t ff;
    tt_file_filter_init(&ff, 0, NULL, false, incl);
    tt_discovered_paths_t dp = {0};
    tt_discover_paths(tmpdir, &ff, &dp);
    TT_ASSERT(paths_contain(&dp, "vendor/package/src/Foo.php"),
              "nested file under vendor included");
    tt_discovered_paths_free(&dp);
    tt_file_filter_free(&ff);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_int_ff_include_with_smart_filter)
{
    char *tmpdir = tt_test_tmpdir();
    tt_test_write_file(tmpdir, "vendor/autoload.php", "<?php\n");
    tt_test_write_file(tmpdir, "vendor/style.css", "body {}\n");
    tt_test_write_file(tmpdir, "src/App.php", "<?php class App {}\n");

    const char *incl[] = {"vendor", NULL};
    tt_file_filter_t ff;
    tt_file_filter_init(&ff, 0, NULL, true, incl);
    tt_discovered_paths_t dp = {0};
    tt_discover_paths(tmpdir, &ff, &dp);
    TT_ASSERT(paths_contain(&dp, "vendor/autoload.php"),
              "vendor PHP included with smart filter");
    TT_ASSERT(!paths_contain(&dp, "vendor/style.css"),
              "vendor CSS excluded by smart filter nocode extensions");
    TT_ASSERT(paths_contain(&dp, "src/App.php"),
              "src PHP still included");
    tt_discovered_paths_free(&dp);
    tt_file_filter_free(&ff);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

void run_int_file_filter_tests(void)
{
    TT_RUN(test_int_ff_discover_includes_source);
    TT_RUN(test_int_ff_discover_excludes_vendor);
    TT_RUN(test_int_ff_discover_excludes_git);
    TT_RUN(test_int_ff_discover_excludes_secrets);
    TT_RUN(test_int_ff_toktokenignore_support);
    TT_RUN(test_int_ff_edge_binary);
    TT_RUN(test_int_ff_edge_empty);
    TT_RUN(test_int_ff_edge_size_limit);
    TT_RUN(test_int_ff_edge_mixed_case_ext);
    TT_RUN(test_int_ff_edge_rebuild_dir);
    TT_RUN(test_int_ff_segment_matching);
    TT_RUN(test_int_ff_fnmatch_ex);
    TT_RUN(test_int_ff_gitignore_parsing);

    TT_RUN(test_int_ff_extra_ignore);
    TT_RUN(test_int_ff_skip_file_patterns);
    TT_RUN(test_int_ff_path_validation);
    TT_RUN(test_int_ff_smart_filter_prunes_vendor_subdir);
    TT_RUN(test_int_ff_markdown_discovered_without_smart_filter);
    TT_RUN(test_int_ff_markdown_included_with_smart_filter);
    TT_RUN(test_int_ff_mdx_included_with_smart_filter);

    TT_RUN(test_int_ff_include_overrides_skip_dirs);
    TT_RUN(test_int_ff_include_preserves_other_skip_dirs);
    TT_RUN(test_int_ff_include_git_blocked);
    TT_RUN(test_int_ff_include_segment_matching);
    TT_RUN(test_int_ff_include_with_smart_filter);
}
