# TokToken Architecture

System architecture for developers and contributors.

---

## Data Flow

```
CLI / MCP Server
    |
    v
Command Dispatch (cmd_*.c)
    |
    v
Index Store (index_store.c) <---> SQLite (database.c)
    |                                  |
    v                                  v
File Filter --> Ctags Runner      Schema (schema.c)
    |               |
    v               v
Source Analyzer  Custom Parsers
    |
    v
Symbol Scorer / Text Search
```

### CLI Mode

`main.c` parses arguments via `cli.c`, dispatches to the appropriate `tt_cmd_*` function in `cmd_*.c`. Commands include `cmd_index.c` (index:create/update), `cmd_index.c` (index:file), `cmd_search.c` (search:symbols/text/cooccurrence/similar), `cmd_inspect.c` (inspect:outline/symbol/file/tree/dependencies/hierarchy/cycles/blast), `cmd_bundle.c` (inspect:bundle), `cmd_find.c` (find:importers/references/callers/dead), `cmd_github.c` (index:github), `cmd_manage.c` (stats/projects:list/cache:clear/codebase:detect), `cmd_help.c` (help), and `cmd_serve.c` (serve). Commands use `index_store.c` for all database operations. The `cmd_update.c` module handles `--self-update`: it downloads the latest release binary, verifies SHA-256, and atomically replaces the running executable.

### MCP Mode

`toktoken serve` starts `mcp_server.c`, which reads JSON-RPC 2.0 requests from STDIN and writes responses to STDOUT. Each request is dispatched to a tool handler in `mcp_tools.c`. Every response includes a `_ttk` metadata envelope with timestamp, duration, version, staleness info, and an optional `update_available` field when a newer version is detected upstream.

---

## Indexing Pipeline

1. **File discovery** (`file_filter.c`): walks the project directory, applies gitignore rules, security filters, smart filter (non-code exclusion + vendor detection), and file size limits.
2. **Language detection** (`language_detector.c`): maps file extensions to languages. Supports custom mappings via config.
3. **Symbol extraction**: dispatches to universal-ctags (`ctags_resolver.c` / `ctags_stream.c`) or one of 14 custom parsers (`parser_*.c`) based on language.
4. **Source analysis** (`source_analyzer.c`): extracts docstrings, signatures, byte offsets, and content hashes.
5. **Summarization** (`summarizer.c`): generates one-line summaries from signatures and docstrings.
6. **Storage** (`index_store.c`): inserts files, symbols, and imports into SQLite. FTS5 index is kept in sync via triggers.
7. **Import extraction** (`import_extractor.c`): parses import/include/require statements for dependency graph.

### Incremental Updates

`index:update` compares xxHash (XXH3) content hashes of on-disk files against stored hashes. Only changed files are re-processed. Deleted files are removed from the index.

### Smart Filter (default: on)

When active, the smart filter applies two additional exclusion layers during file discovery:

1. **Non-code extensions**: CSS, SCSS, LESS, SASS, HTML, HTM, SVG, TOML, GraphQL, XML, XUL, YAML, YML are excluded because ctags extracts selectors/tags as "symbols" that pollute search results (e.g., 38,000 CSS class selectors vs. actual code functions).

2. **Vendor manifest detection**: Subdirectories containing package manager manifests (composer.json, package.json, setup.py, Cargo.toml, go.mod, pom.xml, build.gradle, Gemfile, pyproject.toml) are pruned as likely vendored third-party code.

Disable with `--full` flag, `"smart_filter": false` in config, or the `full` MCP parameter.

---

## Storage

TokToken stores all data outside the project directory. Nothing is written inside the user's codebase.

### Directory layout

```text
~/.cache/.toktoken/                    Base directory (dot-prefixed under .cache)
    projects/                           One subdirectory per indexed project
        <hash>/                         12-char xxHash of canonical project path
            db.sqlite                   SQLite database (WAL mode)
            db.sqlite-wal              WAL journal (auto-managed by SQLite)
            db.sqlite-shm              Shared memory file (auto-managed by SQLite)
    gh-repos/                           GitHub repos cloned via index:github
        <owner>/<repo>/                 Shallow clone of the repository
    logs/                               Diagnostic logs (7-day auto-rotation)
    UPSTREAM_VERSION                    Cached latest release version (12h refresh)
```

