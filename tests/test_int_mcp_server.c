/*
 * test_int_mcp_server.c -- Integration tests for Phase 11: MCP Server.
 *
 * Converted from legacy test_mcp_server.c to test_framework.h format.
 * Uses pipe+fork for integration tests.
 */

#include "test_framework.h"
#include "test_helpers.h"

#include "mcp_server.h"
#include "mcp_tools.h"
#include "cmd_index.h"
#include "cmd_manage.h"
#include "cli.h"
#include "platform.h"
#include "storage_paths.h"
#include "error.h"
#include "version.h"

#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>

/* ---- Fixture helpers ---- */

static char *fixture_dir = NULL;

static void create_fixture_dir(void)
{
    fixture_dir = tt_test_tmpdir();
}

static void cleanup_fixture(void)
{
    if (fixture_dir)
    {
        tt_test_rmdir(fixture_dir);
        free(fixture_dir);
        fixture_dir = NULL;
    }
}

static void create_indexed_project(void)
{
    create_fixture_dir();
    tt_test_write_file(fixture_dir, "package.json", "{\"name\":\"test\"}");
    tt_test_write_file(fixture_dir, "src/main.c",
                       "#include <stdio.h>\n"
                       "int main(int argc, char **argv) {\n"
                       "    printf(\"hello\\n\");\n"
                       "    return 0;\n"
                       "}\n");
    tt_test_write_file(fixture_dir, "src/util.c",
                       "int add(int a, int b) { return a + b; }\n"
                       "int multiply(int a, int b) { return a * b; }\n");

    tt_cli_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.truncate_width = 120;
    opts.path = fixture_dir;
    cJSON *result = tt_cmd_index_create_exec(&opts);
    if (result)
        cJSON_Delete(result);
}

/* ---- Pipe-based server interaction ---- */

typedef struct
{
    pid_t pid;
    int write_fd; /* parent writes here (server's stdin) */
    int read_fd;  /* parent reads here (server's stdout) */
} mcp_test_server_t;

static int start_test_server(mcp_test_server_t *ts, const char *project_root)
{
    int stdin_pipe[2], stdout_pipe[2];
    if (pipe(stdin_pipe) < 0 || pipe(stdout_pipe) < 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0)
        return -1;

    if (pid == 0)
    {
        /* Child: redirect stdin/stdout to pipes */
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        close(stdin_pipe[0]);
        close(stdout_pipe[1]);

        tt_mcp_server_t srv;
        tt_mcp_server_init(&srv, project_root);
        tt_mcp_server_run(&srv);
        tt_mcp_server_free(&srv);
        _exit(0);
    }

    /* Parent */
    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    ts->pid = pid;
    ts->write_fd = stdin_pipe[1];
    ts->read_fd = stdout_pipe[0];
    return 0;
}

static void send_message(mcp_test_server_t *ts, const char *json_str)
{
    ssize_t r;
    r = write(ts->write_fd, json_str, strlen(json_str));
    (void)r;
    r = write(ts->write_fd, "\n", 1);
    (void)r;
}

/*
 * Read one line from server. Returns dynamically allocated string
 * (caller frees), or NULL on timeout/error.
 */
static char *read_response(mcp_test_server_t *ts, int timeout_ms)
{
    /* Deadline-based read: select() before every read() to avoid blocking
     * when the server hasn't finished writing the full line yet. */
    uint64_t deadline = tt_monotonic_ms() + (uint64_t)timeout_ms;

    char buf[65536];
    int pos = 0;
    while (pos < (int)sizeof(buf) - 1)
    {
        uint64_t now = tt_monotonic_ms();
        if (now >= deadline)
            break;
        int remaining = (int)(deadline - now);

        fd_set fds;
        struct timeval tv;
        FD_ZERO(&fds);
        FD_SET(ts->read_fd, &fds);
        tv.tv_sec = remaining / 1000;
        tv.tv_usec = (remaining % 1000) * 1000;

        int ret = select(ts->read_fd + 1, &fds, NULL, NULL, &tv);
        if (ret <= 0)
            break;

        char c;
        ssize_t n = read(ts->read_fd, &c, 1);
        if (n <= 0)
            break;
        if (c == '\n')
            break;
        buf[pos++] = c;
    }
    buf[pos] = '\0';
    if (pos == 0)
        return NULL;
    return strdup(buf);
}

static cJSON *read_json_response(mcp_test_server_t *ts, int timeout_ms)
{
    char *line = read_response(ts, timeout_ms);
    if (!line)
        return NULL;
    cJSON *json = cJSON_Parse(line);
    free(line);
    return json;
}

static void stop_test_server(mcp_test_server_t *ts)
{
    close(ts->write_fd);
    int status;
    waitpid(ts->pid, &status, 0);
    close(ts->read_fd);
}

