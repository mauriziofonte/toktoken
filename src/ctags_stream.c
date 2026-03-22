/*
 * ctags_stream.c -- Streaming pipe reader for ctags subprocess (POSIX).
 *
 * Windows stub: ctags_stream_win.c
 */

#include "platform.h"

#ifndef TT_PLATFORM_WINDOWS

#include "ctags_stream.h"
#include "error.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define INITIAL_LINE_BUF 4096
#define INITIAL_STDERR_BUF 1024
#define READ_CHUNK 4096

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
    stream->stdout_fd = -1;
    stream->stderr_fd = -1;
    stream->pid = -1;

    int pipe_stdout[2] = {-1, -1};
    int pipe_stderr[2] = {-1, -1};

    if (pipe(pipe_stdout) != 0)
    {
        tt_error_set("ctags_stream_start: pipe failed: %s", strerror(errno));
        return -1;
    }
    if (pipe(pipe_stderr) != 0)
    {
        tt_error_set("ctags_stream_start: pipe failed: %s", strerror(errno));
        close(pipe_stdout[0]);
        close(pipe_stdout[1]);
        return -1;
    }

    stream->line_buf = malloc(INITIAL_LINE_BUF);
    stream->stderr_buf = malloc(INITIAL_STDERR_BUF);
    if (!stream->line_buf || !stream->stderr_buf)
    {
        free(stream->line_buf);
        free(stream->stderr_buf);
        close(pipe_stdout[0]);
        close(pipe_stdout[1]);
        close(pipe_stderr[0]);
        close(pipe_stderr[1]);
        tt_error_set("ctags_stream_start: malloc failed");
        return -1;
    }
    stream->line_buf_cap = INITIAL_LINE_BUF;
    stream->stderr_buf_cap = INITIAL_STDERR_BUF;

    if (timeout_sec > 0)
        stream->deadline_ms = tt_monotonic_ms() + (uint64_t)timeout_sec * 1000;

    pid_t pid = fork();
    if (pid < 0)
    {
        tt_error_set("ctags_stream_start: fork failed: %s", strerror(errno));
        free(stream->line_buf);
        free(stream->stderr_buf);
        stream->line_buf = NULL;
        stream->stderr_buf = NULL;
        close(pipe_stdout[0]);
        close(pipe_stdout[1]);
        close(pipe_stderr[0]);
        close(pipe_stderr[1]);
        return -1;
    }

    if (pid == 0)
    {
        /* Child */
        close(pipe_stdout[0]);
        close(pipe_stderr[0]);
        dup2(pipe_stdout[1], STDOUT_FILENO);
        dup2(pipe_stderr[1], STDERR_FILENO);
        close(pipe_stdout[1]);
        close(pipe_stderr[1]);

        int devnull = open("/dev/null", 0);
        if (devnull >= 0)
        {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }

        const char *argv[] = {
            ctags_path,
            "--output-format=json",
            "--fields=+neKStl",
            "--kinds-all=*",
            "--extras=-{qualified}",
            "--pseudo-tags=",
            "--sort=no",
            "--langmap=Markdown:+.mdx",
            "-L", file_list_path,
            NULL};

        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    /* Parent */
    close(pipe_stdout[1]);
    close(pipe_stderr[1]);

    stream->pid = pid;
    stream->stdout_fd = pipe_stdout[0];
    stream->stderr_fd = pipe_stderr[0];

    return 0;
}

/* ===== Readline ===== */

/*
 * drain_stderr -- Read any available stderr data into stream->stderr_buf.
 */
static void drain_stderr(tt_ctags_stream_t *stream)
{
    if (stream->stderr_fd < 0)
        return;

    /* Non-blocking check: poll with 0 timeout */
    struct pollfd pfd = {.fd = stream->stderr_fd, .events = POLLIN};
    while (poll(&pfd, 1, 0) > 0 && (pfd.revents & (POLLIN | POLLHUP)))
    {
        if (stream->stderr_buf_used + READ_CHUNK >= stream->stderr_buf_cap)
        {
            size_t new_cap = stream->stderr_buf_cap * 2;
            char *tmp = realloc(stream->stderr_buf, new_cap);
            if (!tmp)
                break;
            stream->stderr_buf = tmp;
            stream->stderr_buf_cap = new_cap;
        }

        ssize_t n = read(stream->stderr_fd,
                         stream->stderr_buf + stream->stderr_buf_used,
                         stream->stderr_buf_cap - stream->stderr_buf_used - 1);
        if (n <= 0)
        {
            close(stream->stderr_fd);
            stream->stderr_fd = -1;
            break;
        }
        stream->stderr_buf_used += (size_t)n;

        /* Re-arm for next iteration */
        pfd.revents = 0;
    }
}

/*
 * find_newline -- Find '\n' in line_buf[start..used).
 * Returns index of '\n' (absolute position in line_buf), or -1 if not found.
 */
static ssize_t find_newline(const tt_ctags_stream_t *stream)
{
    size_t start = stream->line_buf_start;
    size_t len = stream->line_buf_used - start;
    if (len == 0)
        return -1;
    const char *found = memchr(stream->line_buf + start, '\n', len);
    return found ? (ssize_t)(found - stream->line_buf) : -1;
}

