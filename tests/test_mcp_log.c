/*
 * test_mcp_log.c -- Unit tests for MCP JSONL logging.
 */

#include "test_framework.h"
#include "test_helpers.h"
#include "mcp_log.h"
#include "platform.h"
#include "storage_paths.h"
#include "str_util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <cJSON.h>

/* ---- Helpers ---- */

/*
 * Build the expected mcp.jsonl path under a tmpdir with HOME override.
 */
static char *mcp_log_path(const char *tmpdir)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "%s/.cache/toktoken/logs/mcp.jsonl", tmpdir);
    return strdup(buf);
}

/*
 * Read file content and count lines. Returns line count, fills lines[]
 * with heap-allocated line strings (caller frees each + array).
 */
static int read_jsonl_lines(const char *path, char ***out_lines)
{
    FILE *fp = fopen(path, "r");
    if (!fp)
    {
        *out_lines = NULL;
        return 0;
    }

    char **lines = NULL;
    int count = 0;
    int cap = 0;
    char buf[4096];

    while (fgets(buf, sizeof(buf), fp))
    {
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n')
            buf[--len] = '\0';
        if (len == 0)
            continue;

        if (count >= cap)
        {
            cap = cap ? cap * 2 : 8;
            lines = realloc(lines, sizeof(char *) * (size_t)cap);
        }
        lines[count++] = strdup(buf);
    }

    fclose(fp);
    *out_lines = lines;
    return count;
}

static void free_lines(char **lines, int count)
{
    for (int i = 0; i < count; i++)
        free(lines[i]);
    free(lines);
}

/* ---- Tests ---- */