/* Send initialize and expect success */
static int do_initialize(mcp_test_server_t *ts)
{
    send_message(ts, "{\"jsonrpc\":\"2.0\",\"method\":\"initialize\",\"id\":1,"
                     "\"params\":{\"protocolVersion\":\"2025-11-25\","
                     "\"clientInfo\":{\"name\":\"test\",\"version\":\"1.0\"}}}");
    cJSON *resp = read_json_response(ts, 2000);
    if (!resp)
        return -1;
    cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
    int ok = (result != NULL) ? 0 : -1;
    cJSON_Delete(resp);
    return ok;
}

/* ================================================================
 * A. Test JSON-RPC 2.0 parsing e dispatch
 * ================================================================ */

TT_TEST(test_int_mcp_jsonrpc_parse_error)
{
    mcp_test_server_t ts;
    TT_ASSERT(start_test_server(&ts, "/tmp") == 0, "start server");

    /* Parse error (not JSON) */
    send_message(&ts, "{not json at all}");
    cJSON *resp = read_json_response(&ts, 2000);
    TT_ASSERT_NOT_NULL(resp);
    cJSON *err = cJSON_GetObjectItemCaseSensitive(resp, "error");
    TT_ASSERT_NOT_NULL(err);
    TT_ASSERT_EQ_INT(cJSON_GetObjectItemCaseSensitive(err, "code")->valueint, -32700);
    cJSON_Delete(resp);

    /* Missing method */
    send_message(&ts, "{\"id\":1}");
    resp = read_json_response(&ts, 2000);
    TT_ASSERT_NOT_NULL(resp);
    err = cJSON_GetObjectItemCaseSensitive(resp, "error");
    TT_ASSERT_NOT_NULL(err);
    TT_ASSERT_EQ_INT(cJSON_GetObjectItemCaseSensitive(err, "code")->valueint, -32600);
    cJSON_Delete(resp);

    /* Method not a string */
    send_message(&ts, "{\"method\":42,\"id\":2}");
    resp = read_json_response(&ts, 2000);
    TT_ASSERT_NOT_NULL(resp);
    err = cJSON_GetObjectItemCaseSensitive(resp, "error");
    TT_ASSERT_EQ_INT(cJSON_GetObjectItemCaseSensitive(err, "code")->valueint, -32600);
    cJSON_Delete(resp);

    /* JSON array (not object) */
    send_message(&ts, "[1,2,3]");
    resp = read_json_response(&ts, 2000);
    TT_ASSERT_NOT_NULL(resp);
    err = cJSON_GetObjectItemCaseSensitive(resp, "error");
    TT_ASSERT_EQ_INT(cJSON_GetObjectItemCaseSensitive(err, "code")->valueint, -32600);
    cJSON_Delete(resp);

    /* Unknown method with id -> error -32601 (after init) */
    TT_ASSERT(do_initialize(&ts) == 0, "initialize for unknown method test");

    send_message(&ts, "{\"jsonrpc\":\"2.0\",\"method\":\"unknown\",\"id\":10}");
    resp = read_json_response(&ts, 2000);
    TT_ASSERT_NOT_NULL(resp);
    err = cJSON_GetObjectItemCaseSensitive(resp, "error");
    TT_ASSERT_EQ_INT(cJSON_GetObjectItemCaseSensitive(err, "code")->valueint, -32601);
    cJSON_Delete(resp);

    /* Notification (method, no id) -> no response */
    send_message(&ts, "{\"jsonrpc\":\"2.0\",\"method\":\"some/notification\"}");
    resp = read_json_response(&ts, 500);
    TT_ASSERT_NULL(resp);

    /* Verify response structure: jsonrpc and id */
    send_message(&ts, "{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"id\":99}");
    resp = read_json_response(&ts, 2000);
    TT_ASSERT_NOT_NULL(resp);
    const char *jsonrpc = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(resp, "jsonrpc"));
    TT_ASSERT(jsonrpc && strcmp(jsonrpc, "2.0") == 0, "response has jsonrpc 2.0");
    TT_ASSERT_EQ_INT(cJSON_GetObjectItemCaseSensitive(resp, "id")->valueint, 99);
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(resp, "result"));
    TT_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(resp, "error"));
    cJSON_Delete(resp);

    stop_test_server(&ts);
}

/* ================================================================
 * B. Test initialize handshake
 * ================================================================ */

