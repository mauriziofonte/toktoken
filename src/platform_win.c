/*
 * platform_win.c -- Windows implementation of the platform layer.
 *
 * Uses Win32 APIs: GetFullPathNameW, FindFirstFileW, CreateProcessW, etc.
 * All Windows APIs use wchar_t; this module converts UTF-8 <-> UTF-16 internally.
 *
 * Compiled only on WIN32 targets.
 */

#ifdef TT_PLATFORM_WINDOWS

#include "platform.h"
#include "error.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

/* ===== UTF-8 <-> UTF-16 conversion helpers ===== */

static wchar_t *utf8_to_utf16(const char *s)
{
    if (!s) return NULL;
    int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (len <= 0) return NULL;
    wchar_t *w = malloc((size_t)len * sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, len);
    return w;
}

static char *utf16_to_utf8(const wchar_t *w)
{
    if (!w) return NULL;
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (len <= 0) return NULL;
    char *s = malloc((size_t)len);
    if (!s) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, len, NULL, NULL);
    return s;
}

/* ===== Path operations ===== */

char *tt_realpath(const char *path)
{
    if (!path) {
        tt_error_set("tt_realpath: NULL path");
        return NULL;
    }
    wchar_t *wpath = utf8_to_utf16(path);
    if (!wpath) {
        tt_error_set("tt_realpath: UTF-8 conversion failed");
        return NULL;
    }

    DWORD len = GetFullPathNameW(wpath, 0, NULL, NULL);
    if (len == 0) {
        tt_error_set("tt_realpath: GetFullPathNameW failed for %s", path);
        free(wpath);
        return NULL;
    }

    wchar_t *wresolved = malloc(len * sizeof(wchar_t));
    if (!wresolved) { free(wpath); return NULL; }

    GetFullPathNameW(wpath, len, wresolved, NULL);
    free(wpath);

    char *result = utf16_to_utf8(wresolved);
    free(wresolved);

    if (result) tt_path_normalize_sep(result);
    return result;
}

char *tt_path_join(const char *base, const char *component)
{
    if (!base || !component) return NULL;

    size_t blen = strlen(base);
    size_t clen = strlen(component);

    while (blen > 1 && (base[blen - 1] == '/' || base[blen - 1] == '\\')) blen--;
    while (clen > 0 && (component[0] == '/' || component[0] == '\\')) { component++; clen--; }

    size_t total = blen + 1 + clen + 1;
    char *result = malloc(total);
    if (!result) return NULL;

    memcpy(result, base, blen);
    result[blen] = '/';
    memcpy(result + blen + 1, component, clen);
    result[blen + 1 + clen] = '\0';
    return result;
}

const char *tt_path_basename(const char *path)
{
    if (!path) return "";
    const char *last = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') last = p + 1;
    }
    return last;
}

const char *tt_path_extension(const char *path)
{
    const char *base = tt_path_basename(path);
    if (!base || !*base) return "";

    size_t len = strlen(base);
    if (len >= 10) {
        const char *suffix = base + len - 10;
        if (_stricmp(suffix, ".blade.php") == 0) return suffix;
    }

    const char *dot = strrchr(base, '.');
    if (!dot || dot == base) return "";
    return dot;
}

const char *tt_home_dir(void)
{
    static char home[MAX_PATH * 3] = "";
    if (home[0]) return home;

    const char *env = getenv("USERPROFILE");
    if (env) {
        strncpy(home, env, sizeof(home) - 1);
        tt_path_normalize_sep(home);
        return home;
    }

    wchar_t wpath[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, 0, wpath))) {
        char *utf8 = utf16_to_utf8(wpath);
        if (utf8) {
            strncpy(home, utf8, sizeof(home) - 1);
            tt_path_normalize_sep(home);
            free(utf8);
            return home;
        }
    }

    strncpy(home, "C:/", sizeof(home) - 1);
    return home;
}

bool tt_path_is_absolute(const char *path)
{
    if (!path) return false;
    /* Unix-style /path or Windows-style C:\path or C:/path */
    if (path[0] == '/' || path[0] == '\\') return true;
    if (isalpha((unsigned char)path[0]) && path[1] == ':') return true;
    return false;
}

