/*
 * cmd_update.c -- Self-update command.
 *
 * Flow:
 *   1. Resolve path to the currently running binary
 *   2. Check writability
 *   3. Fetch fresh upstream version, compare with current
 *   4. Download correct platform binary to temp file
 *   5. Download SHA256SUMS and verify integrity
 *   6. Set executable, smoke test, atomic replace
 */

#include "cmd_update.h"
#include "update_check.h"
#include "version.h"
#include "json_output.h"
#include "platform.h"
#include "sha256_util.h"
#include "error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TT_PLATFORM_WINDOWS
#include <unistd.h>   /* access() */
#else
#include <io.h>        /* _access() */
#define access _access
#ifndef W_OK
#define W_OK 2
#endif
#endif

#define TT_RELEASE_BASE_URL "https://github.com/mauriziofonte/toktoken/releases/latest/download/"
#define TT_DOWNLOAD_TIMEOUT_MS 35000
#define TT_SHA256_TIMEOUT_MS 12000
#define TT_SMOKE_TIMEOUT_MS 5000

/* ===== Helpers ===== */

static cJSON *make_error(const char *code, const char *message, const char *hint)
{
    cJSON *json = cJSON_CreateObject();
    if (!json) return NULL;
    cJSON_AddStringToObject(json, "error", code);
    cJSON_AddStringToObject(json, "message", message);
    if (hint && hint[0])
        cJSON_AddStringToObject(json, "hint", hint);
    return json;
}

/*
 * resolve_self_path -- Find the path to the running binary.
 *
 * Tries tt_self_exe_path() first, then tt_which("toktoken") as fallback.
 * [caller-frees]
 */
static char *resolve_self_path(void)
{
    char *path = tt_self_exe_path();
    if (path) return path;

    path = tt_which("toktoken");
    return path;
}

/*
 * download_file -- Download a URL to a local file path via curl.
 *
 * Returns 0 on success, -1 on error.
 */
static int download_file(const char *url, const char *dest, int timeout_ms)
{
    char *curl = tt_which("curl");
    if (!curl) {
        tt_error_set("curl not found in PATH (required for self-update)");
        return -1;
    }

    const char *argv[] = {
        curl, "-fsSL", "--max-time", "30", "-o", dest, url, NULL
    };

    tt_proc_result_t res = tt_proc_run(argv, NULL, timeout_ms);
    int rc = res.exit_code;

    if (rc != 0 && res.stderr_buf && res.stderr_buf[0]) {
        tt_error_set("curl failed: %s", res.stderr_buf);
    } else if (rc != 0) {
        tt_error_set("curl failed with exit code %d", rc);
    }

    tt_proc_result_free(&res);
    free(curl);
    return (rc == 0) ? 0 : -1;
}

/*
 * download_stdout -- Download a URL and return its content as a string.
 *
 * [caller-frees] Returns NULL on error.
 */
static char *download_stdout(const char *url, int timeout_ms)
{
    char *curl = tt_which("curl");
    if (!curl) return NULL;

    const char *argv[] = {
        curl, "-fsSL", "--max-time", "10", url, NULL
    };

    tt_proc_result_t res = tt_proc_run(argv, NULL, timeout_ms);
    char *output = NULL;

    if (res.exit_code == 0 && res.stdout_buf && res.stdout_buf[0]) {
        output = strdup(res.stdout_buf);
    }

    tt_proc_result_free(&res);
    free(curl);
    return output;
}

/*
 * verify_sha256 -- Verify a file against SHA256SUMS content.
 *
 * sha256sums: the full text of SHA256SUMS file
 * binary_name: the expected filename to find in SHA256SUMS
 * file_path: local file to verify
 *
 * Returns 0 if match, -1 if mismatch or not found.
 */
static int verify_sha256(const char *sha256sums, const char *binary_name,
                          const char *file_path)
{
    /* Find the line containing binary_name */
    const char *line = sha256sums;
    const char *found_hash = NULL;

    while (line && *line) {
        /* Each line: "hexhash  filename\n" */
        const char *eol = strchr(line, '\n');
        size_t line_len = eol ? (size_t)(eol - line) : strlen(line);

        /* Check if this line contains our binary name */
        if (line_len > 66) {  /* 64 hex + 2 spaces + at least 1 char filename */
            const char *fname_start = line + 66;
            /* Also handle "hash  filename" with 2-space separator */
            if (strncmp(fname_start, binary_name, strlen(binary_name)) == 0) {
                found_hash = line;
                break;
            }
        }

        line = eol ? eol + 1 : NULL;
    }

    if (!found_hash) {
        tt_error_set("Binary %s not found in SHA256SUMS", binary_name);
        return -1;
    }

    /* Extract expected hash (first 64 chars of the line) */
    char expected[65];
    memcpy(expected, found_hash, 64);
    expected[64] = '\0';

    /* Compute actual hash */
    char *actual = tt_sha256_file(file_path);
    if (!actual) {
        tt_error_set("Failed to compute SHA256 of downloaded file");
        return -1;
    }

    int cmp = strcmp(expected, actual);
    if (cmp != 0) {
        tt_error_set("SHA256 mismatch: expected %s, got %s", expected, actual);
    }

    free(actual);
    return (cmp == 0) ? 0 : -1;
}

/*
 * smoke_test -- Run the downloaded binary with --version and verify output.
 *
 * Returns 0 if the binary appears to work, -1 otherwise.
 */