TT_TEST(test_int_mcp_initialize)
{
    mcp_test_server_t ts;
    TT_ASSERT(start_test_server(&ts, "/tmp") == 0, "start server");

    /* Initialize */
    send_message(&ts, "{\"jsonrpc\":\"2.0\",\"method\":\"initialize\",\"id\":1,"
                      "\"params\":{\"protocolVersion\":\"2025-11-25\","
                      "\"clientInfo\":{\"name\":\"test\",\"version\":\"1.0\"}}}");
    cJSON *resp = read_json_response(&ts, 2000);
    TT_ASSERT_NOT_NULL(resp);

    cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
    TT_ASSERT_NOT_NULL(result);

    const char *proto = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(result, "protocolVersion"));
    TT_ASSERT(proto && strcmp(proto, TT_MCP_PROTOCOL_VERSION) == 0,
              "protocolVersion matches");

    cJSON *caps = cJSON_GetObjectItemCaseSensitive(result, "capabilities");
    cJSON *tools_cap = cJSON_GetObjectItemCaseSensitive(caps, "tools");
    TT_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(tools_cap, "listChanged")));

    cJSON *srv_info = cJSON_GetObjectItemCaseSensitive(result, "serverInfo");
    const char *name = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(srv_info, "name"));
    TT_ASSERT_EQ_STR(name, "toktoken");

    const char *ver = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(srv_info, "version"));
    TT_ASSERT_EQ_STR(ver, TT_VERSION);
    cJSON_Delete(resp);

    /* Double initialize -> error */
    send_message(&ts, "{\"jsonrpc\":\"2.0\",\"method\":\"initialize\",\"id\":2,"
                      "\"params\":{}}");
    resp = read_json_response(&ts, 2000);
    TT_ASSERT_NOT_NULL(resp);
    cJSON *err = cJSON_GetObjectItemCaseSensitive(resp, "error");
    TT_ASSERT_NOT_NULL(err);
    TT_ASSERT_EQ_INT(cJSON_GetObjectItemCaseSensitive(err, "code")->valueint, -32600);
    cJSON_Delete(resp);

    stop_test_server(&ts);

    /* tools/call before initialize -> -32002 */
    TT_ASSERT(start_test_server(&ts, "/tmp") == 0, "start server for uninit test");
    send_message(&ts, "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"id\":1,"
                      "\"params\":{\"name\":\"ping\"}}");
    resp = read_json_response(&ts, 2000);
    TT_ASSERT_NOT_NULL(resp);
    err = cJSON_GetObjectItemCaseSensitive(resp, "error");
    TT_ASSERT_EQ_INT(cJSON_GetObjectItemCaseSensitive(err, "code")->valueint, -32002);
    cJSON_Delete(resp);

    /* tools/list before initialize -> -32002 */
    send_message(&ts, "{\"jsonrpc\":\"2.0\",\"method\":\"tools/list\",\"id\":2}");
    resp = read_json_response(&ts, 2000);
    err = cJSON_GetObjectItemCaseSensitive(resp, "error");
    TT_ASSERT_EQ_INT(cJSON_GetObjectItemCaseSensitive(err, "code")->valueint, -32002);
    cJSON_Delete(resp);

    /* ping before initialize -> -32002 */
    send_message(&ts, "{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"id\":3}");
    resp = read_json_response(&ts, 2000);
    err = cJSON_GetObjectItemCaseSensitive(resp, "error");
    TT_ASSERT_EQ_INT(cJSON_GetObjectItemCaseSensitive(err, "code")->valueint, -32002);
    cJSON_Delete(resp);

    /* notifications/initialized without prior initialize -> no crash, no response */
    send_message(&ts, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
    resp = read_json_response(&ts, 500);
    TT_ASSERT_NULL(resp);

    /* initialize with NULL params -> no crash */
    send_message(&ts, "{\"jsonrpc\":\"2.0\",\"method\":\"initialize\",\"id\":4}");
    resp = read_json_response(&ts, 2000);
    TT_ASSERT_NOT_NULL(resp);
    result = cJSON_GetObjectItemCaseSensitive(resp, "result");
    TT_ASSERT_NOT_NULL(result);
    cJSON_Delete(resp);

    stop_test_server(&ts);
}

/* ================================================================
 * C. Test tools/list -- schema validation
 * ================================================================ */