char *tt_getcwd(void)
{
    wchar_t wbuf[MAX_PATH];
    DWORD len = GetCurrentDirectoryW(MAX_PATH, wbuf);
    if (len == 0 || len >= MAX_PATH) {
        tt_error_set("tt_getcwd: GetCurrentDirectoryW failed");
        return NULL;
    }
    char *result = utf16_to_utf8(wbuf);
    if (result) tt_path_normalize_sep(result);
    return result;
}

char *tt_path_relative(const char *root, const char *full_path)
{
    if (!root || !full_path) return NULL;

    size_t rlen = strlen(root);
    while (rlen > 1 && (root[rlen - 1] == '/' || root[rlen - 1] == '\\')) rlen--;

    if (_strnicmp(full_path, root, rlen) != 0) return NULL;

    char c = full_path[rlen];
    if (c == '/' || c == '\\') {
        char *result = _strdup(full_path + rlen + 1);
        if (result) tt_path_normalize_sep(result);
        return result;
    } else if (c == '\0') {
        return _strdup("");
    }
    return NULL;
}

void tt_path_normalize_sep(char *path)
{
    if (!path) return;
    for (char *p = path; *p; p++) {
        if (*p == '\\') *p = '/';
    }
}

/* ===== Filesystem operations ===== */

static DWORD get_attrs(const char *path)
{
    wchar_t *wpath = utf8_to_utf16(path);
    if (!wpath) return INVALID_FILE_ATTRIBUTES;
    DWORD attrs = GetFileAttributesW(wpath);
    free(wpath);
    return attrs;
}

bool tt_file_exists(const char *path)
{
    return path && get_attrs(path) != INVALID_FILE_ATTRIBUTES;
}