static int smoke_test(const char *binary_path)
{
    const char *argv[] = {binary_path, "--version", NULL};
    tt_proc_result_t res = tt_proc_run(argv, NULL, TT_SMOKE_TIMEOUT_MS);

    int ok = (res.exit_code == 0 &&
              res.stdout_buf &&
              tt_strcasestr(res.stdout_buf, "toktoken") != NULL);

    tt_proc_result_free(&res);

    if (!ok) {
        tt_error_set("Smoke test failed: downloaded binary did not produce expected output");
        return -1;
    }
    return 0;
}

/* ===== Public API ===== */

cJSON *tt_cmd_self_update_exec(tt_cli_opts_t *opts)
{
    (void)opts;
    tt_timer_start();

    /* 1. Resolve self path */
    char *self_path = resolve_self_path();
    if (!self_path) {
        return make_error("self_path_failed",
            "Could not determine binary location.",
            "Ensure toktoken is in your PATH.");
    }

    /* 2. Check writability */
    if (access(self_path, W_OK) != 0) {
        char hint[512];
        snprintf(hint, sizeof(hint),
                 "Binary at %s is not writable. Download manually from: "
                 TT_RELEASE_BASE_URL "%s",
                 self_path, tt_update_platform_binary_name());
        cJSON *err = make_error("not_writable",
            "Cannot update: binary is not writable.", hint);
        free(self_path);
        return err;
    }

    /* 3. Fetch fresh upstream version */
    tt_progress("Checking for updates...\n");
    tt_update_info_t info = tt_update_check_fresh();

    if (!info.upstream_version) {
        tt_update_info_free(&info);
        free(self_path);
        return make_error("check_failed",
            "Could not determine upstream version.",
            "Check your network connection and try again.");
    }

    /* 4. Compare versions */
    if (!info.update_available) {
        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "message", "Already up to date.");
        cJSON_AddStringToObject(result, "version", TT_VERSION);
        tt_update_info_free(&info);
        free(self_path);
        return result;
    }

    tt_progress("Update available: %s -> %s\n", TT_VERSION, info.upstream_version);

    /* 5. Download binary */
    const char *binary_name = tt_update_platform_binary_name();
    char url[512];
    snprintf(url, sizeof(url), TT_RELEASE_BASE_URL "%s", binary_name);

    size_t plen = strlen(self_path);
    char *tmp_path = malloc(plen + 16);
    if (!tmp_path) {
        tt_update_info_free(&info);
        free(self_path);
        return make_error("alloc_failed", "Memory allocation failed.", NULL);
    }
    snprintf(tmp_path, plen + 16, "%s.update-tmp", self_path);

    tt_progress("Downloading %s...\n", binary_name);
    if (download_file(url, tmp_path, TT_DOWNLOAD_TIMEOUT_MS) < 0) {
        cJSON *err = make_error("download_failed",
            "Failed to download binary.", tt_error_get());
        tt_remove_file(tmp_path);
        free(tmp_path);
        tt_update_info_free(&info);
        free(self_path);
        return err;
    }

    /* 6. Download SHA256SUMS and verify */
    char sha_url[512];
    snprintf(sha_url, sizeof(sha_url), TT_RELEASE_BASE_URL "SHA256SUMS");

    tt_progress("Verifying SHA256...\n");
    char *sha256sums = download_stdout(sha_url, TT_SHA256_TIMEOUT_MS);
    if (!sha256sums) {
        cJSON *err = make_error("sha256_download_failed",
            "Failed to download SHA256SUMS.", "Cannot verify binary integrity.");
        tt_remove_file(tmp_path);
        free(tmp_path);
        tt_update_info_free(&info);
        free(self_path);
        return err;
    }

    if (verify_sha256(sha256sums, binary_name, tmp_path) < 0) {
        cJSON *err = make_error("sha256_mismatch",
            "SHA256 verification failed.", tt_error_get());
        free(sha256sums);
        tt_remove_file(tmp_path);
        free(tmp_path);
        tt_update_info_free(&info);
        free(self_path);
        return err;
    }
    free(sha256sums);

    /* 7. Set executable */
    tt_file_set_executable(tmp_path);

    /* 8. Smoke test */
    tt_progress("Running smoke test...\n");
    if (smoke_test(tmp_path) < 0) {
        cJSON *err = make_error("smoke_test_failed",
            "Downloaded binary failed smoke test.", tt_error_get());
        tt_remove_file(tmp_path);
        free(tmp_path);
        tt_update_info_free(&info);
        free(self_path);
        return err;
    }

    /* 9. Atomic replace */
    tt_progress("Replacing binary...\n");
    if (tt_rename_file(tmp_path, self_path) < 0) {
        cJSON *err = make_error("replace_failed",
            "Failed to replace binary.", tt_error_get());
        tt_remove_file(tmp_path);
        free(tmp_path);
        tt_update_info_free(&info);
        free(self_path);
        return err;
    }

    /* 10. Success */
    cJSON *result = cJSON_CreateObject();
    cJSON_AddTrueToObject(result, "success");
    cJSON_AddStringToObject(result, "previous_version", TT_VERSION);
    cJSON_AddStringToObject(result, "new_version", info.upstream_version);
    cJSON_AddStringToObject(result, "path", self_path);

    tt_progress("Updated: %s -> %s\n", TT_VERSION, info.upstream_version);

    free(tmp_path);
    tt_update_info_free(&info);
    free(self_path);
    return result;
}

int tt_cmd_self_update(tt_cli_opts_t *opts)
{
    cJSON *result = tt_cmd_self_update_exec(opts);
    if (!result) {
        return tt_output_error("internal_error", tt_error_get(), NULL);
    }

    /* Check if this is an error response */
    cJSON *err = cJSON_GetObjectItemCaseSensitive(result, "error");
    int exit_code = err ? 1 : 0;

    tt_json_print(result);
    cJSON_Delete(result);
    return exit_code;
}
