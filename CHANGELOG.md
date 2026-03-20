# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.2.0] - Unreleased

### Added

#### New Commands (CLI + MCP)

- **`inspect:cycles`** — Detect circular import cycles in the codebase. Supports `--cross-dir` (cross-directory only) and `--min-length N` filters.
- **`inspect:blast`** — Blast radius analysis for a symbol: traces transitive dependents through the import graph to estimate change impact.
- **`find:dead`** — Find unreferenced ("dead") symbols and files that are never imported. Supports `--exclude-tests` to filter test files.
- **`index:file`** — Reindex a single file incrementally without rebuilding the full index. Keeps FTS triggers active to avoid O(total_symbols) rebuild.
- **`help [command]`** — Introspection command that lists all tools or shows detailed usage for a specific tool, derived from MCP schemas at runtime.

#### New MCP Tools

- `inspect_cycles`, `inspect_blast_radius`, `find_dead`, `index_file`, `help` — MCP tool schemas and executors for all new commands (total: 26 tools, up from 21).

#### Search Enhancements

- **Detail levels** (`--detail compact|standard|full`) for `search:symbols` — control result verbosity.
- **Token budget** (`--token-budget N`) for `search:symbols` — results are included until the byte budget is exhausted (byte_length / 4). At least 1 result is always returned.
- **Scope filters** (`--scope-imports-of`, `--scope-importers-of`) for `search:symbols` — restrict results to files in the forward or reverse import graph of a given file.
- **Centrality ranking** — search results are boosted by import-graph centrality score (`TT_WEIGHT_CENTRALITY = 0.3`).

#### Bundle Enhancements

- **Multi-symbol bundles** — `inspect:bundle` accepts comma-separated IDs (max 20) for batch retrieval.
- **`--include-callers`** — Enrich bundle output with callers (symbols in other files that reference this symbol).
- **Markdown output** (`--output-format markdown` / MCP `output_format: "markdown"`) — render bundle as structured markdown instead of JSON.

#### Import Graph Enhancements

- **Import resolution** (`resolved_file` column) — imports are resolved to actual file paths during indexing, enabling accurate dependency tracing.
- **`--has-importers`** flag for `find:importers` — enriches each result with a boolean indicating whether the importing file is itself imported.
- **File centrality computation** (`file_centrality` table) — PageRank-style scoring based on the import graph, computed during indexing.
- **Dynamic import extraction** for JS/TS/Vue — `import('...')` and `await import('...')` are now extracted alongside static imports.
- **Vue import support** — `.vue` files are dispatched to the JS extractor for import extraction.

#### Import Extractor Bug Fixes (Language Dispatch)

- **C++/Obj-C**: Added `"cpp"`, `"objc"`, `"objcpp"` aliases to the C extractor dispatch (previously only matched `"c++"`, `"objectivec"` — ctags-era names).
- **C#**: Added `"csharp"` alias to the C# extractor dispatch (previously only matched `"c#"`).

#### JS/TS Const Extraction

- New pipeline phase extracts `const NAME = <non-function-value>` as `constant` symbols in JS/TS files. Skips destructuring, arrow functions, function expressions, and names already extracted by ctags.

#### Schema v4

- New `file_centrality` table for import-graph-based centrality scores.
- New `resolved_file` column on `imports` table with B-tree index (`idx_imports_resolved`).
- New `savings_per_tool` table for per-tool token savings breakdown.
- Migration path: v3 → v4 via `ALTER TABLE` + `CREATE TABLE` + `CREATE INDEX`.

#### Per-Tool Token Savings

- `savings_per_tool` table tracks call count, raw bytes, response bytes, and tokens saved per MCP tool.
- `stats` command includes per-tool savings breakdown in output.
- `tt_savings_reset()` clears both `savings_totals` and `savings_per_tool`.

#### MCP Server Enhancements

- **Next-step suggestions** — contextual `_ttk.next_steps` array injected into MCP responses, with concrete IDs from results so the LLM can act immediately.

#### Arena Allocator Improvements

- Arena-aware column helpers (`col_astrdup`, `col_astrdup_nullable`, `read_symbol_row_arena`) for zero-copy symbol reads from SQLite.
- `SIZE_MAX` wraparound guard in `tt_arena_alloc()`.
- `strnlen()` in `tt_arena_strndup()` for safety with non-null-terminated buffers.

#### Build System

