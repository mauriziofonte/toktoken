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
#include <windows.h> /* GetCurrentProcessId */
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
#ifdef TT_PLATFORM_WINDOWS
        "./toktoken.exe",
        "./build/toktoken.exe",
        "./build/debug/toktoken.exe",
        "../build/toktoken.exe",
        "../build/debug/toktoken.exe",
#endif
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
        fprintf(stderr, "[E2E] ERROR: toktoken binary not found\n");
        if (out_json) *out_json = NULL;
        return 127;
    }

    /* Capture stderr to a tempfile so we can surface it on failure. */
    char stderr_capture[512];
#ifdef TT_PLATFORM_WINDOWS
    const char *tmp = getenv("TEMP");
    if (!tmp) tmp = getenv("TMP");
    if (!tmp) tmp = ".";
    snprintf(stderr_capture, sizeof(stderr_capture),
             "%s\\tt_e2e_err_%lu.txt", tmp, (unsigned long)GetCurrentProcessId());
#else
    snprintf(stderr_capture, sizeof(stderr_capture),
             "/tmp/tt_e2e_err_%d.txt", (int)getpid());
#endif

    char cmd[2048];
#ifdef TT_PLATFORM_WINDOWS
    /* Quote the binary path to survive spaces (e.g. runner workspaces). */
    snprintf(cmd, sizeof(cmd), "\"%s\" %s 2>\"%s\"", bin, cmd_args, stderr_capture);
#else
    snprintf(cmd, sizeof(cmd), "%s %s 2>%s", bin, cmd_args, stderr_capture);
#endif

    FILE *p = popen(cmd, "r");
    if (!p) {
        fprintf(stderr, "[E2E] ERROR: popen failed for: %s\n", cmd);
        remove(stderr_capture);
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

    /* Surface stderr when the binary crashed or produced no stdout — this is
     * the only signal we have in CI when the real cause isn't in the assertion. */
    if (exit_code != 0 && total == 0) {
        fprintf(stderr, "[E2E] cmd: %s\n", cmd);
        fprintf(stderr, "[E2E] exit_code: %d\n", exit_code);
        FILE *sf = fopen(stderr_capture, "r");
        if (sf) {
            char serr[4096];
            size_t sn = fread(serr, 1, sizeof(serr) - 1, sf);
            serr[sn] = '\0';
            fclose(sf);
            if (sn > 0) {
                fprintf(stderr, "[E2E] stderr: %s\n", serr);
            } else {
                fprintf(stderr, "[E2E] stderr: (empty)\n");
            }
        }
        fflush(stderr);
    }
    remove(stderr_capture);

    return exit_code;
}

#endif /* TT_TEST_E2E_HELPERS_H */
