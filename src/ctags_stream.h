/*
 * ctags_stream.h -- Streaming pipe reader for ctags subprocess.
 *
 * Spawns ctags via fork+exec, reads stdout line by line using poll()
 * to drain both stdout and stderr concurrently (preventing pipe deadlock).
 * Supports timeout via monotonic deadline.
 */

#ifndef TT_CTAGS_STREAM_H
#define TT_CTAGS_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef TT_PLATFORM_WINDOWS

/*
 * Windows: ctags streaming via CreateProcessW + anonymous pipes.
 *
 * Stderr is drained by a dedicated thread to prevent pipe deadlock.
 * Stdout is read via PeekNamedPipe + ReadFile polling loop.
 * Handles are stored as void* to avoid #include <windows.h> in the header.
 */
typedef struct
{
    void *hProcess;        /* HANDLE: ctags process */
    void *hStdoutRead;     /* HANDLE: read end of stdout pipe */
    void *hStderrRead;     /* HANDLE: read end of stderr pipe */
    void *hStderrThread;   /* HANDLE: stderr drainer thread */
    char *line_buf;        /* [owns] reusable line buffer */
    size_t line_buf_cap;   /* allocated capacity */
    size_t line_buf_used;  /* bytes currently in buffer */
    size_t line_buf_start; /* offset of unconsumed data (deferred compaction) */
    char *stderr_buf;      /* [owns] accumulated stderr */
    size_t stderr_buf_cap;
    size_t stderr_buf_used;
    void *stderr_cs;       /* [owns] CRITICAL_SECTION* for stderr_buf access */
    uint64_t deadline_ms;  /* absolute deadline (monotonic clock), 0 = none */
    bool finished;         /* true after EOF or error */
} tt_ctags_stream_t;

#else /* POSIX */

#include <sys/types.h>

typedef struct
{
    pid_t pid;             /* ctags process PID */
    int stdout_fd;         /* read end of stdout pipe */
    int stderr_fd;         /* read end of stderr pipe */
    char *line_buf;        /* [owns] reusable line buffer */
    size_t line_buf_cap;   /* allocated capacity */
    size_t line_buf_used;  /* bytes currently in buffer */
    size_t line_buf_start; /* offset of unconsumed data (deferred compaction) */
    char *stderr_buf;      /* [owns] accumulated stderr */
    size_t stderr_buf_cap;
    size_t stderr_buf_used;
    uint64_t deadline_ms;  /* absolute deadline (monotonic clock), 0 = none */
    bool finished;         /* true after EOF or error */
} tt_ctags_stream_t;

#endif /* TT_PLATFORM_WINDOWS */

/*
 * tt_ctags_stream_start -- Start ctags subprocess with piped output.
 *
 * ctags_path:     path to the ctags binary.
 * file_list_path: path to a temp file with one absolute path per line (ctags -L).
 * timeout_sec:    timeout in seconds (0 = no timeout).
 *
 * Returns 0 on success, -1 on error.
 */
int tt_ctags_stream_start(tt_ctags_stream_t *stream,
                          const char *ctags_path,
                          const char *file_list_path,
                          int timeout_sec);

/*
 * tt_ctags_stream_readline -- Read next complete line from ctags stdout.
 *
 * Returns pointer to internal buffer (valid until next call).
 * Returns NULL on EOF, error, or timeout.
 * Sets *line_len to length (excluding newline).
 */
const char *tt_ctags_stream_readline(tt_ctags_stream_t *stream,
                                     size_t *line_len);

/*
 * tt_ctags_stream_finish -- Wait for ctags to exit. Clean up pipes and buffers.
 *
 * stderr_out: if non-NULL, set to allocated string with stderr contents.
 *             Caller must free. Set to NULL if no stderr captured.
 *
 * Returns ctags exit code (0 = success), or -1 on error.
 */
int tt_ctags_stream_finish(tt_ctags_stream_t *stream,
                           char **stderr_out);

#endif /* TT_CTAGS_STREAM_H */