- Version bump: 0.1.0 → 0.2.0.
- Added `-lm` to link flags (required for `log()` in centrality computation).
- New source files: `src/cmd_help.c`, `src/cmd_help.h`.
- New integration test suites: `test_int_v020.c`, `test_int_resolve.c`, `test_int_multilang.c`.
- New E2E test suite with full-project fixture: 7 test files, 52 fixture files across 27 languages.
- `.gitignore`: added `diagnostic/` directory.

#### Documentation

- `PERFORMANCE.md`: complete rewrite with fresh benchmark data across 8 open-source projects (Redis, curl, Laravel, Django, Neovim, Kubernetes, dotnet/runtime, Linux kernel).
- `ARCHITECTURE.md`: updated database sizing, indexing timing, query latency, and memory sections with v0.2.0 benchmark data.
- `TOKEN_SAVINGS.md`: updated symbol counts and minor corrections.
- `LLM.md`: restructured with prerequisites, troubleshooting, and per-IDE setup links.
- `docs/rules-template.md`: extracted reusable rules template for LLM integration.
- `docs/setup/claude-code.md`, `docs/setup/windsurf.md`: updated setup instructions.
- `README.md`: updated for v0.2.0 feature set and tool count.

#### Test Suite

- **Integration tests**: `test_int_v020.c` (v0.2.0 feature tests), `test_int_resolve.c` (import resolution), `test_int_multilang.c` (multi-language indexing).
- **E2E full-project suite**: 7 new test files (`test_e2e_full_index.c`, `test_e2e_full_search.c`, `test_e2e_full_inspect.c`, `test_e2e_full_find.c`, `test_e2e_full_manage.c`, `test_e2e_full_langs.c`, `test_e2e_full_imports.c`) with shared helpers (`test_e2e_full_helpers.h`).
- **Fixture**: `tests/fixtures/full-project/` — 52 files across 27 languages with import chains, cycle pair, orphan file, and manifest.
- **Import extractor tests**: Vue import extraction, dynamic import extraction (`import()`), `reimport()` negative test.
- **Token savings tests**: per-tool breakdown, empty state, reset behavior.
- **Updated assertions**: schema version 3 → 4, MCP tool count 21 → 26, compact mode field expectations.

### Changed

- **Compact mode** (`search:symbols --compact`) now includes `name`, `kind`, `file`, `line`, `byte_length` fields (previously only `id` and `l`). Omits `qname`, `sig`, `summary`.
- **Vendor manifest detection** (`file_filter.c`) rewritten from N `stat()` calls per directory to a single `opendir()`/`readdir()` scan — reduces syscalls during file discovery.
- **Incremental pipeline** (`index_pipeline.c`) — when `incremental` flag is set (single-file reindex), FTS triggers and secondary indexes are kept active instead of being dropped and rebuilt.
- **License**: MIT → AGPL-3.0 with commercial exception (committed in v0.1.0).

## [0.1.0] - 2025-03-15

### Added

- Initial release.
- CLI with 21 commands: `index:create`, `index:update`, `index:github`, `search:symbols`, `search:text`, `search:cooccurrence`, `search:similar`, `inspect:outline`, `inspect:symbol`, `inspect:file`, `inspect:tree`, `inspect:bundle`, `inspect:dependencies`, `inspect:hierarchy`, `find:importers`, `find:references`, `find:callers`, `stats`, `projects:list`, `cache:clear`, `codebase:detect`.
- MCP JSON-RPC 2.0 server (`toktoken serve`) with 21 tool schemas.
- SQLite-backed index with FTS5 full-text search (schema v3).
- 16-thread parallel ctags pipeline with MPSC queue.
- Smart filter: excludes non-code files and vendored subdirectories by default.
- 14 custom parsers: Vue, Gleam, GDScript, Julia, HCL/Terraform, Nix, GraphQL, and more.
- Import extraction for 15 language families.
- Token savings tracking with cumulative metrics.
- Staleness detection via git HEAD comparison and mtime sampling.
- Self-update mechanism with SHA-256 verification.
- Diagnostic mode (`-X`) with structured JSONL output.
- Cross-platform support: Linux (x86_64, aarch64, armv7), macOS (x86_64, aarch64), Windows (x86_64).

[0.2.0]: https://github.com/mauriziofonte/toktoken/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/mauriziofonte/toktoken/releases/tag/v0.1.0
