/*
 * test_e2e_helpers.h -- Helpers for E2E tests using popen.
 */

#ifndef TT_TEST_E2E_HELPERS_H
#define TT_TEST_E2E_HELPERS_H

#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * tt_e2e_run -- Run toktoken binary and capture stdout.
 *
 * cmd_args: arguments after "toktoken" (e.g. "index:create --path /tmp/x").
 * out_json: receives parsed cJSON (caller must cJSON_Delete). NULL if parse fails.
 * Returns exit code from pclose.
 */
static inline const char *tt_e2e_binary(void)
{
    static char path[512];
    static int resolved = 0;
    if (resolved) return path[0] ? path : NULL;
    resolved = 1;

    /* Allow override via environment variable */
    const char *env = getenv("TOKTOKEN_BIN");
    if (env && access(env, X_OK) == 0) {
        snprintf(path, sizeof(path), "%s", env);
        return path;
    }

    const char *candidates[] = {
        "./toktoken",
        "./build/toktoken",
        "./build/debug/toktoken",
        "../build/toktoken",
        "../build/debug/toktoken",
        NULL
    };
    for (int i = 0; candidates[i]; i++) {
        if (access(candidates[i], X_OK) == 0) {
            snprintf(path, sizeof(path), "%s", candidates[i]);
            return path;
        }
    }
    path[0] = '\0';
    return NULL;
}

static inline int tt_e2e_run(const char *cmd_args, cJSON **out_json)
{
    const char *bin = tt_e2e_binary();
    if (!bin) {
        if (out_json) *out_json = NULL;
        return 127;
    }

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s %s 2>/dev/null", bin, cmd_args);

    FILE *p = popen(cmd, "r");
    if (!p) {
        if (out_json) *out_json = NULL;
        return -1;
    }

    char buf[65536];
    size_t total = 0;
    size_t n;
    while ((n = fread(buf + total, 1, sizeof(buf) - total - 1, p)) > 0) {
        total += n;
    }
    buf[total] = '\0';

    int status = pclose(p);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    if (out_json) {
        *out_json = cJSON_Parse(buf);
    }

    return exit_code;
}

#endif /* TT_TEST_E2E_HELPERS_H */
