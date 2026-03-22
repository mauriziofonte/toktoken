# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.3.0] - 2026-03-22

### Added

#### Markdown Symbol Extraction (Issue #1)

- **47 languages**: Markdown files (`.md`, `.markdown`, `.mdx`) are now indexed as a first-class language. Headings are extracted as documentation-specific symbol kinds:
  - `chapter` — H1 headings
  - `section` — H2 headings
  - `subsection` — H3-H6 headings (collapsed to a single canonical kind)
- **3 new canonical kinds** added to the kind system: `TT_KIND_CHAPTER`, `TT_KIND_SECTION`, `TT_KIND_SUBSECTION` (`TT_KIND_COUNT` 12 → 15).
- **6 new CTAGS_MAP entries** (3 identity + 3 aliases): `subsubsection`, `l4subsection`, `l5subsection` all collapse to `subsection`.
- **MDX support**: `.mdx` files are mapped to the Markdown ctags parser via `--langmap=Markdown:+.mdx`.
- **Not excluded by smart filter**: unlike CSS/HTML/YAML, Markdown files are always indexed regardless of the smart filter setting. Headings produce high-quality navigable symbols that do not pollute code search results.

#### Documentation Pipeline

- **MCP tool schemas**: `search_symbols`, `inspect_outline`, and `find_dead` kind parameter descriptions now list all 15 canonical kinds with Code/Documentation separation.
- **MCP tool descriptions**: `index_create`, `search_symbols`, and `inspect_outline` descriptions updated to mention headings and documentation kinds.
- **CLI help**: `--kind` flag example updated to include `chapter`.
- **Error messages**: `cmd_search.c` and `cmd_inspect.c` "Valid kinds" strings include `chapter`, `section`, `subsection`.
- **docs/LANGUAGES.md**: Markdown added to ctags-based languages table (46 → 47), smart filter exclusions note clarifies Markdown is never excluded.
- **docs/LLM.md**: new "Documentation Navigation" workflow pattern, `--kind` flag table updated, `index_create` MCP tool description updated, troubleshooting notes clarify Markdown is always indexed.
- **docs/rules-template.md**: smart filter section clarifies Markdown is always indexed with documentation kinds.
- **docs/ARCHITECTURE.md**, **docs/SECURITY.md**, **docs/PERFORMANCE.md**, **docs/CONFIGURATION.md**: smart filter sections updated to note Markdown exception.
- **README.md**: language count 46 → 47, `--kind` option lists all 15 kinds, `--full` flag description notes Markdown exception.

#### Test Suite

- **Fixture**: `tests/fixtures/full-project/src/markdown/guide.md` — 7 headings (1 chapter, 3 sections, 2 subsections, 1 subsubsection→subsection).
- **Unit tests** (`test_symbol_kind.c`): 5 new tests — ctags doc kind mapping, labels, multiline, validity, count. Updated `test_symbol_kind_to_str_stable_values` with 3 new expected values.
- **Unit tests** (`test_language_detector.c`): 2 new tests — extension and path detection for `.md`/`.markdown`/`.mdx`.
- **Integration tests** (`test_int_file_filter.c`): 3 new tests — Markdown discovered without smart filter, Markdown included with smart filter (not excluded), MDX included with smart filter.
- **E2E tests** (`test_e2e_full_langs.c`): 2 new tests — `search:symbols` finds "Installation" as `section` kind, `inspect:outline` produces at least 3 symbols from `guide.md`.

#### Documentation Audit

