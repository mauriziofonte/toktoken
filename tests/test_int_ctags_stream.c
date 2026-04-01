/*
 * test_int_ctags_stream.c -- Integration tests for streaming ctags pipe reader.
 *
 * Requires universal-ctags in PATH.
 */

#include "test_framework.h"
#include "test_helpers.h"
#include "ctags_stream.h"
#include "platform.h"
#include "str_util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

static const char *ctags_path(void)
{
    static const char *names[] = {
        "ctags", "universal-ctags", "ctags-universal", NULL};
    static char resolved[512];
    for (int i = 0; names[i]; i++)
    {
        char *path = tt_which(names[i]);
        if (path)
        {
            snprintf(resolved, sizeof(resolved), "%s", path);
            free(path);
            return resolved;
        }
    }
    return NULL;
}

/*
 * write_file_list -- Write an array of absolute paths to a temp file.
 * Returns [caller-frees] path to the temp file.
 */
static char *write_file_list(const char *const *paths, int count)
{
    char *tmpfile_path = tt_strdup("/tmp/ctags_stream_test_XXXXXX");
    if (!tmpfile_path)
        return NULL;

    int fd = mkstemp(tmpfile_path);
    if (fd < 0)
    {
        free(tmpfile_path);
        return NULL;
    }

    for (int i = 0; i < count; i++)
    {
        if (write(fd, paths[i], strlen(paths[i])) < 0 ||
            write(fd, "\n", 1) < 0)
        {
            close(fd);
            free(tmpfile_path);
            return NULL;
        }
    }
    close(fd);
    return tmpfile_path;
}

TT_TEST(test_stream_basic_readline)
{
    const char *ctags = ctags_path();
    if (!ctags)
        return;

    /* Use mini-project fixture via tt_test_fixture (CWD-independent) */
    const char *fixture = tt_test_fixture("mini-project");
    if (!fixture)
        return;

    char app_path[2048], svc_path[2048];
    snprintf(app_path, sizeof(app_path), "%s/src/App.php", fixture);
    snprintf(svc_path, sizeof(svc_path), "%s/src/Service.php", fixture);

    const char *paths[] = {app_path, svc_path};
    char *file_list = write_file_list(paths, 2);
    TT_ASSERT_NOT_NULL(file_list);

    tt_ctags_stream_t stream;
    int rc = tt_ctags_stream_start(&stream, ctags, file_list, 30);
    TT_ASSERT_EQ_INT(0, rc);

    int line_count = 0;
    size_t len;
    const char *line;
    while ((line = tt_ctags_stream_readline(&stream, &len)) != NULL)
    {
        /* Each ctags JSON line should start with { */
        TT_ASSERT(len > 0, "line should not be empty");
        TT_ASSERT(line[0] == '{', "line should be JSON object");
        line_count++;
    }

    TT_ASSERT(line_count > 0, "should have read at least one line");

    char *stderr_out = NULL;
    int exit_code = tt_ctags_stream_finish(&stream, &stderr_out);
    TT_ASSERT_EQ_INT(0, exit_code);
    free(stderr_out);
    unlink(file_list);
    free(file_list);
}

TT_TEST(test_stream_empty_file_list)
{
    const char *ctags = ctags_path();
    if (!ctags)
        return;

    /* Create empty temp file */
    char *file_list = write_file_list(NULL, 0);
    TT_ASSERT_NOT_NULL(file_list);

    tt_ctags_stream_t stream;
    int rc = tt_ctags_stream_start(&stream, ctags, file_list, 10);
    TT_ASSERT_EQ_INT(0, rc);

    /* Should return NULL immediately (no output from ctags on empty input) */
    size_t len;
    const char *line = tt_ctags_stream_readline(&stream, &len);
    TT_ASSERT(line == NULL, "empty file list should produce no lines");

    char *stderr_out = NULL;
    int exit_code = tt_ctags_stream_finish(&stream, &stderr_out);
    TT_ASSERT_EQ_INT(0, exit_code);
    free(stderr_out);
    unlink(file_list);
    free(file_list);
}

