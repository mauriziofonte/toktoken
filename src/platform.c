/*
 * platform.c -- Unix (Linux + macOS) implementation of the platform layer.
 *
 * Uses POSIX APIs: realpath, opendir/readdir, fork/exec, stat/lstat,
 * fnmatch, clock_gettime, strftime.
 */

#ifndef TT_PLATFORM_WINDOWS

#include "platform.h"
#include "error.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include "wildmatch.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <limits.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>

#ifdef TT_PLATFORM_MACOS
#include <mach-o/dyld.h>
#endif

/* Global interrupt flag (defined in cmd_index.c, set by signal handler in main.c).
 * Checked in tt_proc_run() to kill child processes promptly on SIGINT/SIGTERM. */
extern volatile sig_atomic_t tt_interrupted;

/* ===== Path operations ===== */

char *tt_realpath(const char *path)
{
    if (!path)
    {
        tt_error_set("tt_realpath: NULL path");
        return NULL;
    }
    char *resolved = realpath(path, NULL);
    if (!resolved)
    {
        tt_error_set("tt_realpath: %s: %s", path, strerror(errno));
        return NULL;
    }
    return resolved;
}

char *tt_path_join(const char *base, const char *component)
{
    if (!base || !component)
        return NULL;

    size_t blen = strlen(base);
    size_t clen = strlen(component);

    /* Strip trailing slash from base */
    while (blen > 1 && base[blen - 1] == '/')
        blen--;

    /* Skip leading slash from component */
    while (clen > 0 && component[0] == '/')
    {
        component++;
        clen--;
    }

    size_t total = blen + 1 + clen + 1;
    char *result = malloc(total);
    if (!result)
        return NULL;

    memcpy(result, base, blen);
    result[blen] = '/';
    memcpy(result + blen + 1, component, clen);
    result[blen + 1 + clen] = '\0';
    return result;
}

const char *tt_path_basename(const char *path)
{
    if (!path)
        return "";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

const char *tt_path_extension(const char *path)
{
    const char *base = tt_path_basename(path);
    if (!base || !*base)
        return "";

    /* Special case: .blade.php */
    size_t len = strlen(base);
    if (len >= 10)
    {
        const char *suffix = base + len - 10;
        if (strcmp(suffix, ".blade.php") == 0)
            return suffix;
    }

    const char *dot = strrchr(base, '.');
    if (!dot || dot == base)
        return "";
    return dot;
}

const char *tt_home_dir(void)
{
    const char *home = getenv("HOME");
    return home ? home : "/";
}

bool tt_path_is_absolute(const char *path)
{
    return path && path[0] == '/';
}

char *tt_getcwd(void)
{
    char *buf = getcwd(NULL, 0);
    if (!buf)
    {
        tt_error_set("tt_getcwd: %s", strerror(errno));
        return NULL;
    }
    return buf;
}

char *tt_path_relative(const char *root, const char *full_path)
{
    if (!root || !full_path)
        return NULL;

    size_t rlen = strlen(root);

    /* Strip trailing slash from root */
    while (rlen > 1 && root[rlen - 1] == '/')
        rlen--;

    if (strncmp(full_path, root, rlen) != 0)
        return NULL;

    /* full_path must continue with '/' or be exactly root */
    if (full_path[rlen] == '/')
    {
        return strdup(full_path + rlen + 1);
    }
    else if (full_path[rlen] == '\0')
    {
        return strdup("");
    }
    return NULL;
}

void tt_path_normalize_sep(char *path)
{
    if (!path)
        return;
    for (char *p = path; *p; p++)
    {
        if (*p == '\\')
            *p = '/';
    }
}

/* ===== Filesystem operations ===== */

static bool stat_check(const char *path, mode_t flag)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return false;
    return (st.st_mode & S_IFMT) == flag;
}

bool tt_file_exists(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0;
}