Configuration files (optional, not created by default):

```text
~/.toktoken.json                       Global config (all sections: index, logging)
<project>/.toktoken.json               Project config (index section only)
```

### Path resolution

The project hash is computed by:

1. Resolving the canonical (real) path of the project directory via `realpath()`
2. Computing xxHash (XXH3_64bits) of the canonical path string
3. Taking the first 12 hex characters as the subdirectory name

This ensures each project gets a unique, stable index location regardless of symlinks or relative paths.

### Database lifecycle

- `index:create` creates the base directory hierarchy (if not present), opens the SQLite database in WAL mode, and populates it.
- `index:update` compares on-disk file hashes against stored hashes. Only changed files are re-processed; deleted files are removed from the index.
- `cache:clear` deletes the `db.sqlite` file and its WAL/SHM journals for a single project.
- `cache:clear --all --force` removes all project subdirectories under `~/.cache/.toktoken/projects/`.

SQLite is opened with 6 PRAGMAs: `journal_mode=WAL` (concurrent reads), `foreign_keys=ON`, `synchronous=NORMAL`, `busy_timeout=5000`, `cache_size=-8000` (~8 MB page cache), and `mmap_size=268435456` (256 MB memory-mapped I/O).

### Schema

Current schema version: **4**. Tables:

| Table | Purpose |
| ----- | ------- |
| `metadata` | Key-value store (schema_version, indexed_at, git_head) |
| `files` | Indexed files with path, hash, language, size |
| `symbols` | Extracted symbols with name, kind, signature, byte offsets |
| `imports` | Import/include relationships between files |
| `symbols_fts` | FTS5 virtual table for full-text search on symbols |
| `file_centrality` | Import-graph centrality scores per file (v4) |
| `savings_totals` | Cumulative token savings metrics |
| `savings_per_tool` | Per-tool token savings breakdown (v4) |

The migration backbone in `schema.c` supports schema upgrades via sequential `if (version < N)` checks. The v2→v3 migration removed 4 redundant B-tree indexes. The v3→v4 migration added the `file_centrality` table for import-graph-based ranking in search results.

### Indexes

Schema v4 maintains 3 B-tree indexes on `symbols`, 1 on `files`, and 4 on `imports`:

| Index | Columns | Used by |
| ----- | ------- | ------- |
| `idx_symbols_file_line` | (file, line) | inspect:outline, search:cooccurrence, find:callers, inspect:hierarchy |
| `idx_symbols_kind_lang` | (kind, language) | search:symbols filter, inspect:hierarchy |
| `idx_symbols_parent` | (parent_id) | inspect:hierarchy child lookups |
| `idx_files_language` | (language) | stats |
| `idx_imports_source` | (source_file) | delete by file |
| `idx_imports_target` | (target_name) | find:importers, inspect:dependencies, find:callers |
| `idx_imports_resolved` | (resolved_file) | inspect:cycles, inspect:blast, centrality computation |
| `idx_imports_kind` | (kind) | find:references |

During bulk insert (`index:create`), secondary indexes are dropped and rebuilt after all data is inserted. This avoids incremental B-tree maintenance during high-throughput writes.

### Database sizing

Storage scales linearly with symbol count at approximately 0.7 KB/symbol. Benchmarked across 8 open-source projects:

| Scale | Example | Files | Symbols | DB size |
| ----- | ------- | ----- | ------- | ------- |
| Small | curl | 1,108 | 33,973 | 20 MB |
| Medium | Django | 2,945 | 93,254 | 72 MB |
| Large | Kubernetes | 12,881 | 294,753 | 331 MB |
| Very large | dotnet/runtime | 37,668 | 1,241,626 | 1,344 MB |
| Massive | Linux kernel | 65,231 | 7,433,275 | 5,188 MB |

---

## Performance Characteristics

Benchmarked on Intel Core i9-12900H (20 threads), 32 GiB DDR5, NVMe SSD, Linux 6.6.87 (WSL2). Full details in [PERFORMANCE.md](PERFORMANCE.md).

### Indexing

The pipeline runs 16 parallel ctags workers with an MPSC queue feeding a single SQLite writer thread. Timing breakdown for representative projects:

