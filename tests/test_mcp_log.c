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
#include "test_compat.h"

/* ---- Helpers ---- */

/*
 * Derive the expected mcp.jsonl path from the production storage API.
 * This avoids hardcoding platform-specific cache layouts and respects
 * whatever HOME/USERPROFILE the test has set.
 */
static char *mcp_log_path(void)
{
    char *logs_dir = tt_storage_logs_dir();
    if (!logs_dir) return NULL;
    char *path = tt_path_join(logs_dir, "mcp.jsonl");
    free(logs_dir);
    return path;
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

/* ---- Platform-aware HOME override ---- */

static void set_home_env(const char *dir)
{
#ifdef TT_PLATFORM_WINDOWS
    setenv("USERPROFILE", dir, 1);
#else
    setenv("HOME", dir, 1);
#endif
}

static void restore_home_env(const char *orig)
{
#ifdef TT_PLATFORM_WINDOWS
    if (orig) setenv("USERPROFILE", orig, 1);
    else      unsetenv("USERPROFILE");
#else
    if (orig) setenv("HOME", orig, 1);
    else      unsetenv("HOME");
#endif
}

static char *save_home_env(void)
{
#ifdef TT_PLATFORM_WINDOWS
    const char *v = getenv("USERPROFILE");
#else
    const char *v = getenv("HOME");
#endif
    return v ? tt_strdup(v) : NULL;
}

/* ---- Tests ---- */

TT_TEST(test_mcp_log_tool_call_creates_file)
{
    char *tmpdir = tt_test_tmpdir();
    char *orig_home = save_home_env();
    set_home_env(tmpdir);

    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "query", "test");
    tt_mcp_log_tool_call("search_symbols", args, "/tmp/project", 42, true, NULL);
    cJSON_Delete(args);

    char *path = mcp_log_path();
    TT_ASSERT_TRUE(tt_file_exists(path));

    restore_home_env(orig_home);
    free(orig_home);
    free(path);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_mcp_log_tool_call_format)
{
    char *tmpdir = tt_test_tmpdir();
    char *orig_home = save_home_env();
    set_home_env(tmpdir);

    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "query", "Controller");
    cJSON_AddStringToObject(args, "kind", "class");
    tt_mcp_log_tool_call("search_symbols", args, "/home/user/project",
                         55, true, NULL);
    cJSON_Delete(args);

    char *path = mcp_log_path();
    char **lines = NULL;
    int count = read_jsonl_lines(path, &lines);
    TT_ASSERT_EQ_INT(1, count);
    if (count < 1 || !lines) {
        restore_home_env(orig_home);
        free(orig_home);
        free(path);
        tt_test_rmdir(tmpdir);
        free(tmpdir);
        return;
    }

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

    restore_home_env(orig_home);
    free(orig_home);
    free(path);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_mcp_log_tool_call_failure)
{
    char *tmpdir = tt_test_tmpdir();
    char *orig_home = save_home_env();
    set_home_env(tmpdir);

    tt_mcp_log_tool_call("index_create", NULL, "/tmp/proj",
                         150, false, "No ctags binary found");

    char *path = mcp_log_path();
    char **lines = NULL;
    int count = read_jsonl_lines(path, &lines);
    TT_ASSERT_EQ_INT(1, count);
    if (count < 1 || !lines) {
        restore_home_env(orig_home);
        free(orig_home);
        free(path);
        tt_test_rmdir(tmpdir);
        free(tmpdir);
        return;
    }

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

    restore_home_env(orig_home);
    free(orig_home);
    free(path);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_mcp_log_lifecycle_initialize)
{
    char *tmpdir = tt_test_tmpdir();
    char *orig_home = save_home_env();
    set_home_env(tmpdir);

    tt_mcp_log_lifecycle(TT_MCP_LOG_INITIALIZE, "/tmp/project",
                         "Claude Code 1.2.3");

    char *path = mcp_log_path();
    char **lines = NULL;
    int count = read_jsonl_lines(path, &lines);
    TT_ASSERT_EQ_INT(1, count);
    if (count < 1 || !lines) {
        restore_home_env(orig_home);
        free(orig_home);
        free(path);
        tt_test_rmdir(tmpdir);
        free(tmpdir);
        return;
    }

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

    restore_home_env(orig_home);
    free(orig_home);
    free(path);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_mcp_log_lifecycle_shutdown)
{
    char *tmpdir = tt_test_tmpdir();
    char *orig_home = save_home_env();
    set_home_env(tmpdir);

    tt_mcp_log_lifecycle(TT_MCP_LOG_SHUTDOWN, "/tmp/project", NULL);

    char *path = mcp_log_path();
    char **lines = NULL;
    int count = read_jsonl_lines(path, &lines);
    TT_ASSERT_EQ_INT(1, count);
    if (count < 1 || !lines) {
        restore_home_env(orig_home);
        free(orig_home);
        free(path);
        tt_test_rmdir(tmpdir);
        free(tmpdir);
        return;
    }

    cJSON *entry = cJSON_Parse(lines[0]);
    TT_ASSERT_NOT_NULL(entry);

    TT_ASSERT_EQ_STR("shutdown",
                      cJSON_GetStringValue(
                          cJSON_GetObjectItemCaseSensitive(entry, "event")));
    /* No detail field for shutdown */
    TT_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(entry, "detail"));

    cJSON_Delete(entry);
    free_lines(lines, count);

    restore_home_env(orig_home);
    free(orig_home);
    free(path);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_mcp_log_append_multiple)
{
    char *tmpdir = tt_test_tmpdir();
    char *orig_home = save_home_env();
    set_home_env(tmpdir);

    tt_mcp_log_lifecycle(TT_MCP_LOG_INITIALIZE, "/tmp/p", "test 1.0");
    tt_mcp_log_tool_call("stats", NULL, "/tmp/p", 10, true, NULL);
    tt_mcp_log_lifecycle(TT_MCP_LOG_SHUTDOWN, "/tmp/p", NULL);

    char *path = mcp_log_path();
    char **lines = NULL;
    int count = read_jsonl_lines(path, &lines);
    TT_ASSERT_EQ_INT(3, count);
    if (count < 3 || !lines) {
        restore_home_env(orig_home);
        free(orig_home);
        free(path);
        tt_test_rmdir(tmpdir);
        free(tmpdir);
        return;
    }

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

    restore_home_env(orig_home);
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