time_t tt_file_mtime(const char *path)
{
    if (!path)
        return 0;
    struct stat st;
    if (stat(path, &st) != 0)
        return 0;
    return (time_t)TT_ST_MTIME_SEC(st);
}

bool tt_is_dir(const char *path)
{
    return path && stat_check(path, S_IFDIR);
}

bool tt_is_symlink(const char *path)
{
    if (!path)
        return false;
    struct stat st;
    return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
}

bool tt_is_file(const char *path)
{
    return path && stat_check(path, S_IFREG);
}

int64_t tt_file_size(const char *path)
{
    if (!path)
        return -1;
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    return (int64_t)st.st_size;
}

char *tt_read_file(const char *path, size_t *out_len)
{
    if (!path)
    {
        tt_error_set("tt_read_file: NULL path");
        return NULL;
    }

    FILE *f = fopen(path, "rb");
    if (!f)
    {
        tt_error_set("tt_read_file: %s: %s", path, strerror(errno));
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0)
    {
        tt_error_set("tt_read_file: %s: ftell failed", path);
        fclose(f);
        return NULL;
    }
    fseek(f, 0, SEEK_SET);

    char *buf = malloc((size_t)sz + 1);
    if (!buf)
    {
        tt_error_set("tt_read_file: malloc failed (%ld bytes)", sz);
        fclose(f);
        return NULL;
    }

    size_t nread = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[nread] = '\0';

    if (out_len)
        *out_len = nread;
    return buf;
}

int tt_write_file(const char *path, const char *data, size_t len)
{
    if (!path || !data)
    {
        tt_error_set("tt_write_file: NULL argument");
        return -1;
    }

    FILE *f = fopen(path, "wb");
    if (!f)
    {
        tt_error_set("tt_write_file: %s: %s", path, strerror(errno));
        return -1;
    }

    if (len > 0 && fwrite(data, 1, len, f) != len)
    {
        tt_error_set("tt_write_file: %s: write failed", path);
        fclose(f);
        return -1;
    }

    fclose(f);
    return 0;
}

int tt_mkdir_p(const char *path)
{
    if (!path)
        return -1;

    char *tmp = strdup(path);
    if (!tmp)
        return -1;

    size_t len = strlen(tmp);

    /* Strip trailing slash */
    if (len > 1 && tmp[len - 1] == '/')
        tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; p++)
    {
        if (*p == '/')
        {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
            {
                tt_error_set("tt_mkdir_p: %s: %s", tmp, strerror(errno));
                free(tmp);
                return -1;
            }
            *p = '/';
        }
    }

    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
    {
        tt_error_set("tt_mkdir_p: %s: %s", tmp, strerror(errno));
        free(tmp);
        return -1;
    }

    free(tmp);
    return 0;
}