time_t tt_file_mtime(const char *path)
{
    if (!path) return 0;
    wchar_t *wpath = utf8_to_utf16(path);
    if (!wpath) return 0;

    WIN32_FILE_ATTRIBUTE_DATA fad;
    BOOL ok = GetFileAttributesExW(wpath, GetFileExInfoStandard, &fad);
    free(wpath);
    if (!ok) return 0;

    /* Convert FILETIME to time_t (seconds since 1970-01-01) */
    ULARGE_INTEGER ull;
    ull.HighPart = fad.ftLastWriteTime.dwHighDateTime;
    ull.LowPart = fad.ftLastWriteTime.dwLowDateTime;
    return (time_t)((ull.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

bool tt_is_dir(const char *path)
{
    DWORD a = get_attrs(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool tt_is_symlink(const char *path)
{
    DWORD a = get_attrs(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}

bool tt_is_file(const char *path)
{
    DWORD a = get_attrs(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

int64_t tt_file_size(const char *path)
{
    if (!path) return -1;
    wchar_t *wpath = utf8_to_utf16(path);
    if (!wpath) return -1;

    WIN32_FILE_ATTRIBUTE_DATA fad;
    BOOL ok = GetFileAttributesExW(wpath, GetFileExInfoStandard, &fad);
    free(wpath);
    if (!ok) return -1;

    LARGE_INTEGER sz;
    sz.HighPart = (LONG)fad.nFileSizeHigh;
    sz.LowPart = fad.nFileSizeLow;
    return (int64_t)sz.QuadPart;
}

char *tt_read_file(const char *path, size_t *out_len)
{
    if (!path) {
        tt_error_set("tt_read_file: NULL path");
        return NULL;
    }

    wchar_t *wpath = utf8_to_utf16(path);
    if (!wpath) {
        tt_error_set("tt_read_file: UTF-8 conversion failed");
        return NULL;
    }

    HANDLE h = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(wpath);
    if (h == INVALID_HANDLE_VALUE) {
        tt_error_set("tt_read_file: %s: cannot open", path);
        return NULL;
    }

    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) {
        tt_error_set("tt_read_file: %s: GetFileSizeEx failed", path);
        CloseHandle(h);
        return NULL;
    }

    size_t size = (size_t)sz.QuadPart;
    char *buf = malloc(size + 1);
    if (!buf) {
        CloseHandle(h);
        return NULL;
    }

    DWORD nread = 0;
    ReadFile(h, buf, (DWORD)size, &nread, NULL);
    CloseHandle(h);
    buf[nread] = '\0';

    if (out_len) *out_len = (size_t)nread;
    return buf;
}

int tt_write_file(const char *path, const char *data, size_t len)
{
    if (!path || !data) {
        tt_error_set("tt_write_file: NULL argument");
        return -1;
    }

    wchar_t *wpath = utf8_to_utf16(path);
    if (!wpath) return -1;

    HANDLE h = CreateFileW(wpath, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(wpath);
    if (h == INVALID_HANDLE_VALUE) {
        tt_error_set("tt_write_file: %s: cannot create", path);
        return -1;
    }

    DWORD written = 0;
    if (len > 0) WriteFile(h, data, (DWORD)len, &written, NULL);
    CloseHandle(h);
    return (written == (DWORD)len) ? 0 : -1;
}

int tt_mkdir_p(const char *path)
{
    if (!path) return -1;

    char *tmp = _strdup(path);
    if (!tmp) return -1;
    tt_path_normalize_sep(tmp);

    size_t len = strlen(tmp);
    if (len > 1 && tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            wchar_t *w = utf8_to_utf16(tmp);
            if (w) { CreateDirectoryW(w, NULL); free(w); }
            *p = '/';
        }
    }

    wchar_t *w = utf8_to_utf16(tmp);
    free(tmp);
    if (!w) return -1;

    BOOL ok = CreateDirectoryW(w, NULL);
    free(w);
    if (!ok && GetLastError() != ERROR_ALREADY_EXISTS) {
        tt_error_set("tt_mkdir_p: %s: failed", path);
        return -1;
    }
    return 0;
}

int tt_remove_file(const char *path)
{
    if (!path) return -1;
    wchar_t *wpath = utf8_to_utf16(path);
    if (!wpath) return -1;
    BOOL ok = DeleteFileW(wpath);
    free(wpath);
    if (!ok) {
        tt_error_set("tt_remove_file: %s: failed", path);
        return -1;
    }
    return 0;
}

int tt_remove_dir(const char *path)
{
    if (!path) return -1;
    wchar_t *wpath = utf8_to_utf16(path);
    if (!wpath) return -1;
    BOOL ok = RemoveDirectoryW(wpath);
    free(wpath);
    if (!ok) {
        tt_error_set("tt_remove_dir: %s: failed", path);
        return -1;
    }
    return 0;
}

int tt_remove_dir_recursive(const char *path)
{
    if (!path) return -1;

    wchar_t *wpath = utf8_to_utf16(path);
    if (!wpath) return -1;

    /* Append wildcard for FindFirstFile */
    size_t len = wcslen(wpath);
    wchar_t *pattern = malloc((len + 3) * sizeof(wchar_t));
    if (!pattern) { free(wpath); return -1; }
    wcscpy(pattern, wpath);
    wcscat(pattern, L"\\*");

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    free(pattern);

    int rc = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                continue;

            /* Build child path */
            char *child_name = utf16_to_utf8(fd.cFileName);
            if (!child_name) { rc = -1; break; }
            char *child = tt_path_join(path, child_name);
            free(child_name);
            if (!child) { rc = -1; break; }

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (tt_remove_dir_recursive(child) < 0) rc = -1;
            } else {
                if (tt_remove_file(child) < 0) rc = -1;
            }
            free(child);
            if (rc < 0) break;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    if (rc == 0) {
        if (!RemoveDirectoryW(wpath)) {
            tt_error_set("tt_remove_dir_recursive: %s: failed", path);
            rc = -1;
        }
    }
    free(wpath);
    return rc;
}

/* ===== Directory walking ===== */

int tt_walk_dir(const char *root, tt_walk_cb cb, void *userdata)
{
    if (!root || !cb) {
        tt_error_set("tt_walk_dir: NULL argument");
        return -1;
    }

    char *pattern = tt_path_join(root, "*");
    if (!pattern) return -1;

    wchar_t *wpattern = utf8_to_utf16(pattern);
    free(pattern);
    if (!wpattern) return -1;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpattern, &fd);
    free(wpattern);
    if (h == INVALID_HANDLE_VALUE) return 0;

    do {
        char *name = utf16_to_utf8(fd.cFileName);
        if (!name) continue;

        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            free(name);
            continue;
        }

        bool is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        bool is_link = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

        int rc = cb(root, name, is_dir, is_link, userdata);
        if (rc < 0) {
            free(name);
            FindClose(h);
            return 0;
        }

        if (is_dir && rc == 0) {
            char *subdir = tt_path_join(root, name);
            if (subdir) {
                tt_walk_dir(subdir, cb, userdata);
                free(subdir);
            }
        }

        free(name);
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    return 0;
}

/* ===== Process execution ===== */

/*
 * win_argv_escape -- Escape a single argument for Windows command line.
 *
 * Implements the escaping rules expected by CommandLineToArgvW():
 *   - The argument is wrapped in double quotes.
 *   - Backslashes are literal UNLESS immediately preceding a double quote,
 *     in which case each backslash must be doubled.
 *   - A trailing run of backslashes (before the closing quote) must also
 *     be doubled.
 *   - Each embedded double quote is escaped as \".
 *
 * Reference: https://learn.microsoft.com/en-us/cpp/c-language/parsing-c-command-line-arguments
 *
 * Appends the escaped argument (including surrounding quotes) to dst.
 * dst must point into a buffer with sufficient space.
 * Returns pointer past the last written byte.
 */
static char *win_argv_escape(char *dst, const char *arg)
{
    *dst++ = '"';

    const char *p = arg;
    while (*p)
    {
        /* Count consecutive backslashes */
        size_t num_backslashes = 0;
        while (*p == '\\')
        {
            p++;
            num_backslashes++;
        }

        if (*p == '\0')
        {
            /* Argument ends with backslashes: double them (they precede
             * the closing quote) */
            for (size_t i = 0; i < num_backslashes * 2; i++)
                *dst++ = '\\';
            break;
        }
        else if (*p == '"')
        {
            /* Backslashes followed by a double quote: double the
             * backslashes, then escape the quote */
            for (size_t i = 0; i < num_backslashes * 2; i++)
                *dst++ = '\\';
            *dst++ = '\\';
            *dst++ = '"';
            p++;
        }
        else
        {
            /* Backslashes not followed by a quote: emit them literally */
            for (size_t i = 0; i < num_backslashes; i++)
                *dst++ = '\\';
            *dst++ = *p++;
        }
    }

    *dst++ = '"';
    return dst;
}

/*
 * win_cmdline_size -- Compute worst-case buffer size for escaped command line.
 *
 * Worst case per argument: every character could need doubling (all backslashes
 * before a quote), plus 2 quotes + 1 space separator.
 */
static size_t win_cmdline_size(const char *const argv[])
{
    size_t total = 0;
    for (int i = 0; argv[i]; i++)
    {
        total += strlen(argv[i]) * 2 + 3; /* worst-case escaping + quotes + space */
    }
    return total + 1; /* NUL terminator */
}

tt_proc_result_t tt_proc_run(const char *const argv[], const char *stdin_data,
                              int timeout_ms)
{
    tt_proc_result_t result = { NULL, NULL, -1 };

    if (!argv || !argv[0]) {
        tt_error_set("tt_proc_run: NULL argv");
        return result;
    }

    /* Build properly escaped command line from argv */
    size_t cmdlen = win_cmdline_size(argv);
    char *cmdline = malloc(cmdlen);
    if (!cmdline) return result;

    char *dst = cmdline;
    for (int i = 0; argv[i]; i++) {
        if (i > 0) *dst++ = ' ';
        dst = win_argv_escape(dst, argv[i]);
    }
    *dst = '\0';

    wchar_t *wcmd = utf8_to_utf16(cmdline);
    free(cmdline);
    if (!wcmd) return result;

    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE hStdoutR, hStdoutW, hStderrR, hStderrW;

    if (!CreatePipe(&hStdoutR, &hStdoutW, &sa, 0) ||
        !CreatePipe(&hStderrR, &hStderrW, &sa, 0)) {
        free(wcmd);
        return result;
    }

    SetHandleInformation(hStdoutR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hStderrR, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hStdoutW;
    si.hStdError = hStderrW;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi = {0};
    BOOL ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    free(wcmd);

    CloseHandle(hStdoutW);
    CloseHandle(hStderrW);

    if (!ok) {
        tt_error_set("tt_proc_run: CreateProcessW failed");
        CloseHandle(hStdoutR);
        CloseHandle(hStderrR);
        return result;
    }

    /* Read stdout and stderr */
    /* Simplified: read all from stdout, then stderr */
    {
        size_t cap = 4096, len = 0;
        char *buf = malloc(cap);
        DWORD nread;
        while (ReadFile(hStdoutR, buf + len, (DWORD)(cap - len), &nread, NULL) && nread > 0) {
            len += nread;
            if (len >= cap) { cap *= 2; buf = realloc(buf, cap); }
        }
        buf[len] = '\0';
        result.stdout_buf = buf;
    }
    {
        size_t cap = 4096, len = 0;
        char *buf = malloc(cap);
        DWORD nread;
        while (ReadFile(hStderrR, buf + len, (DWORD)(cap - len), &nread, NULL) && nread > 0) {
            len += nread;
            if (len >= cap) { cap *= 2; buf = realloc(buf, cap); }
        }
        buf[len] = '\0';
        result.stderr_buf = buf;
    }
    CloseHandle(hStdoutR);
    CloseHandle(hStderrR);

    DWORD wait_ms = (timeout_ms > 0) ? (DWORD)timeout_ms : INFINITE;
    DWORD wait_result = WaitForSingleObject(pi.hProcess, wait_ms);

    if (wait_result == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);
        result.exit_code = -1;
        tt_error_set("tt_proc_run: timeout after %d ms", timeout_ms);
    } else {
        DWORD exit_code = 0;
        GetExitCodeProcess(pi.hProcess, &exit_code);
        result.exit_code = (int)exit_code;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return result;
}

void tt_proc_result_free(tt_proc_result_t *r)
{
    if (!r) return;
    free(r->stdout_buf);
    free(r->stderr_buf);
    r->stdout_buf = NULL;
    r->stderr_buf = NULL;
}

void tt_sleep_ms(int ms)
{
    if (ms <= 0) return;
    Sleep((DWORD)ms);
}

/* ===== Thread abstraction (Windows) ===== */

typedef struct
{
    tt_thread_fn fn;
    void *arg;
} tt_thread_trampoline_t;

static DWORD WINAPI thread_trampoline(LPVOID raw)
{
    tt_thread_trampoline_t *t = (tt_thread_trampoline_t *)raw;
    t->fn(t->arg);
    free(t);
    return 0;
}

int tt_thread_create(tt_thread_t *thread, tt_thread_fn fn, void *arg)
{
    tt_thread_trampoline_t *t = malloc(sizeof(tt_thread_trampoline_t));
    if (!t)
        return -1;
    t->fn = fn;
    t->arg = arg;
    HANDLE h = CreateThread(NULL, 0, thread_trampoline, t, 0, NULL);
    if (!h)
    {
        free(t);
        return -1;
    }
    *thread = h;
    return 0;
}

int tt_thread_join(tt_thread_t thread)
{
    HANDLE h = (HANDLE)thread;
    if (!h)
        return -1;
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    return 0;
}

char *tt_tmpfile_write(const char *prefix, const char *data, size_t len)
{
    char tmp_dir[MAX_PATH];
    DWORD dlen = GetTempPathA(sizeof(tmp_dir), tmp_dir);
    if (dlen == 0 || dlen >= sizeof(tmp_dir))
        return NULL;

    char tmp_path[MAX_PATH];
    char pfx[4] = "tt_";
    if (prefix && prefix[0])
    {
        pfx[0] = prefix[0];
        pfx[1] = prefix[1] ? prefix[1] : '_';
        pfx[2] = prefix[2] ? prefix[2] : '_';
        pfx[3] = '\0';
    }
    if (GetTempFileNameA(tmp_dir, pfx, 0, tmp_path) == 0)
        return NULL;

    if (tt_write_file(tmp_path, data, len) < 0)
    {
        DeleteFileA(tmp_path);
        return NULL;
    }
    return _strdup(tmp_path);
}

int tt_getpid(void)
{
    return (int)GetCurrentProcessId();
}

/* ===== Which ===== */

char *tt_which(const char *name)
{
    if (!name) return NULL;

    /* Try common extensions */
    static const char *exts[] = { "", ".exe", ".cmd", ".bat", NULL };
    const char *path_env = getenv("PATH");
    if (!path_env) return NULL;

    char *path_copy = _strdup(path_env);
    if (!path_copy) return NULL;

    char *ctx = NULL;
    char *dir = strtok_s(path_copy, ";", &ctx);
    while (dir) {
        for (int i = 0; exts[i]; i++) {
            size_t len = strlen(dir) + 1 + strlen(name) + strlen(exts[i]) + 1;
            char *full = malloc(len);
            if (!full) continue;
            snprintf(full, len, "%s/%s%s", dir, name, exts[i]);
            tt_path_normalize_sep(full);

            wchar_t *wfull = utf8_to_utf16(full);
            if (wfull && GetFileAttributesW(wfull) != INVALID_FILE_ATTRIBUTES) {
                free(wfull);
                free(path_copy);
                return full;
            }
            free(wfull);
            free(full);
        }
        dir = strtok_s(NULL, ";", &ctx);
    }

    free(path_copy);
    return NULL;
}

/* ===== String utilities ===== */

int tt_strcasecmp(const char *a, const char *b)
{
    return _stricmp(a, b);
}

int tt_strncasecmp(const char *a, const char *b, size_t n)
{
    return _strnicmp(a, b, n);
}

const char *tt_strcasestr(const char *haystack, const char *needle)
{
    if (!haystack || !needle) return NULL;
    if (!*needle) return haystack;

    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        if (_strnicmp(p, needle, nlen) == 0) return p;
    }
    return NULL;
}

/*
 * Custom fnmatch for Windows (POSIX fnmatch not available).
 * Supports: *, ?, [abc], [!abc]
 */
bool tt_fnmatch(const char *pattern, const char *string, bool case_insensitive)
{
    if (!pattern || !string) return false;

    while (*pattern) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern) return true;
            for (const char *s = string; *s; s++) {
                if (tt_fnmatch(pattern, s, case_insensitive)) return true;
            }
            return false;
        } else if (*pattern == '?') {
            if (!*string) return false;
            pattern++;
            string++;
        } else if (*pattern == '[') {
            pattern++;
            bool negate = false;
            if (*pattern == '!' || *pattern == '^') { negate = true; pattern++; }
            bool match = false;
            while (*pattern && *pattern != ']') {
                char c = case_insensitive ? (char)tolower((unsigned char)*pattern) : *pattern;
                char s = case_insensitive ? (char)tolower((unsigned char)*string) : *string;
                if (c == s) match = true;
                pattern++;
            }
            if (*pattern == ']') pattern++;
            if (negate ? match : !match) return false;
            string++;
        } else {
            char pc = case_insensitive ? (char)tolower((unsigned char)*pattern) : *pattern;
            char sc = case_insensitive ? (char)tolower((unsigned char)*string) : *string;
            if (pc != sc) return false;
            pattern++;
            string++;
        }
    }
    return *string == '\0';
}

/*
 * Extended fnmatch with ** globstar support.
 * Mirrors fnmatch_impl from platform.c for Windows builds.
 */
static bool fnmatch_impl(const char *p, const char *s, int flags)
{
    bool pathname = (flags & TT_FNM_PATHNAME) != 0;
    bool casefold = (flags & TT_FNM_CASEFOLD) != 0;

    while (*p) {
        if (*p == '*') {
            /* Check for ** (globstar) */
            if (p[1] == '*') {
                /* Consume all adjacent '*' */
                const char *after = p + 2;
                while (*after == '*') after++;

                /* '**' at end of pattern matches everything */
                if (*after == '\0') return true;

                /* double-star-slash: zero or more directory segments */
                if (*after == '/') {
                    after++; /* skip the '/' */
                    /* Try matching remainder at every '/' boundary */
                    const char *sp = s;
                    /* First: try with zero segments (match here) */
                    if (fnmatch_impl(after, sp, flags)) return true;
                    while (*sp) {
                        if (*sp == '/') {
                            if (fnmatch_impl(after, sp + 1, flags)) return true;
                        }
                        sp++;
                    }
                    return false;
                }

                /* '**' not followed by '/' -- treat like '*' matching across '/' */
                const char *sp = s;
                while (1) {
                    if (fnmatch_impl(after, sp, flags)) return true;
                    if (*sp == '\0') break;
                    sp++;
                }
                return false;
            }

            /* Single '*' */
            p++;
            /* Try matching remainder at every position */
            const char *sp = s;
            while (1) {
                if (fnmatch_impl(p, sp, flags)) return true;
                if (*sp == '\0') break;
                if (pathname && *sp == '/') break;
                sp++;
            }
            return false;
        }

        if (*s == '\0') return false;

        if (*p == '?') {
            if (pathname && *s == '/') return false;
            p++;
            s++;
            continue;
        }

        if (*p == '[') {
            /* Character class */
            p++;
            bool negate = false;
            if (*p == '!' || *p == '^') { negate = true; p++; }

            bool matched = false;
            char sc = casefold ? (char)tolower((unsigned char)*s) : *s;

            while (*p && *p != ']') {
                char lo = casefold ? (char)tolower((unsigned char)*p) : *p;
                p++;
                if (*p == '-' && p[1] && p[1] != ']') {
                    p++;
                    char hi = casefold ? (char)tolower((unsigned char)*p) : *p;
                    p++;
                    if (sc >= lo && sc <= hi) matched = true;
                } else {
                    if (sc == lo) matched = true;
                }
            }
            if (*p == ']') p++;
            if (matched == negate) return false;
            s++;
            continue;
        }

        /* Literal character */
        char pc = casefold ? (char)tolower((unsigned char)*p) : *p;
        char sc = casefold ? (char)tolower((unsigned char)*s) : *s;
        if (pc != sc) return false;
        p++;
        s++;
    }

    return *s == '\0';
}

bool tt_fnmatch_ex(const char *pattern, const char *string, int flags)
{
    if (!pattern || !string) return false;
    return fnmatch_impl(pattern, string, flags);
}

/* ===== Time ===== */

const char *tt_now_rfc3339(void)
{
    static _Thread_local char buf[64];

    time_t t = time(NULL);
    struct tm tm;
    localtime_s(&tm, &t);

    char tmp[64];
    strftime(tmp, sizeof(tmp), "%Y-%m-%dT%H:%M:%S%z", &tm);

    /* Insert colon in timezone */
    size_t len = strlen(tmp);
    if (len >= 5) {
        memcpy(buf, tmp, len - 2);
        buf[len - 2] = ':';
        memcpy(buf + len - 1, tmp + len - 2, 2);
        buf[len + 1] = '\0';
    } else {
        memcpy(buf, tmp, len + 1);
    }

    return buf;
}

uint64_t tt_monotonic_ms(void)
{
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (uint64_t)(count.QuadPart * 1000 / freq.QuadPart);
}

/* ===== Self-update support ===== */

char *tt_self_exe_path(void)
{
    wchar_t buf[MAX_PATH + 1];
    DWORD len = GetModuleFileNameW(NULL, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return NULL;
    char *path = utf16_to_utf8(buf);
    if (path) tt_path_normalize_sep(path);
    return path;
}

int tt_rename_file(const char *src, const char *dst)
{
    if (!src || !dst) return -1;

    wchar_t *wsrc = utf8_to_utf16(src);
    wchar_t *wdst = utf8_to_utf16(dst);
    if (!wsrc || !wdst) {
        free(wsrc);
        free(wdst);
        tt_error_set("tt_rename_file: UTF-16 conversion failed");
        return -1;
    }

    /* Try atomic replace first */
    BOOL ok = MoveFileExW(wsrc, wdst, MOVEFILE_REPLACE_EXISTING);
    if (!ok) {
        /* Fallback: rename existing to .old, then rename new to target */
        wchar_t wold[MAX_PATH + 8];
        _snwprintf(wold, MAX_PATH + 8, L"%s.old", wdst);
        MoveFileExW(wdst, wold, MOVEFILE_REPLACE_EXISTING);
        ok = MoveFileExW(wsrc, wdst, 0);
        if (ok) {
            DeleteFileW(wold);
        }
    }

    free(wsrc);
    free(wdst);

    if (!ok) {
        tt_error_set("tt_rename_file: %s -> %s: failed (error %lu)",
                      src, dst, (unsigned long)GetLastError());
        return -1;
    }
    return 0;
}

int tt_file_set_executable(const char *path)
{
    (void)path;
    return 0; /* no-op on Windows */
}

#else
/* Avoid empty translation unit warning on non-Windows builds */
typedef int tt_platform_win_unused;
#endif /* TT_PLATFORM_WINDOWS */
