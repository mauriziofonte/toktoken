/*
 * platform.h -- OS abstraction layer.
 *
 * Isolates all platform-specific code behind uniform APIs.
 * The rest of the codebase uses these functions without #ifdef.
 *
 * Implementations:
 *   platform.c      -- Unix (Linux + macOS)
 *   platform_win.c  -- Windows
 */

#ifndef TT_PLATFORM_H
#define TT_PLATFORM_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* === Path operations === */

/*
 * tt_realpath -- Resolve symlinks and normalize path.
 *
 * [caller-frees] Returns NULL on failure (sets tt_error).
 */
char *tt_realpath(const char *path);

/*
 * tt_path_join -- Join two path components with separator.
 *
 * [caller-frees] Returns new string "base/component".
 */
char *tt_path_join(const char *base, const char *component);

/*
 * tt_path_basename -- Get filename from path.
 *
 * [borrows] Returns pointer into existing string. Do not free.
 */
const char *tt_path_basename(const char *path);

/*
 * tt_path_extension -- Get file extension (includes dot, e.g. ".php").
 *
 * Special: ".blade.php" returns ".blade.php".
 * Returns "" if no extension.
 * [borrows] Returns pointer into existing string. Do not free.
 */
const char *tt_path_extension(const char *path);

/*
 * tt_home_dir -- Get user home directory.
 *
 * [borrows] Returns static buffer. Do not free.
 */
const char *tt_home_dir(void);

/*
 * tt_path_is_absolute -- Check if path is absolute.
 */
bool tt_path_is_absolute(const char *path);

/*
 * tt_getcwd -- Get current working directory.
 *
 * [caller-frees] Returns NULL on error.
 */
char *tt_getcwd(void);

/*
 * tt_path_relative -- Compute relative path from root to full_path.
 *
 * Both paths should already be resolved (realpath'd).
 * [caller-frees] Returns NULL if full_path is not under root.
 */
char *tt_path_relative(const char *root, const char *full_path);

/*
 * tt_path_normalize_sep -- Normalize path separators in-place.
 *
 * Converts backslash to forward slash.
 */
void tt_path_normalize_sep(char *path);

/* === Filesystem operations === */

bool tt_file_exists(const char *path);
bool tt_is_dir(const char *path);
bool tt_is_symlink(const char *path);
bool tt_is_file(const char *path);

/*
 * tt_file_mtime -- Get file modification time.
 *
 * Returns 0 on error.
 */
time_t tt_file_mtime(const char *path);

/*
 * tt_file_size -- Get file size in bytes.
 *
 * Returns -1 on error.
 */
int64_t tt_file_size(const char *path);

/*
 * tt_read_file -- Read entire file into memory.
 *
 * [caller-frees] Sets *out_len if non-NULL. Returns NULL on error.
 */
char *tt_read_file(const char *path, size_t *out_len);

/*
 * tt_write_file -- Write data to file.
 *
 * Returns 0 on success, -1 on error.
 */
int tt_write_file(const char *path, const char *data, size_t len);

/*
 * tt_mkdir_p -- Create directory and parents.
 *
 * Returns 0 on success, -1 on error.
 */
int tt_mkdir_p(const char *path);

/*
 * tt_remove_file -- Remove a file.
 *
 * Returns 0 on success, -1 on error.
 */
int tt_remove_file(const char *path);

/*
 * tt_remove_dir -- Remove an empty directory.
 *
 * Returns 0 on success, -1 on error.
 */
int tt_remove_dir(const char *path);

/*
 * tt_remove_dir_recursive -- Remove a directory and all its contents.
 *
 * Recursively removes all files and subdirectories within path,
 * then removes the directory itself.
 *
 * Returns 0 on success, -1 on error.
 */
int tt_remove_dir_recursive(const char *path);

/* === Directory walking === */

/*
 * Callback for directory walk.
 *
 * dir:        directory path (without trailing separator)
 * name:       entry name (basename only)
 * is_dir:     true if entry is a directory
 * is_symlink: true if entry is a symlink
 * Return:     0 to continue, 1 to skip (if dir), -1 to stop entirely
 */
typedef int (*tt_walk_cb)(const char *dir, const char *name,
                          bool is_dir, bool is_symlink, void *userdata);

/*
 * tt_walk_dir -- Walk directory tree recursively.
 *
 * Calls cb for each entry. Returns 0 on success, -1 on error.
 */
int tt_walk_dir(const char *root, tt_walk_cb cb, void *userdata);

/* === Process execution === */

/*
 * Result of a subprocess execution.
 */
typedef struct
{
    char *stdout_buf; /* [owns] captured stdout (null-terminated) */
    char *stderr_buf; /* [owns] captured stderr (null-terminated) */
    int exit_code;    /* process exit code (-1 if failed to launch) */
} tt_proc_result_t;

/*
 * tt_proc_run -- Run a subprocess.
 *
 * argv:       NULL-terminated argument array (argv[0] is the program).
 * stdin_data: data to pipe to stdin, or NULL.
 * timeout_ms: timeout in milliseconds, 0 means no timeout.
 * Returns result. Caller must call tt_proc_result_free().
 */
tt_proc_result_t tt_proc_run(const char *const argv[], const char *stdin_data,
                             int timeout_ms);

/*
 * tt_proc_result_free -- Free a process result.
 */
void tt_proc_result_free(tt_proc_result_t *r);

/*
 * tt_sleep_ms -- Sleep for the specified number of milliseconds.
 */
void tt_sleep_ms(int ms);