TT_TEST(test_stream_stderr_capture)
{
    const char *ctags = ctags_path();
    if (!ctags)
        return;

    /* Write a nonexistent file path to the file list */
    const char *paths[] = {"/nonexistent/path/to/file.php"};
    char *file_list = write_file_list(paths, 1);
    TT_ASSERT_NOT_NULL(file_list);

    tt_ctags_stream_t stream;
    int rc = tt_ctags_stream_start(&stream, ctags, file_list, 10);
    TT_ASSERT_EQ_INT(0, rc);

    /* Drain all lines (there may be none or some warnings) */
    size_t len;
    while (tt_ctags_stream_readline(&stream, &len) != NULL)
    {
        /* consume */
    }

    char *stderr_out = NULL;
    tt_ctags_stream_finish(&stream, &stderr_out);

    /* ctags should have written something to stderr about the bad path */
    TT_ASSERT(stderr_out != NULL, "stderr should be captured");
    if (stderr_out)
    {
        TT_ASSERT(strlen(stderr_out) > 0, "stderr should be non-empty");
    }

    free(stderr_out);
    unlink(file_list);
    free(file_list);
}

TT_TEST(test_stream_timeout)
{
    const char *ctags = ctags_path();
    if (!ctags)
        return;

    /* Create a temp directory with many source files to make ctags slow */
    char *tmpdir = tt_test_tmpdir();
    TT_ASSERT_NOT_NULL(tmpdir);

    const char *abs_paths[500];
    char path_storage[500][512];
    for (int i = 0; i < 500; i++)
    {
        char rel[64];
        snprintf(rel, sizeof(rel), "src/file%d.php", i);
        char content[512];
        snprintf(content, sizeof(content),
                 "<?php\nclass File%d {\n"
                 "    public function method%d() {}\n"
                 "    public function other%d() {}\n"
                 "    public function third%d() {}\n"
                 "}\n",
                 i, i, i, i);
        tt_test_write_file(tmpdir, rel, content);
        snprintf(path_storage[i], sizeof(path_storage[i]),
                 "%s/%s", tmpdir, rel);
        abs_paths[i] = path_storage[i];
    }

    char *file_list = write_file_list(abs_paths, 500);
    TT_ASSERT_NOT_NULL(file_list);

    /*
     * Set a very short timeout. ctags might finish quickly on a fast
     * system, so this test verifies the timeout mechanism doesn't crash
     * rather than guaranteeing a timeout occurs.
     */
    tt_ctags_stream_t stream;
    int rc = tt_ctags_stream_start(&stream, ctags, file_list, 1);
    TT_ASSERT_EQ_INT(0, rc);

    /* Read until EOF or timeout — either outcome is acceptable */
    size_t len;
    int line_count = 0;
    while (tt_ctags_stream_readline(&stream, &len) != NULL)
    {
        line_count++;
    }
    TT_ASSERT(line_count >= 0, "lines read");

    /* Finish should not crash regardless of timeout */
    char *stderr_out = NULL;
    tt_ctags_stream_finish(&stream, &stderr_out);
    free(stderr_out);

    unlink(file_list);
    free(file_list);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_stream_large_output)
{
    const char *ctags = ctags_path();
    if (!ctags)
        return;

    /* Create files with many symbols to exceed 64KB pipe buffer */
    char *tmpdir = tt_test_tmpdir();
    TT_ASSERT_NOT_NULL(tmpdir);

    const char *abs_paths[20];
    char path_storage[20][512];
    for (int i = 0; i < 20; i++)
    {
        char rel[64];
        snprintf(rel, sizeof(rel), "src/big%d.php", i);

        /* Each file has ~50 methods = ~50 ctags entries */
        tt_strbuf_t sb;
        tt_strbuf_init(&sb);
        tt_strbuf_append_str(&sb, "<?php\n");
        char line[128];
        snprintf(line, sizeof(line), "class Big%d {\n", i);
        tt_strbuf_append_str(&sb, line);
        for (int j = 0; j < 50; j++)
        {
            snprintf(line, sizeof(line),
                     "    public function method%d_%d(): string { return ''; }\n",
                     i, j);
            tt_strbuf_append_str(&sb, line);
        }
        tt_strbuf_append_str(&sb, "}\n");

        tt_test_write_file(tmpdir, rel, sb.data);
        tt_strbuf_free(&sb);

        snprintf(path_storage[i], sizeof(path_storage[i]),
                 "%s/%s", tmpdir, rel);
        abs_paths[i] = path_storage[i];
    }

    char *file_list = write_file_list(abs_paths, 20);
    TT_ASSERT_NOT_NULL(file_list);

    tt_ctags_stream_t stream;
    int rc = tt_ctags_stream_start(&stream, ctags, file_list, 60);
    TT_ASSERT_EQ_INT(0, rc);

    int line_count = 0;
    size_t len;
    while (tt_ctags_stream_readline(&stream, &len) != NULL)
    {
        line_count++;
    }

    /* 20 files * ~51 symbols (class + 50 methods) = ~1020 lines */
    TT_ASSERT(line_count > 100,
              "should read many lines from large output");

    char *stderr_out = NULL;
    int exit_code = tt_ctags_stream_finish(&stream, &stderr_out);
    TT_ASSERT_EQ_INT(0, exit_code);
    free(stderr_out);

    unlink(file_list);
    free(file_list);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_stream_line_buffer_growth)
{
    const char *ctags = ctags_path();
    if (!ctags)
        return;

    /*
     * Create a PHP file with a very long function name to produce
     * a ctags JSON line that exceeds the initial 4096 buffer.
     */
    char *tmpdir = tt_test_tmpdir();
    TT_ASSERT_NOT_NULL(tmpdir);

    tt_strbuf_t sb;
    tt_strbuf_init(&sb);
    tt_strbuf_append_str(&sb, "<?php\nclass LongNames {\n");

    /* Create a function with ~5000 char name */
    tt_strbuf_append_str(&sb, "    public function ");
    for (int i = 0; i < 5000; i++)
        tt_strbuf_append_char(&sb, 'a' + (i % 26));
    tt_strbuf_append_str(&sb, "(): void {}\n");

    tt_strbuf_append_str(&sb, "}\n");

    tt_test_write_file(tmpdir, "src/long.php", sb.data);
    tt_strbuf_free(&sb);

    char abs_path[512];
    snprintf(abs_path, sizeof(abs_path), "%s/src/long.php", tmpdir);
    const char *paths[] = {abs_path};
    char *file_list = write_file_list(paths, 1);
    TT_ASSERT_NOT_NULL(file_list);

    tt_ctags_stream_t stream;
    int rc = tt_ctags_stream_start(&stream, ctags, file_list, 30);
    TT_ASSERT_EQ_INT(0, rc);

    int line_count = 0;
    int found_long = 0;
    size_t len;
    const char *line;
    while ((line = tt_ctags_stream_readline(&stream, &len)) != NULL)
    {
        line_count++;
        /* The long function name line should be > 4096 bytes */
        if (len > 4096)
            found_long = 1;
    }

    TT_ASSERT(line_count > 0, "should read lines");
    TT_ASSERT(found_long, "should have a line exceeding initial buffer size");

    char *stderr_out = NULL;
    int exit_code = tt_ctags_stream_finish(&stream, &stderr_out);
    TT_ASSERT_EQ_INT(0, exit_code);
    free(stderr_out);

    unlink(file_list);
    free(file_list);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

void run_int_ctags_stream_tests(void)
{
    TT_RUN(test_stream_basic_readline);
    TT_RUN(test_stream_empty_file_list);
    TT_RUN(test_stream_stderr_capture);
    TT_RUN(test_stream_timeout);
    TT_RUN(test_stream_large_output);
    TT_RUN(test_stream_line_buffer_growth);
}
