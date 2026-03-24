/*
 * storage_paths.h -- Compute storage paths for TokToken databases.
 *
 * Base directory: ~/.cache/toktoken
 * Each project gets a subdirectory named by the first 12 hex chars
 * of SHA-256(realpath(project_path)).
 *
 * Migration: on first access, if ~/.cache/.toktoken exists and
 * ~/.cache/toktoken does not, the old directory is atomically renamed.
 */

#ifndef TT_STORAGE_PATHS_H
#define TT_STORAGE_PATHS_H

/*
 * tt_storage_base_dir -- Base storage directory (~/.cache/toktoken).
 *
 * [caller-frees] Returns NULL on error.
 */
char *tt_storage_base_dir(void);

/*
 * tt_storage_projects_dir -- Projects directory (~/.cache/toktoken/projects).
 *
 * [caller-frees] Returns NULL on error.
 */
char *tt_storage_projects_dir(void);

/*
 * tt_storage_project_dir -- Project-specific directory.
 *
 * Computed as {base}/projects/{hash12}/ where hash12 is the first 12 hex
 * characters of SHA-256(realpath(project_path)). If realpath fails, uses
 * the original project_path for hashing.
 *
 * [caller-frees] Returns NULL on error.
 */
char *tt_storage_project_dir(const char *project_path);

/*
 * tt_storage_db_path -- Full path to the project's SQLite database.
 *
 * Returns {project_dir}/db.sqlite.
 *
 * [caller-frees] Returns NULL on error.
 */
char *tt_storage_db_path(const char *project_path);

/*
 * tt_storage_logs_dir -- Logs directory (~/.cache/toktoken/logs).
 *
 * [caller-frees] Returns NULL on error.
 */
char *tt_storage_logs_dir(void);

#endif /* TT_STORAGE_PATHS_H */
