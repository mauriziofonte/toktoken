/*
 * ctags_stream_win.c -- Streaming pipe reader for ctags subprocess (Windows).
 *
 * Architecture:
 *   - CreateProcessW spawns ctags with stdout/stderr redirected to anonymous pipes.
 *   - A dedicated thread drains stderr to prevent pipe deadlock (Windows anonymous
 *     pipes do not support overlapped I/O, so we cannot poll both in one thread).
 *   - Stdout is read via PeekNamedPipe + ReadFile in a polling loop with Sleep(1)
 *     for backpressure and deadline checking.
 *
 * POSIX implementation: ctags_stream.c
 */

#include "platform.h"

#ifdef TT_PLATFORM_WINDOWS

#include "ctags_stream.h"
#include "error.h"
#include "str_util.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define INITIAL_LINE_BUF 4096
#define INITIAL_STDERR_BUF 1024
#define READ_CHUNK 4096

/* ===== Stderr drainer thread ===== */

typedef struct
{
    HANDLE hRead;               /* stderr read end (owned by parent, closed by parent) */
    char **buf;                 /* pointer to stream->stderr_buf — guarded by cs */
    size_t *buf_used;
    size_t *buf_cap;
    CRITICAL_SECTION *cs;
} stderr_drainer_ctx_t;

static DWORD WINAPI stderr_drainer_fn(LPVOID arg)
{
    stderr_drainer_ctx_t *ctx = (stderr_drainer_ctx_t *)arg;
    char tmp[READ_CHUNK];
    DWORD n;

    while (ReadFile(ctx->hRead, tmp, sizeof(tmp), &n, NULL) && n > 0)
    {
        EnterCriticalSection(ctx->cs);
        if (*ctx->buf_used + n >= *ctx->buf_cap)
        {
            size_t new_cap = *ctx->buf_cap * 2;
            if (new_cap < *ctx->buf_used + n + 1)
                new_cap = *ctx->buf_used + n + 1;
            char *nb = realloc(*ctx->buf, new_cap);
            if (nb)
            {
                *ctx->buf = nb;
                *ctx->buf_cap = new_cap;
            }
        }
        if (*ctx->buf_used + n < *ctx->buf_cap)
        {
            memcpy(*ctx->buf + *ctx->buf_used, tmp, n);
            *ctx->buf_used += n;
        }
        LeaveCriticalSection(ctx->cs);
    }

    free(arg); /* free the drainer context */
    return 0;
}

/* ===== Start ===== */

