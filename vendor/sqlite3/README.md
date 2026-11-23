# SQLite

- **Version:** 3.54.1
- **Source:** <https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip>
- **License:** Public Domain (see file LICENSE in this directory)
- **Downloaded:** 2026-11-23

## Files

- `sqlite3.c` -- SQLite amalgamation (single-file build)
- `sqlite3.h` -- SQLite public API header
- `sqlite3ext.h` -- SQLite extension header
- `LICENSE` -- SQLite blessing text (public domain dedication)

## Build Flags

Compiled with `-DSQLITE_ENABLE_FTS5` for full-text search support.
See `CMakeLists.txt` for the complete list of compile definitions.

## Modifications

None. Files are unmodified from the upstream amalgamation.
