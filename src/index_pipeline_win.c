/*
 * index_pipeline_win.c -- Deprecated stub.
 *
 * The indexing pipeline is now fully cross-platform in index_pipeline.c
 * using the platform abstraction layer (tt_thread_create/join, tt_tmpfile_write,
 * tt_sleep_ms, tt_getpid, tt_strcasecmp).
 *
 * This file is intentionally empty. It remains in the tree so that
 * CMake GLOB picks it up without errors. No symbols are defined here.
 */

/* Silence ISO C -Wpedantic "empty translation unit" warning. */
typedef int tt_pipeline_win_unused_;