int tt_ctags_stream_start(tt_ctags_stream_t *stream,
                          const char *ctags_path,
                          const char *file_list_path,
                          int timeout_sec)
{
    if (!stream || !ctags_path || !file_list_path)
    {
        tt_error_set("ctags_stream_start: NULL argument");
        return -1;
    }

    memset(stream, 0, sizeof(*stream));

    /* Allocate CRITICAL_SECTION on the heap (avoid windows.h in header) */
    CRITICAL_SECTION *cs = malloc(sizeof(CRITICAL_SECTION));
    if (!cs)
    {
        tt_error_set("ctags_stream_start: malloc failed for CRITICAL_SECTION");
        return -1;
    }
    InitializeCriticalSection(cs);
    stream->stderr_cs = cs;

    /* Allocate line and stderr buffers */
    stream->line_buf = malloc(INITIAL_LINE_BUF);
    stream->stderr_buf = malloc(INITIAL_STDERR_BUF);
    if (!stream->line_buf || !stream->stderr_buf)
    {
        free(stream->line_buf);
        free(stream->stderr_buf);
        DeleteCriticalSection(cs);
        free(cs);
        stream->stderr_cs = NULL;
        tt_error_set("ctags_stream_start: malloc failed");
        return -1;
    }
    stream->line_buf_cap = INITIAL_LINE_BUF;
    stream->stderr_buf_cap = INITIAL_STDERR_BUF;

    if (timeout_sec > 0)
        stream->deadline_ms = tt_monotonic_ms() + (uint64_t)timeout_sec * 1000;

    /* Create pipes for stdout and stderr */
    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hStdoutRead, hStdoutWrite;
    HANDLE hStderrRead, hStderrWrite;

    if (!CreatePipe(&hStdoutRead, &hStdoutWrite, &sa, 0))
    {
        tt_error_set("ctags_stream_start: CreatePipe stdout failed (%lu)",
                     GetLastError());
        goto fail_bufs;
    }
    /* Read end must NOT be inherited by child */
    SetHandleInformation(hStdoutRead, HANDLE_FLAG_INHERIT, 0);

    if (!CreatePipe(&hStderrRead, &hStderrWrite, &sa, 0))
    {
        tt_error_set("ctags_stream_start: CreatePipe stderr failed (%lu)",
                     GetLastError());
        CloseHandle(hStdoutRead);
        CloseHandle(hStdoutWrite);
        goto fail_bufs;
    }
    SetHandleInformation(hStderrRead, HANDLE_FLAG_INHERIT, 0);

    /* Build command line.
     * ctags on Windows may be invoked via MSYS2 or native — we build a
     * standard command line with quoting. */
    tt_strbuf_t cmdline;
    tt_strbuf_init(&cmdline);
    tt_strbuf_appendf(&cmdline,
        "\"%s\" --output-format=json --fields=+neKStl --kinds-all=* "
        "--kinds-Java=-{local} --extras=-{qualified} --pseudo-tags= "
        "--sort=no \"--langmap=Markdown:+.mdx\" -L \"%s\"",
        ctags_path, file_list_path);

    /* Convert command line to wide string for CreateProcessW */
    int wlen = MultiByteToWideChar(CP_UTF8, 0, cmdline.data, -1, NULL, 0);
    wchar_t *wcmdline = malloc((size_t)wlen * sizeof(wchar_t));
    if (!wcmdline)
    {
        tt_strbuf_free(&cmdline);
        CloseHandle(hStdoutRead);
        CloseHandle(hStdoutWrite);
        CloseHandle(hStderrRead);
        CloseHandle(hStderrWrite);
        tt_error_set("ctags_stream_start: malloc failed for wide cmdline");
        goto fail_bufs;
    }
    MultiByteToWideChar(CP_UTF8, 0, cmdline.data, -1, wcmdline, wlen);
    tt_strbuf_free(&cmdline);

    /* Set up STARTUPINFOW */
    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hStdoutWrite;
    si.hStdError = hStderrWrite;
    si.hStdInput = INVALID_HANDLE_VALUE;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));

    BOOL ok = CreateProcessW(
        NULL,           /* lpApplicationName: NULL = parse from cmdline */
        wcmdline,       /* lpCommandLine: mutable wide string */
        NULL,           /* lpProcessAttributes */
        NULL,           /* lpThreadAttributes */
        TRUE,           /* bInheritHandles */
        CREATE_NO_WINDOW, /* dwCreationFlags */
        NULL,           /* lpEnvironment */
        NULL,           /* lpCurrentDirectory */
        &si,
        &pi);

    free(wcmdline);

    /* Close the write ends in the parent — child has its copies */
    CloseHandle(hStdoutWrite);
    CloseHandle(hStderrWrite);

    if (!ok)
    {
        tt_error_set("ctags_stream_start: CreateProcessW failed (%lu)",
                     GetLastError());
        CloseHandle(hStdoutRead);
        CloseHandle(hStderrRead);
        goto fail_bufs;
    }

    /* We don't need the child's main thread handle */
    CloseHandle(pi.hThread);

    stream->hProcess = pi.hProcess;
    stream->hStdoutRead = hStdoutRead;
    stream->hStderrRead = hStderrRead;

    /* Launch stderr drainer thread */
    stderr_drainer_ctx_t *dctx = malloc(sizeof(stderr_drainer_ctx_t));
    if (!dctx)
    {
        tt_error_set("ctags_stream_start: malloc failed for drainer context");
        /* Still started the process — need to clean up */
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(hStdoutRead);
        CloseHandle(hStderrRead);
        goto fail_bufs;
    }
    dctx->hRead = hStderrRead;
    dctx->buf = &stream->stderr_buf;
    dctx->buf_used = &stream->stderr_buf_used;
    dctx->buf_cap = &stream->stderr_buf_cap;
    dctx->cs = cs;

    HANDLE hThread = CreateThread(NULL, 0, stderr_drainer_fn, dctx, 0, NULL);
    if (!hThread)
    {
        tt_error_set("ctags_stream_start: CreateThread for stderr drainer failed (%lu)",
                     GetLastError());
        free(dctx);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(hStdoutRead);
        CloseHandle(hStderrRead);
        goto fail_bufs;
    }
    stream->hStderrThread = hThread;

    return 0;