- **docs/LLM.md**: fixed MCP tool name `inspect_blast` → `inspect_blast_radius` in the tools table. Added missing `help` tool to both MCP tools table and CLI "Other commands" table.
- **docs/LLM.md**: enhanced Step 3 MCP setup section — added config file paths column to agent table, clarified stdio transport, added tool count ("26 tools via JSON-RPC 2.0"). Added new section 3.3 "Auto-approve tools (recommended)" with guidance for safe auto-approval of read-only TokToken tools.
- **docs/ARCHITECTURE.md**: removed "Gemfile" from vendor manifests list (not implemented). Fixed MD040 linter warning by adding `text` language specifier to data flow diagram fenced code block. Restructured CLI Mode from wall-of-text paragraph to module→commands table for readability.
- **docs/CONFIGURATION.md**: removed "Gemfile" from vendor manifests list. Added documentation for three undocumented features: `.toktokenignore` file support, monorepo workspace-aware pruning (npm workspaces, Cargo `[workspace] members`, Go `go.work use` directives), and source tree directory protection (`src/`, `lib/`, `packages/`, `apps/`, `internal/`, `modules/`, `crates/`). Fixed `extra_extensions` example: replaced `"mdx": "markdown"` (now built-in) with `"tsx": "typescript"`.
- **docs/setup/windsurf.md**: replaced incomplete `alwaysAllow` array (20 tools) with complete list of all 26 tools.
- **docs/setup/claude-code.md**: added `mcp__toktoken__help` to permissions allow list. Updated scope options table with current Claude Code 2026 terminology (`~/.claude.json` paths).
- **README.md**: cleaned up Quick Start PATH note from multi-sentence inline text to concise callout.

## [0.2.1] - 2026-03-21

### Fixed

- **Preserve `full_index` flag across `index:update` cycles** (PR #3, @JeffreyVdb): when a project was indexed with `--full` (smart filter disabled), a subsequent `index:update` without `--full` would re-enable the smart filter during file discovery, causing files outside the smart filter to be treated as deleted and purged from the database. The `full` flag is now persisted as `full_index` in index metadata and automatically restored on `index:update`.

### Security

- **Windows argv quoting hardening** (`platform_win.c`): reimplemented command-line construction with proper `CommandLineToArgvW`-compatible escaping — backslashes before quotes are doubled, trailing backslashes before closing quote are doubled, embedded quotes escaped as `\"`. Prevents argument injection via crafted filenames or repository names on Windows.
- **PATH resolution hardening** (`github.c`, `text_search.c`): subprocess argv[0] now uses fully-resolved absolute paths via `tt_which()` with per-session caching, preventing TOCTOU PATH hijacking. Added `tt_gh_reset_path_cache()` for test environments that manipulate PATH.
- **ReDoS quantifier detection** (`text_search.c`): extended `tt_regex_validate()` to detect `{n,m}` repetition syntax inside groups, in addition to `+` and `*`, closing a gap in nested-quantifier ReDoS protection.
- **Native filesystem operations** (`github.c`): replaced Unix-only `rm -rf` and `ls -1` subprocess calls with cross-platform `tt_remove_dir_recursive()` and `tt_walk_dir()` APIs, eliminating shell command dependency and potential command injection surface on all platforms.

## [0.2.0] - 2026-03-20

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

### Fixed

- **ARM 32-bit portability**: guarded `n >> 32` in `hashmap.c` with `#if SIZE_MAX > 0xFFFFFFFFUL` to avoid shift-count-overflow warning on 32-bit targets.
- **Empty translation unit warning**: added `#else` typedef in `platform.c` to suppress `-Wpedantic` warning on Windows/MinGW builds.
- **Unused variable warnings**: removed dead variables `line_start` (`index_pipeline.c`) and `resolved_count` (`index_store.c`).
- **Unchecked return values**: added error handling for `write()` in `index_pipeline.c` and `test_int_ctags_stream.c`, and for `symlink()` in `test_int_file_filter.c`.
- **E2E test binary discovery**: added `TOKTOKEN_BIN` environment variable override in test helpers for CI environments with non-standard build paths.
- **Unused static function warnings**: added `__attribute__((unused))` to helper functions in `test_e2e_full_helpers.h`.

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

[0.3.0]: https://github.com/mauriziofonte/toktoken/compare/v0.2.1...v0.3.0
[0.2.1]: https://github.com/mauriziofonte/toktoken/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/mauriziofonte/toktoken/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/mauriziofonte/toktoken/releases/tag/v0.1.0
