/*
 * test_e2e_helpers.h -- Helpers for E2E tests using popen.
 */

#ifndef TT_TEST_E2E_HELPERS_H
#define TT_TEST_E2E_HELPERS_H

#include "platform.h"
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef TT_PLATFORM_WINDOWS
#include <io.h>
#define access _access
#define popen  _popen
#define pclose _pclose
#ifndef X_OK
#define X_OK 0 /* Windows: existence check only */
#endif
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

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
#ifdef TT_PLATFORM_WINDOWS
    snprintf(cmd, sizeof(cmd), "%s %s 2>NUL", bin, cmd_args);
#else
    snprintf(cmd, sizeof(cmd), "%s %s 2>/dev/null", bin, cmd_args);
#endif

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
#ifdef TT_PLATFORM_WINDOWS
    int exit_code = status; /* pclose returns exit code directly on Windows */
#else
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif

    if (out_json) {
        *out_json = cJSON_Parse(buf);
    }

    return exit_code;
}

#endif /* TT_TEST_E2E_HELPERS_H */