TT_TEST(test_mcp_log_tool_call_creates_file)
{
    char *tmpdir = tt_test_tmpdir();
    char *orig_home = getenv("HOME") ? tt_strdup(getenv("HOME")) : NULL;
    setenv("HOME", tmpdir, 1);

    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "query", "test");
    tt_mcp_log_tool_call("search_symbols", args, "/tmp/project", 42, true, NULL);
    cJSON_Delete(args);

    char *path = mcp_log_path(tmpdir);
    TT_ASSERT_TRUE(tt_file_exists(path));

    if (orig_home)
        setenv("HOME", orig_home, 1);
    else
        unsetenv("HOME");
    free(orig_home);
    free(path);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_mcp_log_tool_call_format)
{
    char *tmpdir = tt_test_tmpdir();
    char *orig_home = getenv("HOME") ? tt_strdup(getenv("HOME")) : NULL;
    setenv("HOME", tmpdir, 1);

    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "query", "Controller");
    cJSON_AddStringToObject(args, "kind", "class");
    tt_mcp_log_tool_call("search_symbols", args, "/home/user/project",
                         55, true, NULL);
    cJSON_Delete(args);

    char *path = mcp_log_path(tmpdir);
    char **lines = NULL;
    int count = read_jsonl_lines(path, &lines);
    TT_ASSERT_EQ_INT(1, count);

    cJSON *entry = cJSON_Parse(lines[0]);
    TT_ASSERT_NOT_NULL(entry);

    /* Validate fields */
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(entry, "ts"));
    TT_ASSERT_EQ_STR("tool_call",
                      cJSON_GetStringValue(
                          cJSON_GetObjectItemCaseSensitive(entry, "event")));
    TT_ASSERT_EQ_STR("search_symbols",
                      cJSON_GetStringValue(
                          cJSON_GetObjectItemCaseSensitive(entry, "tool")));
    TT_ASSERT_EQ_STR("/home/user/project",
                      cJSON_GetStringValue(
                          cJSON_GetObjectItemCaseSensitive(entry, "project")));

    cJSON *dur = cJSON_GetObjectItemCaseSensitive(entry, "duration_ms");
    TT_ASSERT_NOT_NULL(dur);
    TT_ASSERT_EQ_INT(55, (int)cJSON_GetNumberValue(dur));

    cJSON *success = cJSON_GetObjectItemCaseSensitive(entry, "success");
    TT_ASSERT_NOT_NULL(success);
    TT_ASSERT_TRUE(cJSON_IsTrue(success));

    /* args should contain query and kind */
    cJSON *logged_args = cJSON_GetObjectItemCaseSensitive(entry, "args");
    TT_ASSERT_NOT_NULL(logged_args);
    TT_ASSERT_EQ_STR("Controller",
                      cJSON_GetStringValue(
                          cJSON_GetObjectItemCaseSensitive(logged_args, "query")));

    /* No error field on success */
    TT_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(entry, "error"));

    cJSON_Delete(entry);
    free_lines(lines, count);

    if (orig_home)
        setenv("HOME", orig_home, 1);
    else
        unsetenv("HOME");
    free(orig_home);
    free(path);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_mcp_log_tool_call_failure)
{
    char *tmpdir = tt_test_tmpdir();
    char *orig_home = getenv("HOME") ? tt_strdup(getenv("HOME")) : NULL;
    setenv("HOME", tmpdir, 1);

    tt_mcp_log_tool_call("index_create", NULL, "/tmp/proj",
                         150, false, "No ctags binary found");

    char *path = mcp_log_path(tmpdir);
    char **lines = NULL;
    int count = read_jsonl_lines(path, &lines);
    TT_ASSERT_EQ_INT(1, count);

    cJSON *entry = cJSON_Parse(lines[0]);
    TT_ASSERT_NOT_NULL(entry);

    cJSON *success = cJSON_GetObjectItemCaseSensitive(entry, "success");
    TT_ASSERT_NOT_NULL(success);
    TT_ASSERT_TRUE(cJSON_IsFalse(success));

    TT_ASSERT_EQ_STR("No ctags binary found",
                      cJSON_GetStringValue(
                          cJSON_GetObjectItemCaseSensitive(entry, "error")));

    cJSON_Delete(entry);
    free_lines(lines, count);

    if (orig_home)
        setenv("HOME", orig_home, 1);
    else
        unsetenv("HOME");
    free(orig_home);
    free(path);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_mcp_log_lifecycle_initialize)
{
    char *tmpdir = tt_test_tmpdir();
    char *orig_home = getenv("HOME") ? tt_strdup(getenv("HOME")) : NULL;
    setenv("HOME", tmpdir, 1);

    tt_mcp_log_lifecycle(TT_MCP_LOG_INITIALIZE, "/tmp/project",
                         "Claude Code 1.2.3");

    char *path = mcp_log_path(tmpdir);
    char **lines = NULL;
    int count = read_jsonl_lines(path, &lines);
    TT_ASSERT_EQ_INT(1, count);

    cJSON *entry = cJSON_Parse(lines[0]);
    TT_ASSERT_NOT_NULL(entry);

    TT_ASSERT_EQ_STR("initialize",
                      cJSON_GetStringValue(
                          cJSON_GetObjectItemCaseSensitive(entry, "event")));
    TT_ASSERT_EQ_STR("Claude Code 1.2.3",
                      cJSON_GetStringValue(
                          cJSON_GetObjectItemCaseSensitive(entry, "detail")));
    TT_ASSERT_EQ_STR("/tmp/project",
                      cJSON_GetStringValue(
                          cJSON_GetObjectItemCaseSensitive(entry, "project")));

    cJSON_Delete(entry);
    free_lines(lines, count);

    if (orig_home)
        setenv("HOME", orig_home, 1);
    else
        unsetenv("HOME");
    free(orig_home);
    free(path);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_mcp_log_lifecycle_shutdown)
{
    char *tmpdir = tt_test_tmpdir();
    char *orig_home = getenv("HOME") ? tt_strdup(getenv("HOME")) : NULL;
    setenv("HOME", tmpdir, 1);

    tt_mcp_log_lifecycle(TT_MCP_LOG_SHUTDOWN, "/tmp/project", NULL);

    char *path = mcp_log_path(tmpdir);
    char **lines = NULL;
    int count = read_jsonl_lines(path, &lines);
    TT_ASSERT_EQ_INT(1, count);

    cJSON *entry = cJSON_Parse(lines[0]);
    TT_ASSERT_NOT_NULL(entry);

    TT_ASSERT_EQ_STR("shutdown",
                      cJSON_GetStringValue(
                          cJSON_GetObjectItemCaseSensitive(entry, "event")));
    /* No detail field for shutdown */
    TT_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(entry, "detail"));

    cJSON_Delete(entry);
    free_lines(lines, count);

    if (orig_home)
        setenv("HOME", orig_home, 1);
    else
        unsetenv("HOME");
    free(orig_home);
    free(path);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_mcp_log_append_multiple)
{
    char *tmpdir = tt_test_tmpdir();
    char *orig_home = getenv("HOME") ? tt_strdup(getenv("HOME")) : NULL;
    setenv("HOME", tmpdir, 1);

    tt_mcp_log_lifecycle(TT_MCP_LOG_INITIALIZE, "/tmp/p", "test 1.0");
    tt_mcp_log_tool_call("stats", NULL, "/tmp/p", 10, true, NULL);
    tt_mcp_log_lifecycle(TT_MCP_LOG_SHUTDOWN, "/tmp/p", NULL);

    char *path = mcp_log_path(tmpdir);
    char **lines = NULL;
    int count = read_jsonl_lines(path, &lines);
    TT_ASSERT_EQ_INT(3, count);

    /* Each line must be valid JSON */
    for (int i = 0; i < count; i++)
    {
        cJSON *entry = cJSON_Parse(lines[i]);
        TT_ASSERT_NOT_NULL(entry);
        TT_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(entry, "ts"));
        TT_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(entry, "event"));
        cJSON_Delete(entry);
    }

    free_lines(lines, count);

    if (orig_home)
        setenv("HOME", orig_home, 1);
    else
        unsetenv("HOME");
    free(orig_home);
    free(path);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

void run_mcp_log_tests(void)
{
    TT_SUITE("MCP Log");
    TT_RUN(test_mcp_log_tool_call_creates_file);
    TT_RUN(test_mcp_log_tool_call_format);
    TT_RUN(test_mcp_log_tool_call_failure);
    TT_RUN(test_mcp_log_lifecycle_initialize);
    TT_RUN(test_mcp_log_lifecycle_shutdown);
    TT_RUN(test_mcp_log_append_multiple);
}