fail_bufs:
    free(stream->line_buf);
    free(stream->stderr_buf);
    stream->line_buf = NULL;
    stream->stderr_buf = NULL;
    if (stream->stderr_cs)
    {
        DeleteCriticalSection((CRITICAL_SECTION *)stream->stderr_cs);
        free(stream->stderr_cs);
        stream->stderr_cs = NULL;
    }
    stream->finished = true;
    return -1;
}

/* ===== Readline ===== */

/*
 * find_newline -- Find '\n' in line_buf[start..used).
 * Returns index of '\n' (absolute position in line_buf), or -1 if not found.
 */
static int find_newline(const tt_ctags_stream_t *stream)
{
    size_t start = stream->line_buf_start;
    size_t len = stream->line_buf_used - start;
    if (len == 0)
        return -1;
    const char *found = memchr(stream->line_buf + start, '\n', len);
    return found ? (int)(found - stream->line_buf) : -1;
}

/*
 * compact_line_buf -- Move unconsumed data to front of buffer.
 */
static void compact_line_buf(tt_ctags_stream_t *stream)
{
    if (stream->line_buf_start == 0)
        return;
    size_t remaining = stream->line_buf_used - stream->line_buf_start;
    if (remaining > 0)
        memmove(stream->line_buf, stream->line_buf + stream->line_buf_start,
                remaining);
    stream->line_buf_used = remaining;
    stream->line_buf_start = 0;
}

const char *tt_ctags_stream_readline(tt_ctags_stream_t *stream,
                                     size_t *line_len)
{
    if (!stream || stream->finished)
        return NULL;

    /* Compact buffer: reclaim space from previously returned line */
    compact_line_buf(stream);

    /* Check if we already have a complete line buffered */
    int nl = find_newline(stream);
    if (nl >= 0)
        goto emit_line;

    /* Read more data from stdout until we get a newline or EOF */
    HANDLE hStdout = (HANDLE)stream->hStdoutRead;

    while (1)
    {
        /* Ensure space in line buffer */
        if (stream->line_buf_used + READ_CHUNK >= stream->line_buf_cap)
        {
            size_t new_cap = stream->line_buf_cap * 2;
            char *tmp = realloc(stream->line_buf, new_cap);
            if (!tmp)
            {
                stream->finished = true;
                tt_error_set("ctags_stream_readline: realloc failed");
                return NULL;
            }
            stream->line_buf = tmp;
            stream->line_buf_cap = new_cap;
        }

        /* Check timeout */
        if (stream->deadline_ms > 0)
        {
            uint64_t now = tt_monotonic_ms();
            if (now >= stream->deadline_ms)
            {
                stream->finished = true;
                tt_error_set("ctags_stream_readline: timeout");
                return NULL;
            }
        }

        /* Check if there's data available on stdout */
        DWORD avail = 0;
        if (!PeekNamedPipe(hStdout, NULL, 0, NULL, &avail, NULL))
        {
            /* Pipe broken or closed — EOF */
            stream->finished = true;

            /* Emit remaining buffered data as final line */
            if (stream->line_buf_used > stream->line_buf_start)
            {
                const char *result = stream->line_buf + stream->line_buf_start;
                size_t rlen = stream->line_buf_used - stream->line_buf_start;
                stream->line_buf[stream->line_buf_used] = '\0';
                if (line_len)
                    *line_len = rlen;
                stream->line_buf_used = 0;
                stream->line_buf_start = 0;
                return result;
            }
            return NULL;
        }

        if (avail == 0)
        {
            /* No data yet. Check if process is still alive. */
            DWORD exit_code;
            if (GetExitCodeProcess((HANDLE)stream->hProcess, &exit_code) &&
                exit_code != STILL_ACTIVE)
            {
                /* Process exited, but there might be buffered pipe data.
                 * Try one more PeekNamedPipe. */
                if (!PeekNamedPipe(hStdout, NULL, 0, NULL, &avail, NULL) ||
                    avail == 0)
                {
                    stream->finished = true;
                    if (stream->line_buf_used > stream->line_buf_start)
                    {
                        const char *result = stream->line_buf + stream->line_buf_start;
                        size_t rlen = stream->line_buf_used - stream->line_buf_start;
                        stream->line_buf[stream->line_buf_used] = '\0';
                        if (line_len)
                            *line_len = rlen;
                        stream->line_buf_used = 0;
                        stream->line_buf_start = 0;
                        return result;
                    }
                    return NULL;
                }
                /* avail > 0, fall through to ReadFile */
            }
            else
            {
                /* Process still running, no data yet — sleep and retry */
                Sleep(1);
                continue;
            }
        }

        /* Read available data from stdout */
        DWORD to_read = READ_CHUNK;
        if (avail < to_read)
            to_read = avail;

        DWORD bytes_read = 0;
        if (!ReadFile(hStdout,
                      stream->line_buf + stream->line_buf_used,
                      to_read, &bytes_read, NULL) || bytes_read == 0)
        {
            /* EOF or error */
            stream->finished = true;
            if (stream->line_buf_used > stream->line_buf_start)
            {
                const char *result = stream->line_buf + stream->line_buf_start;
                size_t rlen = stream->line_buf_used - stream->line_buf_start;
                stream->line_buf[stream->line_buf_used] = '\0';
                if (line_len)
                    *line_len = rlen;
                stream->line_buf_used = 0;
                stream->line_buf_start = 0;
                return result;
            }
            return NULL;
        }

        stream->line_buf_used += bytes_read;

        nl = find_newline(stream);
        if (nl >= 0)
            goto emit_line;
    }

emit_line:
    /* Strip trailing \r if present (ctags on Windows emits CRLF) */
    if (nl > 0 && stream->line_buf[nl - 1] == '\r')
    {
        stream->line_buf[nl - 1] = '\0';
        stream->line_buf[nl] = '\0';
        if (line_len)
            *line_len = (size_t)nl - 1 - stream->line_buf_start;
    }
    else
    {
        stream->line_buf[nl] = '\0';
        if (line_len)
            *line_len = (size_t)nl - stream->line_buf_start;
    }

    const char *result = stream->line_buf + stream->line_buf_start;

    /* Advance start past the newline; compaction deferred to next call */
    stream->line_buf_start = (size_t)nl + 1;

    return result;
}