| Phase | curl (1K files) | Kubernetes (13K files) | Linux (65K files) |
| ----- | --------------- | ---------------------- | ----------------- |
| Discovery | 32 ms | 813 ms | 2,080 ms |
| Pipeline (ctags + DB writes) | 517 ms | 3,520 ms | 59,560 ms |
| Summary generation | 86 ms | 1,300 ms | 30,380 ms |
| Schema rebuild (B-tree + FTS5 + centrality) | 109 ms | 2,200 ms | 59,270 ms |
| **Total** | **0.80 s** | **10.13 s** | **171.17 s** |

Schema rebuild (B-tree index creation + FTS5 rebuild + centrality computation) accounts for 14-35% of total time.

### Query latency

All MCP tool queries return in under 500 ms on projects up to 65K files. Most return in under 50 ms:

| Operation | Small (< 3K files) | Large (13K-65K files) |
| --------- | ------------------ | --------------------- |
| search:symbols | 13-17 ms | 25-418 ms |
| inspect:symbol | 12-19 ms | 13-14 ms |
| inspect:outline | 15-49 ms | 7-24 ms |
| inspect:bundle | 17-49 ms | 8-14 ms |
| search:text | 25-118 ms | 36-103 ms |
| inspect:cycles | 3-14 ms | 35-653 ms |
| inspect:blast | 3-55 ms | 2-3 ms |
| find:dead | 3-5 ms | 6-21 ms |

### Memory

Peak RSS during indexing scales with symbol count. For typical projects (< 100K symbols), peak RSS stays under 130 MB. The Linux kernel (7.4M symbols) peaks at ~4.0 GB.

---

## Search

### Symbol Search

`search:symbols` uses a two-phase strategy:

1. **FTS5 match** on the `symbols_fts` virtual table (primary path)
2. **Full table scan** as fallback if FTS5 fails or returns no valid terms

Results are scored in-memory by `tt_score_symbol()` in `symbol_scorer.c` using name similarity, signature, summary, keywords, and docstring relevance. A dynamic minimum score threshold scales with query word count. The `--debug` flag (or `debug` MCP parameter) exposes per-field score breakdowns for search tuning.

### Text Search

`search:text` tries ripgrep first (if available), falls back to a manual line-by-line search of indexed file content.

---

## Staleness Detection

Two-tier approach:

1. **Git HEAD comparison**: if the current `git rev-parse HEAD` differs from the stored `git_head`, the index is stale.
2. **Mtime sampling**: samples modification times of a subset of indexed files. If any are newer than the index timestamp, the index is stale.

Staleness results are cached for 5 seconds in the MCP server to avoid repeated filesystem checks.

---

## Diagnostic Mode

The `--diagnostic` / `-X` flag (`diagnostic.c` / `diagnostic.h`) enables structured JSONL output on stderr during indexing. Each line is a self-contained JSON object with a monotonic timestamp, phase identifier, event name, and payload fields.

### Design

- **Thread-safe**: each event is a single `fprintf(stderr)` call, atomic under POSIX
- **Zero overhead when disabled**: the `tt_diag_enabled()` check is a simple boolean read, optimized by branch prediction
- **Convenience macros**: `TT_DIAG(phase, event, fmt, ...)` and `TT_DIAG_MEM()` compile to no-ops when diagnostic is disabled

### Event Format

```json
{"ts":2.145,"ph":"worker","ev":"done","wid":3,"files":4096,"symbols":312847,"elapsed_ms":21453}
```

- `ts`: seconds since `tt_diag_init()` (monotonic clock)
- `ph`: pipeline phase (`init`, `discovery`, `pipeline`, `worker`, `writer`, `summary`, `schema`, `mem`, `done`)
- `ev`: event name within the phase

### Phases and Events