TT_TEST(test_int_mcp_tools_list)
{
    mcp_test_server_t ts;
    TT_ASSERT(start_test_server(&ts, "/tmp") == 0, "start server");
    TT_ASSERT(do_initialize(&ts) == 0, "initialize");

    send_message(&ts, "{\"jsonrpc\":\"2.0\",\"method\":\"tools/list\",\"id\":2}");
    cJSON *resp = read_json_response(&ts, 2000);
    TT_ASSERT_NOT_NULL(resp);

    cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
    cJSON *tools = cJSON_GetObjectItemCaseSensitive(result, "tools");
    TT_ASSERT_TRUE(cJSON_IsArray(tools));
    TT_ASSERT_EQ_INT(cJSON_GetArraySize(tools), 26);

    /* Check expected tool names */
    const char *expected_names[] = {
        "codebase_detect", "index_create", "index_update",
        "search_symbols", "search_text",
        "inspect_outline", "inspect_symbol", "inspect_file", "inspect_tree",
        "stats", "projects_list", "cache_clear",
        "index_github", "inspect_bundle", "find_importers", "find_references",
        "find_callers", "search_cooccurrence", "search_similar",
        "inspect_dependencies", "inspect_hierarchy",
        "inspect_cycles", "inspect_blast_radius", "find_dead", "index_file",
        "help"};

    for (int i = 0; i < 26; i++)
    {
        cJSON *tool = cJSON_GetArrayItem(tools, i);
        const char *tname = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(tool, "name"));
        TT_ASSERT(tname && strlen(tname) > 0, "tool has non-empty name");
        TT_ASSERT_EQ_STR(tname, expected_names[i]);

        const char *desc = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(tool, "description"));
        TT_ASSERT(desc && strlen(desc) > 0, "tool has non-empty description");

        cJSON *schema = cJSON_GetObjectItemCaseSensitive(tool, "inputSchema");
        TT_ASSERT_TRUE(cJSON_IsObject(schema));

        const char *type = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(schema, "type"));
        TT_ASSERT_EQ_STR(type, "object");

        cJSON *props = cJSON_GetObjectItemCaseSensitive(schema, "properties");
        TT_ASSERT_TRUE(cJSON_IsObject(props));
    }

    /* Validate search_symbols schema specifically */
    cJSON *ss_tool = cJSON_GetArrayItem(tools, 3); /* search_symbols */
    cJSON *ss_schema = cJSON_GetObjectItemCaseSensitive(ss_tool, "inputSchema");
    cJSON *ss_props = cJSON_GetObjectItemCaseSensitive(ss_schema, "properties");
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(ss_props, "query"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(ss_props, "kind"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(ss_props, "compact"));
    cJSON *ss_req = cJSON_GetObjectItemCaseSensitive(ss_schema, "required");
    TT_ASSERT_TRUE(cJSON_IsArray(ss_req));
    TT_ASSERT_EQ_INT(cJSON_GetArraySize(ss_req), 1);
    TT_ASSERT_EQ_STR(cJSON_GetArrayItem(ss_req, 0)->valuestring, "query");

    /* Validate inspect_tree (no required) */
    cJSON *it_tool = cJSON_GetArrayItem(tools, 8); /* inspect_tree */
    cJSON *it_schema = cJSON_GetObjectItemCaseSensitive(it_tool, "inputSchema");
    cJSON *it_req = cJSON_GetObjectItemCaseSensitive(it_schema, "required");
    TT_ASSERT_NULL(it_req);

    /* Validate property types */
    cJSON *q = cJSON_GetObjectItemCaseSensitive(ss_props, "query");
    TT_ASSERT_EQ_STR(cJSON_GetStringValue(
                         cJSON_GetObjectItemCaseSensitive(q, "type")),
                     "string");
    cJSON *lim = cJSON_GetObjectItemCaseSensitive(ss_props, "limit");
    TT_ASSERT_EQ_STR(cJSON_GetStringValue(
                         cJSON_GetObjectItemCaseSensitive(lim, "type")),
                     "integer");
    cJSON *cmp = cJSON_GetObjectItemCaseSensitive(ss_props, "compact");
    TT_ASSERT_EQ_STR(cJSON_GetStringValue(
                         cJSON_GetObjectItemCaseSensitive(cmp, "type")),
                     "boolean");

    cJSON_Delete(resp);
    stop_test_server(&ts);
}

/* ================================================================
 * D. Test tools/call dispatch and MCP response format
 * ================================================================ */

TT_TEST(test_int_mcp_tools_call_dispatch)
{
    mcp_test_server_t ts;
    TT_ASSERT(start_test_server(&ts, "/tmp") == 0, "start server");
    TT_ASSERT(do_initialize(&ts) == 0, "initialize");

    /* Unknown tool -> -32602 */
    send_message(&ts, "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"id\":2,"
                      "\"params\":{\"name\":\"nonexistent\"}}");
    cJSON *resp = read_json_response(&ts, 2000);
    TT_ASSERT_NOT_NULL(resp);
    cJSON *err = cJSON_GetObjectItemCaseSensitive(resp, "error");
    TT_ASSERT_EQ_INT(cJSON_GetObjectItemCaseSensitive(err, "code")->valueint, -32602);
    cJSON_Delete(resp);

    /* Missing tool name -> -32602 */
    send_message(&ts, "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"id\":3,"
                      "\"params\":{\"arguments\":{}}}");
    resp = read_json_response(&ts, 2000);
    err = cJSON_GetObjectItemCaseSensitive(resp, "error");
    TT_ASSERT_EQ_INT(cJSON_GetObjectItemCaseSensitive(err, "code")->valueint, -32602);
    cJSON_Delete(resp);

    /* arguments=NULL -> no crash (codebase_detect works without args) */
    send_message(&ts, "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"id\":4,"
                      "\"params\":{\"name\":\"codebase_detect\"}}");
    resp = read_json_response(&ts, 2000);
    TT_ASSERT_NOT_NULL(resp);
    cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
    TT_ASSERT_NOT_NULL(result);
    cJSON *content = cJSON_GetObjectItemCaseSensitive(result, "content");
    TT_ASSERT_TRUE(cJSON_IsArray(content));
    TT_ASSERT_GE_INT(cJSON_GetArraySize(content), 1);
    cJSON *first = cJSON_GetArrayItem(content, 0);
    TT_ASSERT_EQ_STR(cJSON_GetStringValue(
                         cJSON_GetObjectItemCaseSensitive(first, "type")),
                     "text");
    const char *text = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(first, "text"));
    TT_ASSERT_NOT_NULL(text);
    /* text should be valid JSON */
    cJSON *inner = cJSON_Parse(text);
    TT_ASSERT_NOT_NULL(inner);
    cJSON_Delete(inner);
    cJSON *is_error = cJSON_GetObjectItemCaseSensitive(result, "isError");
    TT_ASSERT_TRUE(cJSON_IsBool(is_error));
    cJSON_Delete(resp);

    /* Tool-level error: search_symbols without query -> isError=true */
    send_message(&ts, "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"id\":5,"
                      "\"params\":{\"name\":\"search_symbols\",\"arguments\":{}}}");
    resp = read_json_response(&ts, 2000);
    result = cJSON_GetObjectItemCaseSensitive(resp, "result");
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "isError")));
    content = cJSON_GetObjectItemCaseSensitive(result, "content");
    text = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(content, 0), "text"));
    inner = cJSON_Parse(text);
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(inner, "error"));
    cJSON_Delete(inner);
    cJSON_Delete(resp);

    stop_test_server(&ts);
}