/*
 * tt_which -- Check if a binary exists in PATH.
 *
 * [caller-frees] Returns full path or NULL if not found.
 */
char *tt_which(const char *name);

/* === String utilities (platform-specific) === */

int tt_strcasecmp(const char *a, const char *b);
int tt_strncasecmp(const char *a, const char *b, size_t n);

/*
 * tt_strcasestr -- Case-insensitive substring search.
 *
 * [borrows] Returns pointer into haystack or NULL.
 */
const char *tt_strcasestr(const char *haystack, const char *needle);

/*
 * tt_fnmatch -- fnmatch-compatible glob matching.
 */
bool tt_fnmatch(const char *pattern, const char *string, bool case_insensitive);

/* Flags for tt_fnmatch_ex */
#define TT_FNM_CASEFOLD 1 /* Case-insensitive matching */
#define TT_FNM_PATHNAME 2 /* '*' does not match '/' */

/*
 * tt_fnmatch_ex -- Extended fnmatch with pathname mode and ** globstar.
 *
 * Supports:
 *   - '**' matches zero or more path segments (including separators)
 *   - TT_FNM_PATHNAME: '*' does not match '/'
 *   - TT_FNM_CASEFOLD: case-insensitive matching
 */
bool tt_fnmatch_ex(const char *pattern, const char *string, int flags);

/* === Thread abstraction === */

/*
 * Lightweight thread abstraction for the indexing pipeline.
 * Thread function takes void* and returns nothing.
 */
typedef void (*tt_thread_fn)(void *arg);

#ifdef TT_PLATFORM_WINDOWS
typedef void *tt_thread_t; /* HANDLE */
#else
#include <pthread.h>
typedef pthread_t tt_thread_t;
#endif

/*
 * tt_thread_create -- Start a new thread.
 *
 * Returns 0 on success, -1 on error.
 */
int tt_thread_create(tt_thread_t *thread, tt_thread_fn fn, void *arg);

/*
 * tt_thread_join -- Wait for thread to finish and release resources.
 *
 * Returns 0 on success, -1 on error.
 */
int tt_thread_join(tt_thread_t thread);

/*
 * tt_tmpfile_write -- Create a temp file with the given content.
 *
 * prefix: a prefix string for the temp file name.
 * data:   content to write.
 * len:    content length.
 *
 * [caller-frees] Returns the temp file path, or NULL on error.
 */
char *tt_tmpfile_write(const char *prefix, const char *data, size_t len);

/*
 * tt_getpid -- Get current process ID.
 */
int tt_getpid(void);

/* === Self-update support === */

/*
 * tt_self_exe_path -- Get the absolute path of the currently running binary.
 *
 * Linux:   readlink("/proc/self/exe")
 * macOS:   _NSGetExecutablePath() + realpath()
 * Windows: GetModuleFileNameW()
 *
 * [caller-frees] Returns NULL on failure.
 */
char *tt_self_exe_path(void);

/*
 * tt_rename_file -- Atomically rename src to dst (replaces dst if it exists).
 *
 * POSIX:   rename() (atomic on same filesystem).
 * Windows: MoveFileExW(MOVEFILE_REPLACE_EXISTING) with fallback.
 *
 * Returns 0 on success, -1 on error (sets tt_error).
 */
int tt_rename_file(const char *src, const char *dst);

/*
 * tt_file_set_executable -- Set executable permission on a file.
 *
 * POSIX:   chmod(path, 0755).
 * Windows: no-op (all files are executable).
 *
 * Returns 0 on success, -1 on error.
 */
int tt_file_set_executable(const char *path);

/* === Time === */

/*
 * tt_now_rfc3339 -- Current time as RFC 3339 string.
 *
 * Format: "2026-03-10T15:30:00+01:00"
 * Must match PHP date('c') output exactly.
 * [borrows] Returns static buffer. Do not free.
 */
const char *tt_now_rfc3339(void);

/*
 * tt_monotonic_ms -- Monotonic time in milliseconds.
 *
 * For elapsed timing, not wall-clock.
 */
uint64_t tt_monotonic_ms(void);

/* === Portability: struct stat mtime access === */

/*
 * Sub-second mtime differs across platforms:
 *   Windows:      st_mtime      (time_t, seconds only)
 *   macOS:        st_mtimespec  (struct timespec)
 *   Linux/glibc:  st_mtim       (struct timespec, POSIX.1-2008)
 *
 * Detection uses ONLY project-controlled TT_PLATFORM_* macros (set by CMake)
 * to avoid ambiguity from feature-test macros (_GNU_SOURCE, _DEFAULT_SOURCE)
 * that may leak across toolchains during cross-compilation.
 *
 * These macros always yield int64_t, so call sites never touch
 * platform-specific struct members directly.
 */
#if defined(TT_PLATFORM_WINDOWS)
#define TT_ST_MTIME_SEC(st) ((int64_t)(st).st_mtime)
#define TT_ST_MTIME_NSEC(st) ((int64_t)0)
#elif defined(TT_PLATFORM_MACOS)
#define TT_ST_MTIME_SEC(st) ((int64_t)(st).st_mtimespec.tv_sec)
#define TT_ST_MTIME_NSEC(st) ((int64_t)(st).st_mtimespec.tv_nsec)
#else
/* Linux and other POSIX: st_mtim (POSIX.1-2008) */
#define TT_ST_MTIME_SEC(st) ((int64_t)(st).st_mtim.tv_sec)
#define TT_ST_MTIME_NSEC(st) ((int64_t)(st).st_mtim.tv_nsec)
#endif

#endif /* TT_PLATFORM_H */