int tt_remove_file(const char *path)
{
    if (!path)
        return -1;
    if (unlink(path) != 0)
    {
        tt_error_set("tt_remove_file: %s: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

int tt_remove_dir(const char *path)
{
    if (!path)
        return -1;
    if (rmdir(path) != 0)
    {
        tt_error_set("tt_remove_dir: %s: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

int tt_remove_dir_recursive(const char *path)
{
    if (!path)
        return -1;

    DIR *d = opendir(path);
    if (!d)
    {
        tt_error_set("tt_remove_dir_recursive: %s: %s", path, strerror(errno));
        return -1;
    }

    int rc = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL)
    {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        char *child = tt_path_join(path, de->d_name);
        if (!child)
        {
            rc = -1;
            break;
        }

        struct stat st;
        if (lstat(child, &st) < 0)
        {
            free(child);
            rc = -1;
            break;
        }

        if (S_ISDIR(st.st_mode))
        {
            if (tt_remove_dir_recursive(child) < 0)
                rc = -1;
        }
        else
        {
            if (unlink(child) != 0)
                rc = -1;
        }
        free(child);
        if (rc < 0)
            break;
    }
    closedir(d);

    if (rc == 0)
    {
        if (rmdir(path) != 0)
        {
            tt_error_set("tt_remove_dir_recursive: rmdir %s: %s", path, strerror(errno));
            rc = -1;
        }
    }
    return rc;
}

/* ===== Directory walking ===== */

static int walk_recursive(const char *dir, tt_walk_cb cb, void *userdata)
{
    DIR *d = opendir(dir);
    if (!d)
    {
        tt_error_set("tt_walk_dir: opendir %s: %s", dir, strerror(errno));
        return -1;
    }

    size_t dlen = strlen(dir);
    char full[PATH_MAX];

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL)
    {
        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == '\0' ||
             (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
            continue;

        size_t nlen = strlen(ent->d_name);
        if (dlen + 1 + nlen >= PATH_MAX)
            continue;

        memcpy(full, dir, dlen);
        full[dlen] = '/';
        memcpy(full + dlen + 1, ent->d_name, nlen + 1);

        bool is_dir_entry = false;
        bool is_link = false;

#ifdef _DIRENT_HAVE_D_TYPE
        if (ent->d_type != DT_UNKNOWN && ent->d_type != DT_LNK)
        {
            /* Fast path: d_type is reliable */
            is_dir_entry = (ent->d_type == DT_DIR);
        }
        else
#endif
        {
            /* Slow path: lstat for DT_UNKNOWN, DT_LNK, or no d_type */
            struct stat st;
            if (lstat(full, &st) != 0)
                continue;

            is_dir_entry = S_ISDIR(st.st_mode);
            is_link = S_ISLNK(st.st_mode);

            /* Symlink pointing to directory: mark both flags */
            if (is_link && !is_dir_entry)
            {
                struct stat st2;
                if (stat(full, &st2) == 0 && S_ISDIR(st2.st_mode))
                    is_dir_entry = true;
            }
        }

        int rc = cb(dir, ent->d_name, is_dir_entry, is_link, userdata);
        if (rc < 0)
        {
            closedir(d);
            return 0; /* stop requested, not an error */
        }

        if (is_dir_entry && rc == 0)
        {
            int sub_rc = walk_recursive(full, cb, userdata);
            if (sub_rc < 0)
            {
                closedir(d);
                return sub_rc;
            }
        }
    }

    closedir(d);
    return 0;
}

int tt_walk_dir(const char *root, tt_walk_cb cb, void *userdata)
{
    if (!root || !cb)
    {
        tt_error_set("tt_walk_dir: NULL argument");
        return -1;
    }
    return walk_recursive(root, cb, userdata);
}

/* ===== Process execution ===== */

tt_proc_result_t tt_proc_run(const char *const argv[], const char *stdin_data,
                             int timeout_ms)
{
    tt_proc_result_t result = {NULL, NULL, -1};

    if (!argv || !argv[0])
    {
        tt_error_set("tt_proc_run: NULL argv");
        return result;
    }

    int pipe_stdout[2] = {-1, -1};
    int pipe_stderr[2] = {-1, -1};
    int pipe_stdin[2] = {-1, -1};

    if (pipe(pipe_stdout) != 0 || pipe(pipe_stderr) != 0)
    {
        tt_error_set("tt_proc_run: pipe failed: %s", strerror(errno));
        return result;
    }

    if (stdin_data && pipe(pipe_stdin) != 0)
    {
        tt_error_set("tt_proc_run: pipe failed: %s", strerror(errno));
        close(pipe_stdout[0]);
        close(pipe_stdout[1]);
        close(pipe_stderr[0]);
        close(pipe_stderr[1]);
        return result;
    }

    pid_t pid = fork();
    if (pid < 0)
    {
        tt_error_set("tt_proc_run: fork failed: %s", strerror(errno));
        close(pipe_stdout[0]);
        close(pipe_stdout[1]);
        close(pipe_stderr[0]);
        close(pipe_stderr[1]);
        if (stdin_data)
        {
            close(pipe_stdin[0]);
            close(pipe_stdin[1]);
        }
        return result;
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

        if (stdin_data)
        {
            close(pipe_stdin[1]);
            dup2(pipe_stdin[0], STDIN_FILENO);
            close(pipe_stdin[0]);
        }
        else
        {
            int devnull = open("/dev/null", 0);
            if (devnull >= 0)
            {
                dup2(devnull, STDIN_FILENO);
                close(devnull);
            }
        }

        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    /* Parent */
    close(pipe_stdout[1]);
    close(pipe_stderr[1]);

    if (stdin_data)
    {
        close(pipe_stdin[0]);
        /* Write stdin data. Ignore partial writes for simplicity. */
        size_t slen = strlen(stdin_data);
        ssize_t written = write(pipe_stdin[1], stdin_data, slen);
        (void)written;
        close(pipe_stdin[1]);
    }

    /*
     * Drain stdout and stderr concurrently using poll().
     *
     * Reading them sequentially (stdout first, then stderr) causes a
     * classic pipe deadlock when the child produces enough output on
     * BOTH channels: the parent blocks reading stdout while the child
     * blocks writing to stderr (pipe buffer full, nobody draining it).
     *
     * poll() lets us read whichever fd has data available, preventing
     * either pipe buffer from filling up.
     */
    size_t out_cap = 4096, out_len = 0;
    size_t err_cap = 4096, err_len = 0;
    char *out_buf = malloc(out_cap);
    char *err_buf = malloc(err_cap);
    if (!out_buf || !err_buf)
    {
        free(out_buf);
        free(err_buf);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(pipe_stdout[0]);
        close(pipe_stderr[0]);
        tt_error_set("tt_proc_run: malloc failed");
        return result;
    }

    struct pollfd pfds[2];
    pfds[0].fd = pipe_stdout[0];
    pfds[0].events = POLLIN;
    pfds[1].fd = pipe_stderr[0];
    pfds[1].events = POLLIN;
    int open_fds = 2;

    uint64_t deadline = 0;
    if (timeout_ms > 0)
        deadline = tt_monotonic_ms() + (uint64_t)timeout_ms;

    while (open_fds > 0)
    {
        int poll_timeout = -1; /* infinite */
        if (deadline > 0)
        {
            uint64_t now = tt_monotonic_ms();
            if (now >= deadline)
            {
                kill(pid, SIGKILL);
                waitpid(pid, NULL, 0);
                close(pipe_stdout[0]);
                close(pipe_stderr[0]);
                free(out_buf);
                free(err_buf);
                tt_error_set("tt_proc_run: timeout after %d ms", timeout_ms);
                result.exit_code = -1;
                return result;
            }
            poll_timeout = (int)(deadline - now);
        }

        /* Check for interrupt (SIGINT/SIGTERM) before blocking */
        if (tt_interrupted)
        {
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            close(pipe_stdout[0]);
            close(pipe_stderr[0]);
            free(out_buf);
            free(err_buf);
            tt_error_set("tt_proc_run: interrupted");
            result.exit_code = -1;
            return result;
        }

        int pr = poll(pfds, 2, poll_timeout);
        if (pr < 0)
        {
            if (errno == EINTR)
                continue; /* re-check tt_interrupted at top */
            break;
        }
        if (pr == 0)
        {
            /* Timeout */
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            close(pipe_stdout[0]);
            close(pipe_stderr[0]);
            free(out_buf);
            free(err_buf);
            tt_error_set("tt_proc_run: timeout after %d ms", timeout_ms);
            result.exit_code = -1;
            return result;
        }

        /* Read stdout */
        if (pfds[0].fd >= 0 && (pfds[0].revents & (POLLIN | POLLHUP)))
        {
            ssize_t n = read(pfds[0].fd, out_buf + out_len, out_cap - out_len);
            if (n > 0)
            {
                out_len += (size_t)n;
                if (out_len + 1 >= out_cap)
                {
                    out_cap *= 2;
                    char *tmp = realloc(out_buf, out_cap);
                    if (tmp)
                        out_buf = tmp;
                }
            }
            else
            {
                pfds[0].fd = -1; /* EOF or error — stop polling this fd */
                open_fds--;
            }
        }

        /* Read stderr */
        if (pfds[1].fd >= 0 && (pfds[1].revents & (POLLIN | POLLHUP)))
        {
            ssize_t n = read(pfds[1].fd, err_buf + err_len, err_cap - err_len);
            if (n > 0)
            {
                err_len += (size_t)n;
                if (err_len + 1 >= err_cap)
                {
                    err_cap *= 2;
                    char *tmp = realloc(err_buf, err_cap);
                    if (tmp)
                        err_buf = tmp;
                }
            }
            else
            {
                pfds[1].fd = -1;
                open_fds--;
            }
        }
    }

    out_buf[out_len] = '\0';
    err_buf[err_len] = '\0';
    result.stdout_buf = out_buf;
    result.stderr_buf = err_buf;

    close(pipe_stdout[0]);
    close(pipe_stderr[0]);

    /* Wait for child (pipes are closed, child should exit promptly).
     * With SA_RESTART disabled, waitpid() can return -1/EINTR on signal.
     * If interrupted, kill the child and reap it. */
    int status = 0;
    pid_t wp;
    while ((wp = waitpid(pid, &status, 0)) < 0 && errno == EINTR)
    {
        if (tt_interrupted)
        {
            kill(pid, SIGKILL);
            /* One more waitpid to reap — cannot leave zombies */
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
                ;
            result.exit_code = -1;
            return result;
        }
    }

    if (wp > 0 && WIFEXITED(status))
    {
        result.exit_code = WEXITSTATUS(status);
    }
    else if (wp > 0 && WIFSIGNALED(status))
    {
        result.exit_code = -1;
    }
    else
    {
        result.exit_code = -1;
    }

    return result;
}

void tt_proc_result_free(tt_proc_result_t *r)
{
    if (!r)
        return;
    free(r->stdout_buf);
    free(r->stderr_buf);
    r->stdout_buf = NULL;
    r->stderr_buf = NULL;
}

void tt_sleep_ms(int ms)
{
    if (ms <= 0)
        return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* ===== Thread abstraction (POSIX) ===== */

typedef struct
{
    tt_thread_fn fn;
    void *arg;
} tt_thread_trampoline_t;

static void *thread_trampoline(void *raw)
{
    tt_thread_trampoline_t *t = (tt_thread_trampoline_t *)raw;
    t->fn(t->arg);
    free(t);
    return NULL;
}

int tt_thread_create(tt_thread_t *thread, tt_thread_fn fn, void *arg)
{
    tt_thread_trampoline_t *t = malloc(sizeof(tt_thread_trampoline_t));
    if (!t)
        return -1;
    t->fn = fn;
    t->arg = arg;
    if (pthread_create(thread, NULL, thread_trampoline, t) != 0)
    {
        free(t);
        return -1;
    }
    return 0;
}

int tt_thread_join(tt_thread_t thread)
{
    return pthread_join(thread, NULL) == 0 ? 0 : -1;
}

char *tt_tmpfile_write(const char *prefix, const char *data, size_t len)
{
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/%s_%d_XXXXXX",
             prefix ? prefix : "tt", (int)getpid());
    int fd = mkstemp(tmp_path);
    if (fd < 0)
        return NULL;
    ssize_t written = write(fd, data, len);
    close(fd);
    if (written < 0 || (size_t)written != len)
    {
        unlink(tmp_path);
        return NULL;
    }
    return strdup(tmp_path);
}

int tt_getpid(void)
{
    return (int)getpid();
}

/* ===== Which ===== */

char *tt_which(const char *name)
{
    if (!name)
        return NULL;

    /* If name contains '/', check directly */
    if (strchr(name, '/'))
    {
        if (access(name, X_OK) == 0)
            return strdup(name);
        return NULL;
    }

    const char *path_env = getenv("PATH");
    if (!path_env)
        return NULL;

    char *path_copy = strdup(path_env);
    if (!path_copy)
        return NULL;

    char *saveptr = NULL;
    char *dir = strtok_r(path_copy, ":", &saveptr);
    while (dir)
    {
        char *full = tt_path_join(dir, name);
        if (full && access(full, X_OK) == 0)
        {
            free(path_copy);
            return full;
        }
        free(full);
        dir = strtok_r(NULL, ":", &saveptr);
    }

    free(path_copy);
    return NULL;
}

/* ===== String utilities (platform-specific) ===== */

int tt_strcasecmp(const char *a, const char *b)
{
    return strcasecmp(a, b);
}

int tt_strncasecmp(const char *a, const char *b, size_t n)
{
    return strncasecmp(a, b, n);
}

const char *tt_strcasestr(const char *haystack, const char *needle)
{
    if (!haystack || !needle)
        return NULL;
    if (!*needle)
        return haystack;

    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++)
    {
        if (strncasecmp(p, needle, nlen) == 0)
            return p;
    }
    return NULL;
}

bool tt_fnmatch(const char *pattern, const char *string, bool case_insensitive)
{
    if (!pattern || !string)
        return false;
    int flags = case_insensitive ? WM_CASEFOLD : 0;
    return wildmatch(pattern, string, flags) == WM_MATCH;
}

bool tt_fnmatch_ex(const char *pattern, const char *string, int flags)
{
    if (!pattern || !string)
        return false;
    int wm_flags = WM_WILDSTAR;
    if (flags & TT_FNM_PATHNAME)
        wm_flags |= WM_PATHNAME;
    if (flags & TT_FNM_CASEFOLD)
        wm_flags |= WM_CASEFOLD;
    return wildmatch(pattern, string, wm_flags) == WM_MATCH;
}

/* ===== Time ===== */

const char *tt_now_rfc3339(void)
{
    static _Thread_local char buf[64];

    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);

    /*
     * PHP date('c') produces: 2026-03-10T15:30:00+01:00
     * strftime %z gives:      +0100  (no colon)
     * We need to insert the colon manually.
     */
    char tmp[64];
    strftime(tmp, sizeof(tmp), "%Y-%m-%dT%H:%M:%S%z", &tm);

    /* Insert colon in timezone: +0100 -> +01:00 */
    size_t len = strlen(tmp);
    if (len >= 5)
    {
        /* Copy all except last 2 chars, insert colon */
        memcpy(buf, tmp, len - 2);
        buf[len - 2] = ':';
        memcpy(buf + len - 1, tmp + len - 2, 2);
        buf[len + 1] = '\0';
    }
    else
    {
        /* Fallback: just copy as-is */
        memcpy(buf, tmp, len + 1);
    }

    return buf;
}

uint64_t tt_monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* ===== Self-update support ===== */

char *tt_self_exe_path(void)
{
#ifdef TT_PLATFORM_MACOS
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0)
    {
        return tt_realpath(buf);
    }
    return NULL;
#else
    /* Linux: /proc/self/exe */
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0)
    {
        buf[len] = '\0';
        return strdup(buf);
    }
    return NULL;
#endif
}

int tt_rename_file(const char *src, const char *dst)
{
    if (!src || !dst)
        return -1;
    if (rename(src, dst) != 0)
    {
        tt_error_set("tt_rename_file: %s -> %s: %s", src, dst, strerror(errno));
        return -1;
    }
    return 0;
}

int tt_file_set_executable(const char *path)
{
    if (!path)
        return -1;
    if (chmod(path, 0755) != 0)
    {
        tt_error_set("tt_file_set_executable: %s: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

#else
/* Avoid ISO C empty translation unit warning (-Wpedantic) on Windows builds. */
typedef int tt_platform_unix_unused;
#endif /* !TT_PLATFORM_WINDOWS */