/* ================================================================
 * E. Test tools/call execution per tool
 * ================================================================ */

TT_TEST(test_int_mcp_tools_execution)
{
    create_indexed_project();

    mcp_test_server_t ts;
    TT_ASSERT(start_test_server(&ts, fixture_dir) == 0, "start server with fixture");
    TT_ASSERT(do_initialize(&ts) == 0, "initialize");

    /* codebase_detect */
    {
        char msg[1024];
        snprintf(msg, sizeof(msg),
                 "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"id\":10,"
                 "\"params\":{\"name\":\"codebase_detect\",\"arguments\":{\"path\":\"%s\"}}}",
                 fixture_dir);
        send_message(&ts, msg);
        cJSON *resp = read_json_response(&ts, 3000);
        TT_ASSERT_NOT_NULL(resp);
        cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
        TT_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "isError")));
        cJSON_Delete(resp);
    }

    /* stats */
    {
        char msg[1024];
        snprintf(msg, sizeof(msg),
                 "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"id\":11,"
                 "\"params\":{\"name\":\"stats\",\"arguments\":{\"path\":\"%s\"}}}",
                 fixture_dir);
        send_message(&ts, msg);
        cJSON *resp = read_json_response(&ts, 3000);
        TT_ASSERT_NOT_NULL(resp);
        cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
        TT_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "isError")));
        /* Parse inner text and check for files/symbols */
        cJSON *content = cJSON_GetObjectItemCaseSensitive(result, "content");
        const char *text = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(content, 0), "text"));
        cJSON *inner = cJSON_Parse(text);
        TT_ASSERT_NOT_NULL(inner);
        TT_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(inner, "files"));
        TT_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(inner, "symbols"));
        cJSON_Delete(inner);
        cJSON_Delete(resp);
    }

    /* search_symbols */
    {
        char msg[1024];
        snprintf(msg, sizeof(msg),
                 "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"id\":12,"
                 "\"params\":{\"name\":\"search_symbols\",\"arguments\":"
                 "{\"query\":\"main\",\"path\":\"%s\"}}}",
                 fixture_dir);
        send_message(&ts, msg);
        cJSON *resp = read_json_response(&ts, 3000);
        TT_ASSERT_NOT_NULL(resp);
        cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
        TT_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "isError")));
        cJSON_Delete(resp);
    }

    /* search_symbols without query -> isError=true */
    {
        send_message(&ts, "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"id\":13,"
                          "\"params\":{\"name\":\"search_symbols\",\"arguments\":{}}}");
        cJSON *resp = read_json_response(&ts, 2000);
        cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
        TT_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "isError")));
        cJSON_Delete(resp);
    }

    /* inspect_tree */
    {
        char msg[1024];
        snprintf(msg, sizeof(msg),
                 "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"id\":14,"
                 "\"params\":{\"name\":\"inspect_tree\",\"arguments\":{\"path\":\"%s\"}}}",
                 fixture_dir);
        send_message(&ts, msg);
        cJSON *resp = read_json_response(&ts, 3000);
        TT_ASSERT_NOT_NULL(resp);
        cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
        TT_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "isError")));
        cJSON_Delete(resp);
    }

    /* projects_list */
    {
        send_message(&ts, "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"id\":15,"
                          "\"params\":{\"name\":\"projects_list\",\"arguments\":{}}}");
        cJSON *resp = read_json_response(&ts, 3000);
        TT_ASSERT_NOT_NULL(resp);
        cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
        TT_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "isError")));
        cJSON_Delete(resp);
    }

    /* inspect_file missing file -> isError=true */
    {
        send_message(&ts, "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"id\":16,"
                          "\"params\":{\"name\":\"inspect_file\",\"arguments\":{}}}");
        cJSON *resp = read_json_response(&ts, 2000);
        cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
        TT_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "isError")));
        cJSON_Delete(resp);
    }

    stop_test_server(&ts);
    cleanup_fixture();
}

/* ================================================================
 * F. Test parameter defaults and propagation
 * ================================================================ */