/* ===== Finish ===== */

int tt_ctags_stream_finish(tt_ctags_stream_t *stream,
                           char **stderr_out)
{
    if (!stream)
        return -1;

    /* Close stdout read end to unblock the process if it's writing */
    if (stream->hStdoutRead)
    {
        CloseHandle((HANDLE)stream->hStdoutRead);
        stream->hStdoutRead = NULL;
    }

    /* Wait for stderr drainer thread to finish (pipe closes when process exits) */
    if (stream->hStderrThread)
    {
        WaitForSingleObject((HANDLE)stream->hStderrThread, 5000);
        CloseHandle((HANDLE)stream->hStderrThread);
        stream->hStderrThread = NULL;
    }

    /* Close stderr read end (drainer thread is done) */
    if (stream->hStderrRead)
    {
        CloseHandle((HANDLE)stream->hStderrRead);
        stream->hStderrRead = NULL;
    }

    /* Wait for process to exit */
    int exit_code = -1;
    if (stream->hProcess)
    {
        DWORD wait_result = WaitForSingleObject((HANDLE)stream->hProcess, 5000);
        if (wait_result == WAIT_TIMEOUT)
        {
            TerminateProcess((HANDLE)stream->hProcess, 1);
            WaitForSingleObject((HANDLE)stream->hProcess, 1000);
        }

        DWORD code;
        if (GetExitCodeProcess((HANDLE)stream->hProcess, &code))
            exit_code = (int)code;

        CloseHandle((HANDLE)stream->hProcess);
        stream->hProcess = NULL;
    }

    /* Transfer stderr to caller or free it.
     * Lock the CS one last time (drainer thread is joined, but be safe). */
    CRITICAL_SECTION *cs = (CRITICAL_SECTION *)stream->stderr_cs;

    if (stderr_out)
    {
        if (cs)
            EnterCriticalSection(cs);
        if (stream->stderr_buf_used > 0 && stream->stderr_buf)
        {
            stream->stderr_buf[stream->stderr_buf_used] = '\0';
            *stderr_out = stream->stderr_buf;
            stream->stderr_buf = NULL;
        }
        else
        {
            *stderr_out = NULL;
            free(stream->stderr_buf);
            stream->stderr_buf = NULL;
        }
        if (cs)
            LeaveCriticalSection(cs);
    }
    else
    {
        free(stream->stderr_buf);
        stream->stderr_buf = NULL;
    }

    /* Free line buffer */
    free(stream->line_buf);
    stream->line_buf = NULL;
    stream->line_buf_cap = 0;
    stream->line_buf_used = 0;

    /* Free critical section */
    if (cs)
    {
        DeleteCriticalSection(cs);
        free(cs);
        stream->stderr_cs = NULL;
    }

    stream->finished = true;

    return exit_code;
}

#endif /* TT_PLATFORM_WINDOWS */
