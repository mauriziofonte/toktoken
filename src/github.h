/*
 * github.h -- GitHub repository operations via gh CLI.
 *
 * Provides clone, pull, validation, and gh CLI availability checks.
 * All shell interactions use tt_proc_run() with argument arrays (no
 * string interpolation) to prevent command injection.
 *
 * Repository clones are stored under ~/.cache/toktoken/gh-repos/{owner}/{repo}/.
 */

#ifndef TT_GITHUB_H
#define TT_GITHUB_H

#include <stdbool.h>
#include <stddef.h>

/* ---- gh CLI checks ---- */

/*
 * tt_gh_available -- Check if `gh` CLI exists in PATH.
 */
bool tt_gh_available(void);

/*
 * tt_gh_authenticated -- Check if `gh` is authenticated.
 *
 * Runs `gh auth status` and checks exit code.
 * On failure, sets tt_error with diagnostic message.
 */
bool tt_gh_authenticated(void);

/*
 * tt_gh_check -- Full check: gh available + authenticated.
 *
 * On failure, sets tt_error with user-friendly message including
 * installation/configuration instructions.
 *
 * Returns 0 if ok, -1 if gh is missing or not authenticated.
 */
int tt_gh_check(void);

/*
 * tt_gh_reset_path_cache -- Invalidate cached gh/git resolved paths.
 *
 * Must be called after modifying PATH in tests or when the environment
 * changes. Without this, cached paths from previous lookups persist.
 */
void tt_gh_reset_path_cache(void);

/* ---- Input validation ---- */

/*
 * tt_gh_validate_repo -- Validate "owner/repo" format.
 *
 * Rules:
 *   - Exactly one "/"
 *   - owner: 1-39 chars, alphanumeric + hyphen, no leading hyphen
 *   - repo: 1-100 chars, alphanumeric + hyphen + underscore + dot, no leading dot
 *   - No path traversal (..), no whitespace
 *   - Normalizes to lowercase
 *
 * On success, writes normalized owner and repo to output buffers.
 * Returns 0 if valid, -1 if invalid (sets tt_error).
 */
int tt_gh_validate_repo(const char *repo_spec,
                         char *owner_out, size_t owner_size,
                         char *repo_out, size_t repo_size);

/* ---- Repository directory management ---- */

/*
 * tt_gh_repos_base_dir -- Base directory for cloned repos (~/.cache/toktoken/gh-repos).
 *
 * [caller-frees] Returns NULL on error.
 */
char *tt_gh_repos_base_dir(void);

/*
 * tt_gh_repo_dir -- Full path for a specific repo clone.
 *
 * Returns ~/.cache/toktoken/gh-repos/{owner}/{repo}/
 * Creates intermediate directories if needed.
 *
 * [caller-frees] Returns NULL on error.
 */
char *tt_gh_repo_dir(const char *owner, const char *repo);

/*
 * tt_gh_repo_exists -- Check if repo has been cloned.
 *
 * Verifies the directory exists and contains a .git directory.
 */
bool tt_gh_repo_exists(const char *owner, const char *repo);

/* ---- Clone and pull ---- */

/*
 * tt_gh_clone -- Clone a GitHub repository.
 *
 * Uses `gh repo clone owner/repo target_dir`.
 * depth: if > 0, shallow clone with --depth N. Default should be 1.
 * branch: if non-NULL, clone specific branch with -b.
 *
 * On failure, cleans up partial directory. Sets tt_error.
 * Returns 0 on success, -1 on error.
 */
int tt_gh_clone(const char *owner, const char *repo,
                const char *target_dir, int depth, const char *branch);

/*
 * tt_gh_pull -- Update an already-cloned repository.
 *
 * Runs `git -C target_dir pull --ff-only`.
 * out_message: if non-NULL, receives the stdout output (caller-frees).
 *
 * Returns 0 on success, -1 on error (sets tt_error).
 */
int tt_gh_pull(const char *target_dir, char **out_message);

/* ---- Cleanup ---- */

/*
 * tt_gh_remove_repo -- Remove a cloned repository.
 *
 * Deletes the directory ~/.cache/toktoken/gh-repos/{owner}/{repo}/ recursively.
 * Returns 0 on success, -1 on error.
 */
int tt_gh_remove_repo(const char *owner, const char *repo);

/*
 * tt_gh_remove_all_repos -- Remove all cloned repositories.
 *
 * Deletes ~/.cache/toktoken/gh-repos/ recursively.
 * Returns 0 on success, -1 on error.
 */
int tt_gh_remove_all_repos(void);

/*
 * tt_gh_list_entry_t -- Info about one cloned repository.
 */
typedef struct {
    char *owner;        /* [owns] */
    char *repo;         /* [owns] */
    char *local_path;   /* [owns] full path */
} tt_gh_list_entry_t;

/*
 * tt_gh_list_repos -- List all cloned repositories.
 *
 * out_entries: receives array of entries. [caller-frees each + array]
 * out_count: number of entries.
 * Returns 0 on success, -1 on error.
 */
int tt_gh_list_repos(tt_gh_list_entry_t **out_entries, int *out_count);

/*
 * tt_gh_list_entry_free -- Free a single list entry.
 */
void tt_gh_list_entry_free(tt_gh_list_entry_t *entry);

#endif /* TT_GITHUB_H */