TT_TEST(test_int_mcp_parameter_defaults)
{
    mcp_test_server_t ts;
    TT_ASSERT(start_test_server(&ts, "/tmp") == 0, "start server");
    TT_ASSERT(do_initialize(&ts) == 0, "initialize");

    /* Extra unknown parameters -> ignored (no crash) */
    send_message(&ts, "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"id\":2,"
                      "\"params\":{\"name\":\"codebase_detect\","
                      "\"arguments\":{\"path\":\"/tmp\",\"unknown_param\":\"value\","
                      "\"another\":42}}}");
    cJSON *resp = read_json_response(&ts, 2000);
    TT_ASSERT_NOT_NULL(resp);
    cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
    TT_ASSERT_NOT_NULL(result);
    cJSON_Delete(resp);

    stop_test_server(&ts);
}

/* ================================================================
 * G. Integration STDIO -- pipe-based lifecycle
 * ================================================================ */

TT_TEST(test_int_mcp_stdio_lifecycle)
{
    mcp_test_server_t ts;
    TT_ASSERT(start_test_server(&ts, "/tmp") == 0, "start server");

    /* initialize */
    TT_ASSERT(do_initialize(&ts) == 0, "initialize");

    /* notifications/initialized (no response) */
    send_message(&ts, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
    cJSON *resp = read_json_response(&ts, 500);
    TT_ASSERT_NULL(resp);

    /* tools/list */
    send_message(&ts, "{\"jsonrpc\":\"2.0\",\"method\":\"tools/list\",\"id\":2}");
    resp = read_json_response(&ts, 2000);
    TT_ASSERT_NOT_NULL(resp);
    cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
    cJSON *tools = cJSON_GetObjectItemCaseSensitive(result, "tools");
    TT_ASSERT_EQ_INT(cJSON_GetArraySize(tools), 26);
    cJSON_Delete(resp);

    /* ping */
    send_message(&ts, "{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"id\":3}");
    resp = read_json_response(&ts, 2000);
    TT_ASSERT_NOT_NULL(resp);
    result = cJSON_GetObjectItemCaseSensitive(resp, "result");
    TT_ASSERT_TRUE(cJSON_IsObject(result));
    cJSON_Delete(resp);

    /* EOF -> clean shutdown */
    close(ts.write_fd);
    int status;
    waitpid(ts.pid, &status, 0);
    TT_ASSERT_TRUE(WIFEXITED(status));
    TT_ASSERT_EQ_INT(WEXITSTATUS(status), 0);
    close(ts.read_fd);
}

/* ================================================================
 * H. STDIO edge cases -- robustness
 * ================================================================ */

TT_TEST(test_int_mcp_stdio_robustness)
{
    mcp_test_server_t ts;
    TT_ASSERT(start_test_server(&ts, "/tmp") == 0, "start server");
    TT_ASSERT(do_initialize(&ts) == 0, "initialize");

    /* Empty lines -> ignored */
    {
        ssize_t r = write(ts.write_fd, "\n\n\n", 3);
        (void)r;
    }
    cJSON *resp = read_json_response(&ts, 500);
    TT_ASSERT_NULL(resp);

    /* JSON string */
    send_message(&ts, "\"just a string\"");
    resp = read_json_response(&ts, 2000);
    TT_ASSERT_NOT_NULL(resp);
    cJSON *err = cJSON_GetObjectItemCaseSensitive(resp, "error");
    TT_ASSERT_EQ_INT(cJSON_GetObjectItemCaseSensitive(err, "code")->valueint, -32600);
    cJSON_Delete(resp);

    /* JSON number */
    send_message(&ts, "42");
    resp = read_json_response(&ts, 2000);
    TT_ASSERT_NOT_NULL(resp);
    err = cJSON_GetObjectItemCaseSensitive(resp, "error");
    TT_ASSERT_EQ_INT(cJSON_GetObjectItemCaseSensitive(err, "code")->valueint, -32600);
    cJSON_Delete(resp);

    /* JSON null */
    send_message(&ts, "null");
    resp = read_json_response(&ts, 2000);
    TT_ASSERT_NOT_NULL(resp);
    err = cJSON_GetObjectItemCaseSensitive(resp, "error");
    TT_ASSERT_EQ_INT(cJSON_GetObjectItemCaseSensitive(err, "code")->valueint, -32600);
    cJSON_Delete(resp);

    /* Unicode in method */
    send_message(&ts, "{\"method\":\"\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\",\"id\":1}");
    resp = read_json_response(&ts, 2000);
    TT_ASSERT_NOT_NULL(resp);
    err = cJSON_GetObjectItemCaseSensitive(resp, "error");
    TT_ASSERT_EQ_INT(cJSON_GetObjectItemCaseSensitive(err, "code")->valueint, -32601);
    cJSON_Delete(resp);

    /* String ID */
    send_message(&ts, "{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"id\":\"str-id\"}");
    resp = read_json_response(&ts, 2000);
    TT_ASSERT_NOT_NULL(resp);
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(resp, "id");
    TT_ASSERT_TRUE(cJSON_IsString(id));
    TT_ASSERT_EQ_STR(id->valuestring, "str-id");
    cJSON_Delete(resp);

    /* Burst of errors followed by valid message */
    for (int i = 0; i < 5; i++)
    {
        send_message(&ts, "{invalid json}");
        resp = read_json_response(&ts, 2000);
        cJSON_Delete(resp);
    }
    send_message(&ts, "{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"id\":99}");
    resp = read_json_response(&ts, 2000);
    TT_ASSERT_NOT_NULL(resp);
    cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
    TT_ASSERT_NOT_NULL(result);
    cJSON_Delete(resp);

    stop_test_server(&ts);
}

/* ================================================================
 * I. EOF and shutdown
 * ================================================================ */

TT_TEST(test_int_mcp_eof_shutdown)
{
    /* Immediate EOF */
    {
        mcp_test_server_t ts;
        TT_ASSERT(start_test_server(&ts, "/tmp") == 0, "start for immediate EOF");
        close(ts.write_fd);
        int status;
        waitpid(ts.pid, &status, 0);
        TT_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                  "immediate EOF -> exit 0");
        close(ts.read_fd);
    }

    /* EOF after initialize */
    {
        mcp_test_server_t ts;
        TT_ASSERT(start_test_server(&ts, "/tmp") == 0, "start for EOF after init");
        do_initialize(&ts);
        close(ts.write_fd);
        int status;
        waitpid(ts.pid, &status, 0);
        TT_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                  "EOF after init -> exit 0");
        close(ts.read_fd);
    }

    /* EOF mid-message (partial JSON) */
    {
        mcp_test_server_t ts;
        TT_ASSERT(start_test_server(&ts, "/tmp") == 0, "start for partial EOF");
        /* Write partial JSON without newline, then close */
        const char *partial = "{\"method\":\"ini";
        {
            ssize_t r = write(ts.write_fd, partial, strlen(partial));
            (void)r;
        }
        close(ts.write_fd);
        int status;
        waitpid(ts.pid, &status, 0);
        TT_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                  "partial EOF -> exit 0 (no hang)");
        close(ts.read_fd);
    }
}