| Phase | Events | Emitted by |
| ----- | ------ | ---------- |
| `init` | `sysinfo` | `cmd_index.c` -- CPU count, worker count, ctags path |
| `discovery` | `start`, `done` | `cmd_index.c` -- file discovery timing, file/size counts |
| `pipeline` | `start`, `balance`, `done` | `cmd_index.c`, `index_pipeline.c` -- worker allocation, LPT load balancing byte distribution, pipeline totals |
| `worker` | `ctags_start`, `progress`, `slow_file`, `backpressure`, `done` | `index_pipeline.c` -- per-worker ctags execution, periodic progress, files taking >500ms, queue backpressure, completion stats |
| `writer` | `commit`, `starvation`, `done` | `index_pipeline.c` -- SQLite batch commit timing, writer idle periods, total rows written |
| `summary` | `start`, `done` | `cmd_index.c` -- symbol summary generation (two-phase: SQL bulk + docstring processing) |
| `schema` | `start`, `secondary_idx`, `fts_rebuild`, `fts_triggers`, `done` | `cmd_index.c` -- secondary index creation, FTS5 rebuild, trigger reinstallation |
| `mem` | `snapshot` | `diagnostic.c` -- RSS and virtual memory from `/proc/self/statm` (Linux only), tracks peak RSS |
| `done` | `summary` | `cmd_index.c` -- final totals with elapsed time and peak RSS |

### Memory Tracking

`tt_diag_mem_snapshot()` reads `/proc/self/statm` on Linux and emits `vm_kb` and `rss_kb`. Peak RSS is tracked internally and reported in the final `done/summary` event via `tt_diag_peak_rss_kb()`. No-op on non-Linux platforms.

---

## Auto-Update

TokToken includes a built-in update mechanism with zero HTTP library dependencies. All network I/O is delegated to `curl` via `tt_proc_run()`, consistent with the existing pattern for `gh` and `git`.

### Version Check (`update_check.c`)

`tt_update_check()` is the hot-path version check used by `--version`, `--help`, and MCP responses:

1. Reads `~/.cache/.toktoken/UPSTREAM_VERSION` and checks file mtime
2. If the cache is fresh (< 12 hours old): compares cached version against `TT_VERSION` using `tt_semver_compare()`
3. If stale or missing: fetches `https://github.com/mauriziofonte/toktoken/releases/latest/download/VERSION` via `curl -fsSL --max-time 5`, writes to cache atomically (temp file + rename)
4. On any failure (no curl, no network, timeout): returns `{NULL, false}` silently -- never blocks or errors

`tt_update_check_fresh()` bypasses cache age and always fetches. Used only by `--self-update`.

### Self-Update (`cmd_update.c`)

`--self-update` (or `--self-upgrade`) performs a full verified binary replacement:

1. Resolves self path via `tt_self_exe_path()` (Linux: `/proc/self/exe`, macOS: `_NSGetExecutablePath`, Windows: `GetModuleFileNameW`)
2. Checks write permission with `access(W_OK)`. If read-only: shows manual download URL
3. Fetches fresh upstream version. If already up to date: exits cleanly
4. Downloads platform binary (`toktoken-{os}-{arch}`) via `curl -fsSL --max-time 30`
5. Downloads `SHA256SUMS` and verifies the binary hash with `tt_sha256_file()`
6. Sets executable permission (`chmod 0755` on Unix, no-op on Windows)
7. Smoke tests the download by running it with `--version`
8. Atomically replaces the running binary via `tt_rename_file()` (POSIX: `rename()`, Windows: `MoveFileExW` with `.old` fallback)

### Platform Binary Naming

Binary names are determined at compile time via the `TT_ARCH` CMake definition:

- `toktoken-linux-x86_64`, `toktoken-linux-aarch64`, `toktoken-linux-armv7`
- `toktoken-macos-x86_64`, `toktoken-macos-aarch64`
- `toktoken-win-x86_64.exe`

---

## Dependencies

| Dependency | Type | Version | Used by |
| ---------- | ---- | ------- | ------- |
| SQLite | Vendored | 3.48.0 (with FTS5) | `database.c`, `index_store.c` |
| cJSON | Vendored | 1.7.19 | `config.c`, `json_output.c`, `mcp_server.c` |
| yyjson | Vendored | 0.12.0 | `index_pipeline.c` (ctags JSON parsing) |
| xxHash | Vendored | 0.8.3 | `fast_hash.c` (content hashing, path hashing) |
| mpsc-queue | Vendored | commit ea09e7b | `index_pipeline.c` (worker-to-writer queue) |
| wildmatch | Vendored | v0.9+36 | `platform.c` (gitignore glob matching) |
| SHA-256 | Vendored | -- | `cmd_update.c` (binary verification only) |
| universal-ctags | External | Any recent version | `ctags_resolver.c`, `ctags_stream.c` |

All vendored code is compiled statically into the binary. No shared libraries required at runtime.
