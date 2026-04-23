/*
 * test_e2e_helpers.h -- Helpers for E2E tests.
 *
 * Uses tt_proc_run() (platform.h) which invokes CreateProcessW on Windows and
 * fork/exec on POSIX, bypassing the shell entirely. This avoids cmd.exe's
 * quote-stripping rules that corrupt commands with multiple quote pairs.
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
#ifndef X_OK
#define X_OK 0 /* Windows: existence check only */
#endif
#else
#include <unistd.h>
#endif

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

/*
 * tt_e2e_tokenize -- Split cmd_args into argv tokens, respecting double quotes.
 *
 * argv[0] is populated with `bin`. Subsequent tokens come from cmd_args.
 * Double quotes delimit tokens containing spaces; the quote characters
 * themselves are stripped.
 *
 * Returns argc on success. argv[argc] is set to NULL.
 */
static inline int tt_e2e_tokenize(const char *bin, const char *cmd_args,
                                   const char **argv, int argv_max,
                                   char *store, size_t store_size)
{
    int argc = 0;
    if (argc < argv_max) argv[argc++] = bin;

    size_t sp = 0;
    const char *p = cmd_args;
    while (*p && argc < argv_max - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        argv[argc++] = &store[sp];
        char quote_ch = 0; /* 0 = not in quote, otherwise holds '"' or '\'' */
        while (*p && (quote_ch || (*p != ' ' && *p != '\t'))) {
            if (*p == '"' || *p == '\'') {
                if (!quote_ch) {
                    quote_ch = *p;
                    p++;
                    continue;
                }
                if (quote_ch == *p) {
                    quote_ch = 0;
                    p++;
                    continue;
                }
                /* Different quote char inside: treat as literal */
            }
            if (sp < store_size - 1) store[sp++] = *p;
            p++;
        }
        if (sp < store_size - 1) store[sp++] = '\0';
    }
    argv[argc] = NULL;
    return argc;
}

/*
 * tt_e2e_run -- Run toktoken binary and capture stdout.
 *
 * cmd_args: arguments after "toktoken" (e.g. "index:create --path /tmp/x").
 * out_json: receives parsed cJSON (caller must cJSON_Delete). NULL if parse fails.
 * Returns exit code. Returns 127 if binary not found, -1 on spawn failure.
 */
static inline int tt_e2e_run(const char *cmd_args, cJSON **out_json)
{
    const char *bin = tt_e2e_binary();
    if (!bin) {
        fprintf(stderr, "[E2E] ERROR: toktoken binary not found\n");
        if (out_json) *out_json = NULL;
        return 127;
    }

    const char *argv[64];
    char argstore[4096];
    int argc = tt_e2e_tokenize(bin, cmd_args, argv, 64, argstore, sizeof(argstore));
    (void)argc;

    tt_proc_result_t r = tt_proc_run(argv, NULL, 0);

    if (out_json) {
        *out_json = (r.stdout_buf && *r.stdout_buf) ? cJSON_Parse(r.stdout_buf) : NULL;
    }

    /* Surface diagnostic when the binary failed without producing JSON output. */
    int has_stdout = r.stdout_buf && *r.stdout_buf;
    if (r.exit_code != 0 && !has_stdout) {
        fprintf(stderr, "[E2E] bin: %s\n", bin);
        fprintf(stderr, "[E2E] args: %s\n", cmd_args);
        fprintf(stderr, "[E2E] exit_code: %d\n", r.exit_code);
        if (r.stderr_buf && *r.stderr_buf) {
            fprintf(stderr, "[E2E] stderr: %s\n", r.stderr_buf);
        } else {
            fprintf(stderr, "[E2E] stderr: (empty)\n");
        }
        fflush(stderr);
    }

    int exit_code = r.exit_code;
    tt_proc_result_free(&r);
    return exit_code;
}

#endif /* TT_TEST_E2E_HELPERS_H */