/* ================================================================
 * J. Signal handling
 * ================================================================ */

TT_TEST(test_int_mcp_signals)
{
    /* SIGINT during wait */
    {
        mcp_test_server_t ts;
        TT_ASSERT(start_test_server(&ts, "/tmp") == 0, "start for SIGINT");
        usleep(100000); /* let server block on fgets */
        kill(ts.pid, SIGINT);
        int status;
        waitpid(ts.pid, &status, 0);
        TT_ASSERT(WIFEXITED(status) || WIFSIGNALED(status),
                  "SIGINT -> process terminated");
        close(ts.write_fd);
        close(ts.read_fd);
    }

    /* SIGTERM during wait */
    {
        mcp_test_server_t ts;
        TT_ASSERT(start_test_server(&ts, "/tmp") == 0, "start for SIGTERM");
        usleep(100000);
        kill(ts.pid, SIGTERM);
        int status;
        waitpid(ts.pid, &status, 0);
        TT_ASSERT(WIFEXITED(status) || WIFSIGNALED(status),
                  "SIGTERM -> process terminated");
        close(ts.write_fd);
        close(ts.read_fd);
    }

    /* SIGINT after initialize */
    {
        mcp_test_server_t ts;
        TT_ASSERT(start_test_server(&ts, "/tmp") == 0, "start for SIGINT after init");
        do_initialize(&ts);
        usleep(100000);
        kill(ts.pid, SIGINT);
        int status;
        waitpid(ts.pid, &status, 0);
        TT_ASSERT(WIFEXITED(status) || WIFSIGNALED(status),
                  "SIGINT after init -> terminated");
        close(ts.write_fd);
        close(ts.read_fd);
    }
}

/* ================================================================
 * K. Broken pipe (stdout)
 * ================================================================ */

TT_TEST(test_int_mcp_broken_pipe)
{
    mcp_test_server_t ts;
    TT_ASSERT(start_test_server(&ts, "/tmp") == 0, "start server");

    /* Initialize, then close our read end of stdout pipe */
    do_initialize(&ts);
    close(ts.read_fd);

    /* Send another message -- server should detect write error and exit */
    send_message(&ts, "{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"id\":2}");
    usleep(200000);

    int status;
    int waited = waitpid(ts.pid, &status, WNOHANG);
    if (waited == 0)
    {
        /* Server still running, give it more time */
        usleep(500000);
        waitpid(ts.pid, &status, WNOHANG);
    }
    /* Server should have exited (or will exit on next write) */
    close(ts.write_fd);
    waitpid(ts.pid, &status, 0);
    TT_ASSERT(WIFEXITED(status) || WIFSIGNALED(status),
              "broken pipe -> server terminated");
}

/* ================================================================
 * L. Lazy database opening
 * ================================================================ */