/*
 * compact_line_buf -- Move unconsumed data to front of buffer.
 * Called at the start of readline to reclaim space used by the
 * previously returned line (deferred compaction).
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
    ssize_t nl = find_newline(stream);
    if (nl >= 0)
        goto emit_line;

    /* Read more data from stdout until we get a newline or EOF */
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

        /* Compute poll timeout */
        int poll_timeout = -1;
        if (stream->deadline_ms > 0)
        {
            uint64_t now = tt_monotonic_ms();
            if (now >= stream->deadline_ms)
            {
                stream->finished = true;
                tt_error_set("ctags_stream_readline: timeout");
                return NULL;
            }
            poll_timeout = (int)(stream->deadline_ms - now);
        }

        /* Poll stdout (and stderr to prevent deadlock) */
        struct pollfd pfds[2];
        int nfds = 0;
        int stdout_idx = -1, stderr_idx = -1;

        if (stream->stdout_fd >= 0)
        {
            stdout_idx = nfds;
            pfds[nfds].fd = stream->stdout_fd;
            pfds[nfds].events = POLLIN;
            nfds++;
        }
        if (stream->stderr_fd >= 0)
        {
            stderr_idx = nfds;
            pfds[nfds].fd = stream->stderr_fd;
            pfds[nfds].events = POLLIN;
            nfds++;
        }

        if (nfds == 0)
        {
            /* Both fds closed, no more data */
            stream->finished = true;

            /* Emit remaining buffered data as final line (no trailing newline) */
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

        int pr = poll(pfds, (nfds_t)nfds, poll_timeout);
        if (pr < 0)
        {
            if (errno == EINTR)
                continue;
            stream->finished = true;
            tt_error_set("ctags_stream_readline: poll failed: %s", strerror(errno));
            return NULL;
        }
        if (pr == 0)
        {
            /* Timeout */
            stream->finished = true;
            tt_error_set("ctags_stream_readline: timeout");
            return NULL;
        }

        /* Drain stderr first (to prevent deadlock) */
        if (stderr_idx >= 0 && (pfds[stderr_idx].revents & (POLLIN | POLLHUP)))
        {
            if (stream->stderr_buf_used + READ_CHUNK >= stream->stderr_buf_cap)
            {
                size_t new_cap = stream->stderr_buf_cap * 2;
                char *tmp = realloc(stream->stderr_buf, new_cap);
                if (tmp)
                {
                    stream->stderr_buf = tmp;
                    stream->stderr_buf_cap = new_cap;
                }
            }

            ssize_t n = read(stream->stderr_fd,
                             stream->stderr_buf + stream->stderr_buf_used,
                             stream->stderr_buf_cap - stream->stderr_buf_used - 1);
            if (n <= 0)
            {
                close(stream->stderr_fd);
                stream->stderr_fd = -1;
            }
            else
            {
                stream->stderr_buf_used += (size_t)n;
            }
        }

        /* Read stdout */
        if (stdout_idx >= 0 && (pfds[stdout_idx].revents & (POLLIN | POLLHUP)))
        {
            ssize_t n = read(stream->stdout_fd,
                             stream->line_buf + stream->line_buf_used,
                             stream->line_buf_cap - stream->line_buf_used - 1);
            if (n <= 0)
            {
                close(stream->stdout_fd);
                stream->stdout_fd = -1;

                /* Drain remaining stderr */
                drain_stderr(stream);

                /* Check for remaining buffered line */
                nl = find_newline(stream);
                if (nl >= 0)
                    goto emit_line;

                /* Emit remaining data as final line */
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

            stream->line_buf_used += (size_t)n;

            nl = find_newline(stream);
            if (nl >= 0)
                goto emit_line;
        }
    }

emit_line:
    /* Null-terminate the line at the newline position */
    stream->line_buf[nl] = '\0';

    const char *result = stream->line_buf + stream->line_buf_start;
    if (line_len)
        *line_len = (size_t)nl - stream->line_buf_start;

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

    /* Close remaining open fds */
    if (stream->stdout_fd >= 0)
    {
        close(stream->stdout_fd);
        stream->stdout_fd = -1;
    }

    /* Drain any remaining stderr before closing */
    drain_stderr(stream);

    if (stream->stderr_fd >= 0)
    {
        close(stream->stderr_fd);
        stream->stderr_fd = -1;
    }

    /* Wait for child process */
    int exit_code = -1;
    if (stream->pid > 0)
    {
        int status;
        pid_t w = waitpid(stream->pid, &status, 0);
        if (w > 0 && WIFEXITED(status))
            exit_code = WEXITSTATUS(status);
        else if (w > 0 && WIFSIGNALED(status))
            exit_code = -1;
        stream->pid = -1;
    }

    /* Transfer stderr to caller or free it */
    if (stderr_out)
    {
        if (stream->stderr_buf_used > 0)
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

    stream->finished = true;

    return exit_code;
}

#endif /* !TT_PLATFORM_WINDOWS */