TT_TEST(test_int_mcp_lazy_db)
{
    mcp_test_server_t ts;
    TT_ASSERT(start_test_server(&ts, "/tmp") == 0, "start server");
    TT_ASSERT(do_initialize(&ts) == 0, "initialize");

    /* codebase_detect without index -> works (no db needed) */
    send_message(&ts, "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"id\":2,"
                      "\"params\":{\"name\":\"codebase_detect\","
                      "\"arguments\":{\"path\":\"/tmp\"}}}");
    cJSON *resp = read_json_response(&ts, 2000);
    TT_ASSERT_NOT_NULL(resp);
    cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
    TT_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "isError")));
    cJSON_Delete(resp);

    /* projects_list -> works without db */
    send_message(&ts, "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"id\":3,"
                      "\"params\":{\"name\":\"projects_list\",\"arguments\":{}}}");
    resp = read_json_response(&ts, 2000);
    TT_ASSERT_NOT_NULL(resp);
    result = cJSON_GetObjectItemCaseSensitive(resp, "result");
    TT_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "isError")));
    cJSON_Delete(resp);

    /* search_symbols without index -> isError=true */
    send_message(&ts, "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"id\":4,"
                      "\"params\":{\"name\":\"search_symbols\","
                      "\"arguments\":{\"query\":\"test\",\"path\":\"/tmp/nonexistent_dir_xyz\"}}}");
    resp = read_json_response(&ts, 2000);
    TT_ASSERT_NOT_NULL(resp);
    result = cJSON_GetObjectItemCaseSensitive(resp, "result");
    TT_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "isError")));
    cJSON_Delete(resp);

    stop_test_server(&ts);
}

/* ================================================================
 * M. _ttk response envelope
 * ================================================================ */

TT_TEST(test_int_mcp_ttk_envelope)
{
    mcp_test_server_t ts;
    TT_ASSERT(start_test_server(&ts, "/tmp") == 0, "start server");
    TT_ASSERT(do_initialize(&ts) == 0, "initialize");

    /* Call codebase_detect and check _ttk in text.
     * codebase_detect does real disk I/O (stat, file discovery on /tmp),
     * so allow a generous timeout to avoid flakiness on slow CI runners. */
    send_message(&ts, "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"id\":2,"
                      "\"params\":{\"name\":\"codebase_detect\","
                      "\"arguments\":{\"path\":\"/tmp\"}}}");
    cJSON *resp = read_json_response(&ts, 5000);
    TT_ASSERT_NOT_NULL(resp);

    cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
    cJSON *content = cJSON_GetObjectItemCaseSensitive(result, "content");
    const char *text = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(content, 0), "text"));

    cJSON *inner = cJSON_Parse(text);
    TT_ASSERT_NOT_NULL(inner);

    cJSON *tt_env = cJSON_GetObjectItemCaseSensitive(inner, "_ttk");
    TT_ASSERT_NOT_NULL(tt_env);

    const char *ts_val = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(tt_env, "timestamp"));
    TT_ASSERT(ts_val != NULL && strlen(ts_val) > 0, "_ttk.timestamp non-empty");

    cJSON *dur = cJSON_GetObjectItemCaseSensitive(tt_env, "duration_ms");
    TT_ASSERT(dur != NULL && cJSON_IsNumber(dur), "_ttk.duration_ms is number");
    TT_ASSERT_GE_INT(dur->valueint, 0);

    const char *version = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(tt_env, "version"));
    TT_ASSERT_EQ_STR(version, TT_VERSION);

    cJSON_Delete(inner);
    cJSON_Delete(resp);

    /* _ttk also present for tool errors */
    send_message(&ts, "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"id\":3,"
                      "\"params\":{\"name\":\"search_symbols\",\"arguments\":{}}}");
    resp = read_json_response(&ts, 5000);
    result = cJSON_GetObjectItemCaseSensitive(resp, "result");
    content = cJSON_GetObjectItemCaseSensitive(result, "content");
    text = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(content, 0), "text"));
    inner = cJSON_Parse(text);
    TT_ASSERT_NOT_NULL(inner);
    tt_env = cJSON_GetObjectItemCaseSensitive(inner, "_ttk");
    TT_ASSERT_NOT_NULL(tt_env);
    cJSON_Delete(inner);
    cJSON_Delete(resp);

    stop_test_server(&ts);
}

/* ================================================================
 * Runner
 * ================================================================ */

void run_int_mcp_server_tests(void)
{
    TT_SUITE("Integration: MCP Server");

    TT_RUN(test_int_mcp_jsonrpc_parse_error); /* A */
    TT_RUN(test_int_mcp_initialize);          /* B */
    TT_RUN(test_int_mcp_tools_list);          /* C */
    TT_RUN(test_int_mcp_tools_call_dispatch); /* D */
    TT_RUN(test_int_mcp_tools_execution);     /* E */
    TT_RUN(test_int_mcp_parameter_defaults);  /* F */
    TT_RUN(test_int_mcp_stdio_lifecycle);     /* G */
    TT_RUN(test_int_mcp_stdio_robustness);    /* H */
    TT_RUN(test_int_mcp_eof_shutdown);        /* I */
    TT_RUN(test_int_mcp_signals);             /* J */
    TT_RUN(test_int_mcp_broken_pipe);         /* K */
    TT_RUN(test_int_mcp_lazy_db);             /* L */
    TT_RUN(test_int_mcp_ttk_envelope);         /* M */
}
