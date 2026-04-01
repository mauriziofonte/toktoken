# TokToken (beta release)

Right now, your AI coding agent **reads entire files just to find one function**. That wastes tokens, money, and context window. **TokToken fixes this**.

Real numbers from [Redis](https://github.com/redis/redis) (727 files, 45K symbols, indexed in 0.9s):

| What your agent needs | Without TokToken | With TokToken | Savings |
| --------------------- | ---------------- | ------------- | ------- |
| `initServer()` in `server.c` (8141 lines) | 84,193 tokens | 2,699 tokens | **97%** |
| `sdslen()` in `sds.h` (340 lines) | 2,678 tokens | 132 tokens | **95%** |
| `processCommand()` in `server.c` | 84,193 tokens | 4,412 tokens | **95%** |
| `redisCommandProc` typedef in `server.h` (4503 lines) | 56,754 tokens | 50 tokens | **99%** |

**TokToken** scales well. Check it on the [Linux kernel](https://github.com/torvalds/linux) (65K files, 7.4M symbols) indexes in ~170 seconds:

| What your agent needs | Without TokToken | With TokToken | Savings |
| --------------------- | ---------------- | ------------- | ------- |
| `__schedule()` in `kernel/sched/core.c` (10961 lines) | 73,084 tokens | 1,335 tokens | **98%** |
| `do_sys_open()` in `fs/open.c` (1583 lines) | 9,915 tokens | 89 tokens | **99%** |
| `kmalloc()` in `include/linux/slab.h` (1280 lines) | 11,272 tokens | 1,372 tokens | **88%** |
| `ext4_fill_super()` in `fs/ext4/super.c` (7573 lines) | 53,651 tokens | 376 tokens | **99%** |

One command indexes your codebase. Your agent searches symbols, traces imports, and retrieves only the code it needs -- **88-99% fewer tokens** on every operation.

```bash
toktoken index:create            # index your project (once)
toktoken search:symbols "auth"   # find symbols by name
toktoken inspect:symbol <id>     # retrieve just the code that matters
```

Works with Claude Code, Cursor, Windsurf, Copilot, Gemini, Codex, **and any MCP-compatible agent**. Single binary, zero dependencies, nothing written inside your project.

> **Quick setup:** tell your AI agent to read [docs/LLM.md](docs/LLM.md) -- it will install and configure TokToken (almost) autonomously. For the rest of you, humans, keep reading.

## Features

- **49 languages** via [universal-ctags](https://ctags.io/) + 16 custom parsers (see [docs/LANGUAGES.md](docs/LANGUAGES.md))
- **Import graph**: cross-file dependency tracking with `find:importers`, `find:references`, `find:callers`, and `inspect:dependencies`
- **FTS5 search** with relevance scoring, cascading query strategies, and token-budget-aware result slicing
- **Incremental indexing** using content hashing -- re-indexes only changed files, including single-file reindex
- **Dead code detection**: find symbols with no callers or importers across the codebase
- **Blast radius analysis**: trace all files and symbols affected by a change to a given file or symbol
- **Circular import detection**: identify import cycles in the dependency graph
- **Multi-symbol bundles**: retrieve context bundles for multiple symbols in a single call with markdown output option
- **Scope-filtered search**: restrict symbol and text search to a subtree or file set
- **Centrality-based ranking**: symbols ranked by import-graph centrality in addition to FTS relevance
- **MCP server** (`toktoken serve`) for native IDE integration with tiered tool loading
- **GitHub repo indexing** (`toktoken index:github owner/repo`) -- index any public repo without cloning
- **Smart filtering**: excludes non-code files (CSS, HTML) and vendored directories by default, with selective `--include` override
- **Security**: symlink escape detection, secret pattern filtering, binary exclusion
- **Token savings tracking**: cumulative metrics via the `stats` command
- **Auto-update**: `--self-update` with SHA-256 verification and atomic binary replacement
- **Cross-platform**: Linux (x64/ARM64/ARMv7), macOS (Intel/Apple Silicon), Windows (x64)
- **Single static binary**: no runtime requirements beyond `universal-ctags` (see below)
- **No project pollution**: all data stored under `~/.cache/toktoken/`, nothing written inside your project

## Prerequisites

TokToken requires [universal-ctags](https://ctags.io/) (NOT exuberant-ctags) for symbol extraction. Static linking of ctags is planned but not yet implemented.

| Platform | Install command |
| -------- | --------------- |
| Ubuntu/Debian | `sudo apt-get install -y universal-ctags` |
| Fedora/RHEL | `sudo dnf install universal-ctags` |
| Arch | `sudo pacman -S ctags` |
| macOS | `brew install universal-ctags` |
| Windows | `choco install universal-ctags` |

Verify installation: `ctags --version` must show **Universal Ctags**, not Exuberant Ctags.

## Quick Start

> **PATH setup:** Ensure `~/.local/bin` is in your PATH. If not, add `export PATH="$HOME/.local/bin:$PATH"` to your `~/.bashrc` or `~/.zshrc`.

```bash
# Install (Linux x86_64 example)
mkdir -p ~/.local/bin
curl -fsSL https://github.com/mauriziofonte/toktoken/releases/latest/download/toktoken-linux-x86_64 \
  -o ~/.local/bin/toktoken && chmod +x ~/.local/bin/toktoken

# Index a project
cd /path/to/your/project
toktoken index:create

# Search for symbols (-k filters by kind, -c enables compact output)
# Note: by default, TokToken excludes non-code files (CSS, HTML, SVG) and
# vendored subdirectories. Use -f / --full to include everything.
# Markdown files are always indexed (documentation kinds: chapter, section, subsection).
toktoken search:symbols "auth" -ck class,method,function

# Full-text search grouped by file
toktoken search:text "TODO" -g file

# Inspect a specific symbol
toktoken inspect:symbol "src/Auth.php::Auth.login#method"

# File outline (cheaper than reading the whole file)
toktoken inspect:outline src/Auth.php

# Update index after edits
toktoken index:update
```

## Commands

| Command | Description |
| ------- | ----------- |
| `index:create [path]` | Create index for a project |
| `index:update [path]` | Incremental re-index (hash-based) |
| `index:file <file>` | Reindex a single file |
| `index:github <repo>` | Clone and index a GitHub repository |
| `search:symbols <query>` | Search symbols by name (FTS5 + scoring + centrality ranking) |
| `search:text <query>` | Full-text search across files (ripgrep + fallback) |
| `search:cooccurrence <a>,<b>` | Find symbols that co-occur in the same file |
| `search:similar <id>` | Find symbols similar to a given one |
| `inspect:outline <file>` | Show file symbol hierarchy |
| `inspect:symbol <id>` | Retrieve symbol source code |
| `inspect:file <file>` | Show file content (supports `--lines START-END`) |
| `inspect:bundle <id>[,id2,...]` | Get symbol context bundle (definition + imports + outline); comma-separated IDs for multi-symbol; `--format markdown` for markdown output |
| `inspect:tree` | Show indexed file tree |
| `inspect:dependencies <file>` | Trace transitive import graph (recursive) |
| `inspect:hierarchy <file>` | Show class/function hierarchy with parent-child relationships |
| `find:importers <file>` | Find files that import a given file |
| `find:references <id>` | Find import references to an identifier. Use `--check` for boolean reference check |
| `find:callers <id>` | Find symbols that likely call a given function/method |
| `find:dead` | Find symbols with no callers or importers (dead code detection) |
| `inspect:blast <id>` | Symbol blast radius analysis (files transitively affected by a change) |
| `inspect:cycles` | Detect circular import chains in the dependency graph |
| `help [command]` | List all tools or show detailed usage for a specific tool |
| `suggest` | Onboarding discovery: top keywords, kind/language distribution, most-imported files, example queries |
| `stats` | Index statistics + token savings report |
| `cache:clear` | Delete current project index. With `--all --force`: delete all TokToken data |
| `codebase:detect [path]` | Detect if directory is a codebase |
| `repos:list` | List cloned GitHub repositories |
| `repos:remove <repo>` | Remove a cloned repository |
| `repos:clear` | Remove all cloned repositories |
| `serve` | Start MCP server on STDIO |
| `--self-update` | Update to latest release (SHA-256 verified) |

### Options

All options have a long form (`--option`). Most also have a single-letter short form (`-x`).
Short boolean flags can be combined: `-cn` is equivalent to `--compact --count`.
Value flags accept attached (`-l10`) or separate (`-l 10`) values.

#### Indexing options

Used with `index:create`, `index:update`, and `index:github`.

| Short | Long | Argument | Description |
| ----- | ---- | -------- | ----------- |
| `-m` | `--max-files` | `<n>` | Maximum number of files to index. Files beyond this limit are silently skipped. Default: 200000. |
| `-f` | `--full` | | Disable the smart filter. By default, TokToken excludes non-code files (CSS, HTML, SVG, YAML, XML, TOML, GraphQL) and vendored subdirectories to reduce noise. Markdown files (`.md`, `.markdown`, `.mdx`) are always indexed regardless of the smart filter, producing documentation-specific kinds (`chapter`, `section`, `subsection`). With `--full`, everything is indexed. |
| `-I` | `--include` | `<dir>` | Force-include a normally-skipped directory (e.g. `vendor`). Repeatable: `-I vendor -I node_modules`. VCS dirs (`.git`, `.svn`, `.hg`) cannot be included. The override persists across `index:update` cycles. Unlike `--full`, this only un-skips the named directory — the smart filter still applies to file extensions inside it. |
| `-i` | `--ignore` | `<pattern>` | Add an extra ignore pattern. Files/directories matching this glob are skipped during discovery. Repeatable: `-i vendor -i dist -i .cache`. |
| | `--languages` | `<list>` | Comma-separated list of languages to index. Only files detected as one of these languages are processed. Example: `--languages c,python,rust`. |
| `-X` | `--diagnostic` | | Enable structured diagnostic output. Emits JSONL events to stderr with per-phase timing, worker progress, memory snapshots, and pipeline metrics. See [Diagnostic Mode](#diagnostic-mode). |

#### Search options

Used with `search:symbols` and `search:text`.

| Short | Long | Argument | Description |
| ----- | ---- | -------- | ----------- |
| `-k` | `--kind` | `<types>` | Filter results by symbol kind. Comma-separated. Code: `function`, `class`, `method`, `variable`, `constant`, `type`, `enum`, `interface`, `trait`, `property`, `namespace`, `directive`. Documentation: `chapter`, `section`, `subsection`. Example: `-k function,method` returns only functions and methods. |
| `-L` | `--language` | `<lang>` | Filter results to a specific language. Example: `-L python` returns only Python symbols. |
| `-n` | `--count` | | Return only the match count, not the results themselves. Useful for checking whether a term exists in the codebase without fetching data. Output: `{"q":"term","count":42}`. |
| `-g` | `--group-by` | `file` | Group `search:text` results by file path instead of listing individual lines. Returns per-file hit counts. |
| `-C` | `--context` | `<n>` | Include `n` lines of surrounding context before and after each match. Only applies to `search:text`. |
| `-r` | `--regex` | | Treat the search query as a regular expression instead of a plain text/FTS query. |
| `-s` | `--case-sensitive` | | Force case-sensitive matching. By default, searches are case-insensitive. |
| `-D` | `--debug` | | Include per-field scoring breakdown in `search:symbols` results. Shows how name, signature, summary, and qualified_name contribute to the final score. |
| | `--no-sig` | | Omit symbol signatures from results. Reduces output size for discovery queries where only names and locations matter. |
| | `--no-summary` | | Omit symbol summaries from results. Combine with `--no-sig` for minimal output. |

#### Inspect options

Used with `inspect:file`, `inspect:tree`, and `inspect:bundle`.

| Short | Long | Argument | Description |
| ----- | ---- | -------- | ----------- |
| | `--lines` | `<start-end>` | Return only a specific line range from `inspect:file`. Example: `--lines 10-50` returns lines 10 through 50. |
| `-d` | `--depth` | `<n>` | Limit directory depth for `inspect:tree`. Example: `-d 2` shows only top-level and one level of subdirectories. |

#### Global options

Available for all commands.

| Short | Long | Argument | Description |
| ----- | ---- | -------- | ----------- |
| `-p` | `--path` | `<dir>` | Set the project directory. Defaults to the current working directory. |
| `-o` | `--format` | `<fmt>` | Output format: `json` (default) or `table` (human-readable columnar output). |
| | `--filter` | `<pattern>` | Include only files whose path matches this pattern. Pipe-separated for OR: `--filter "src\|lib"`. Case-insensitive. |
| `-e` | `--exclude` | `<pattern>` | Exclude files whose path matches this pattern. Pipe-separated for OR: `-e "vendor\|node_modules"`. Case-insensitive. |
| `-l` | `--limit` | `<n>` | Cap the number of results returned. Applies to all search and list commands. |
| `-c` | `--compact` | | Compact JSON output. Removes whitespace and shortens field names, reducing response size by ~47%. Recommended for AI agent usage. |
| `-u` | `--unique` | | Deduplicate results by symbol name. |
| `-S` | `--sort` | `<field>` | Sort results by the given field. Default: `score` (relevance). Other options depend on the command. |
| `-t` | `--truncate` | `<n>` | Truncate output lines to `n` characters. Default: 120. Minimum: 20. |
| `-v` | `--version` | | Print version and exit. |
| `-h` | `--help` | | Print help and exit. |
| | `--self-update` | | Download and install the latest release. Verifies SHA-256 checksum and performs atomic binary replacement. |

#### Examples

```bash
# Search symbols, compact + count only
toktoken search:symbols "auth" -cn

# Search functions in Python, limit 20, compact
toktoken search:symbols "parse" -ck function -L python -l20

# Full-text search grouped by file, 3 lines of context
toktoken search:text "TODO" -g file -C3

# Index with max 10k files, ignore vendor and dist
toktoken index:create -m10000 -i vendor -i dist

# Index with vendor/ included (e.g. Symfony/Laravel projects)
toktoken index:create --include vendor

# Index everything (disable smart filter)
toktoken index:create -f

# Tree with depth 2, compact
toktoken inspect:tree -cd2

# All boolean flags combined
toktoken search:symbols "init" -cnrs
# equivalent to: --compact --count --regex --case-sensitive
```

## Diagnostic Mode

The `--diagnostic` / `-X` flag enables structured JSONL output on stderr during indexing. Each line is a self-contained JSON object with a timestamp, phase, event type, and payload. Designed for performance analysis and debugging.

```bash
# Index with diagnostics, capture to file
toktoken index:create -X 2>/tmp/diagnostics.jsonl

# Pretty-print events
jq . /tmp/diagnostics.jsonl
```

Events are organized by phase:

| Phase | Events | Description |
| ----- | ------ | ----------- |
| `init` | `sysinfo` | CPU count, workers, ctags path |
| `discovery` | `start`, `done` | File discovery timing and counts |
| `pipeline` | `start`, `balance`, `done` | Worker allocation, load balancing stats, pipeline totals |
| `worker` | `ctags_start`, `progress`, `slow_file`, `backpressure`, `done` | Per-worker ctags execution, progress, and completion |
| `writer` | `commit`, `starvation`, `done` | SQLite batch commits, writer idle time |
| `summary` | `start`, `done` | Symbol summary generation timing |
| `schema` | `start`, `secondary_idx`, `fts_rebuild`, `fts_triggers`, `done` | Index and FTS rebuild phases |
| `mem` | `snapshot` | RSS and virtual memory (Linux only) |
| `done` | `summary` | Final totals with peak RSS |

Example event:

```json
{"ts":2.145,"ph":"worker","ev":"done","wid":3,"files":4096,"symbols":312847,"elapsed_ms":21453}
```

All timestamps (`ts`) are seconds since pipeline start. Worker events include `wid` (worker ID). Memory snapshots report `vm_kb` and `rss_kb`.

## MCP Server

TokToken runs as a Model Context Protocol server for native integration with AI IDEs:

```bash
toktoken serve
```

### Claude Code

```bash
# User-scoped (available across all projects)
claude mcp add-json --scope user toktoken '{"command":"toktoken","args":["serve"]}'

# Or project-scoped (shared via .mcp.json)
claude mcp add-json --scope project toktoken '{"command":"toktoken","args":["serve"]}'

# Verify
claude mcp list
```

### VS Code / GitHub Copilot

> **VS Code uses a different format.** The top-level key is `"servers"`, not `"mcpServers"`.

Create `.vscode/mcp.json` in your project root:

```json
{
  "servers": {
    "toktoken": {
      "command": "toktoken",
      "args": ["serve"]
    }
  }
}
```

See [docs/setup/copilot-vscode.md](docs/setup/copilot-vscode.md) for user-scoped setup and requirements.

### Other MCP Clients

For Cursor, Windsurf, Gemini CLI, Gemini Code Assist, and Claude Desktop, add the following to the platform's MCP config file:

```json
{
  "mcpServers": {
    "toktoken": {
      "command": "toktoken",
      "args": ["serve"]
    }
  }
}
```

| Platform | Config path | Setup guide |
| -------- | ----------- | ----------- |
| Claude Desktop | `~/Library/Application Support/Claude/claude_desktop_config.json` (macOS) | [setup](docs/setup/claude-desktop.md) |
| Cursor (project) | `.cursor/mcp.json` in project root | [setup](docs/setup/cursor.md) |
| Cursor (global) | `~/.cursor/mcp.json` | [setup](docs/setup/cursor.md) |
| Windsurf | `~/.codeium/windsurf/mcp_config.json` | [setup](docs/setup/windsurf.md) |
| Gemini CLI | `~/.gemini/settings.json` | [setup](docs/setup/gemini-cli.md) |
| Gemini Code Assist | `.gemini/settings.json` in project root | [setup](docs/setup/gemini-code-assist.md) |
| OpenAI Codex CLI | `AGENTS.md` in project root (no MCP) | [setup](docs/setup/codex-cli.md) |

Exposes 27 tools: `codebase_detect`, `index_create`, `index_update`, `index_file`, `index_github`, `search_symbols`, `search_text`, `search_cooccurrence`, `search_similar`, `inspect_outline`, `inspect_symbol`, `inspect_file`, `inspect_tree`, `inspect_bundle`, `inspect_dependencies`, `inspect_hierarchy`, `inspect_cycles`, `inspect_blast_radius`, `find_importers`, `find_references`, `find_callers`, `find_dead`, `suggest`, `stats`, `projects_list`, `cache_clear`, `help`.

## AI Agent Setup

**For LLMs**: read [docs/LLM.md](docs/LLM.md) for complete setup and integration instructions. This file is designed to be consumed directly by AI agents to autonomously configure TokToken for the user's environment.

```
https://raw.githubusercontent.com/mauriziofonte/toktoken/main/docs/LLM.md
```

Supported platforms: Claude Code, Cursor, Windsurf, Gemini CLI, Codex CLI, and any agent that can execute shell commands.

## Building from Source

Requires: CMake >= 3.16, GCC >= 9 or Clang >= 10.

```bash
# Debug (with ASan + UBSan)
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)

# Release
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Static binary (Linux)
cmake -B build -DCMAKE_BUILD_TYPE=Release -DTT_STATIC=ON
cmake --build build -j$(nproc)

# Windows (MSYS2/MinGW64 shell)
cmake -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)

# Run tests
./build/test_unit
./build/test_integration
./build/test_e2e
```

See [CONTRIBUTING.md](CONTRIBUTING.md) for development workflow, coding standards, and test conventions.

## Storage

TokToken stores all data outside the project directory. Nothing is written inside your codebase.

### Cache directory

The cache directory holds indexes, logs, GitHub clones, and the update-check cache.

| OS | Path |
| -- | ---- |
| Linux | `~/.cache/toktoken/` (`$HOME/.cache/toktoken/`) |
| macOS | `~/.cache/toktoken/` (`$HOME/.cache/toktoken/`) |
| Windows | `%USERPROFILE%\.cache\toktoken\` (e.g. `C:\Users\<name>\.cache\toktoken\`) |

Home directory resolution: on Unix, `$HOME`; on Windows, `%USERPROFILE%` with `SHGetFolderPathW(CSIDL_PROFILE)` fallback.

```text
~/.cache/toktoken/
    projects/
        <hash>/                 Project directory (hash = first 12 hex chars of xxhash of canonical path)
            db.sqlite           SQLite database (schema v4, WAL mode)
            db.sqlite-wal       WAL journal (auto-managed by SQLite)
            db.sqlite-shm       Shared memory file (auto-managed by SQLite)
    gh-repos/
        <owner>/<repo>/         Shallow clone of GitHub repo (via index:github)
    logs/
        mcp.jsonl               MCP tool call + lifecycle log (append-only JSONL)
    UPSTREAM_VERSION            Cached latest release version (plain text, 12h refresh via curl)
```

**Database contents** (per project, in `db.sqlite`):

| Table | Purpose |
| ----- | ------- |
| `metadata` | Key-value store: `project_path`, `schema_version`, `toktoken_version`, `indexed_at`, `git_head`, `full_index` |
| `files` | Indexed files with path, content hash, language, summary, size, mtime |
| `symbols` | Extracted symbols: name, qualified name, kind, signature, docstring, line range |
| `symbols_fts` | FTS5 virtual table for full-text symbol search |
| `imports` | Import/dependency edges between files |
| `file_centrality` | PageRank-style centrality scores per file |

### Configuration files

Optional, not created by default:

| OS | Global config | Project config |
| -- | ------------- | -------------- |
| Linux / macOS | `~/.toktoken.json` | `<project>/.toktoken.json` |
| Windows | `%USERPROFILE%\.toktoken.json` | `<project>\.toktoken.json` |

Global config supports all sections (`index`, `logging`). Project config supports only the `index` section. See [CONFIGURATION.md](docs/CONFIGURATION.md) for the full reference.

### Migration from v0.3.x

Prior to v0.4.0, the cache directory was `~/.cache/.toktoken/` (dot-prefixed). On first access, TokToken atomically renames the old directory to `~/.cache/toktoken/`. If another process holds the old path (e.g. a concurrent MCP server), the rename is deferred to the next invocation. No data is lost.

### Cleanup

`cache:clear` deletes the current project's index database. `cache:clear --all --force` removes all TokToken data (indexes, GitHub clones, logs).

## Documentation

| Document | Description |
| -------- | ----------- |
| [docs/LLM.md](docs/LLM.md) | AI agent setup and integration guide |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | System architecture and data flow |
| [docs/ASSESSMENT.md](docs/ASSESSMENT.md) | Tool assessment results and methodology |
| [docs/SECURITY.md](docs/SECURITY.md) | Security model and threat mitigations |
| [docs/LANGUAGES.md](docs/LANGUAGES.md) | Supported languages and parsers |
| [docs/CONFIGURATION.md](docs/CONFIGURATION.md) | Configuration reference |
| [docs/PERFORMANCE.md](docs/PERFORMANCE.md) | Real-world performance benchmarks |
| [docs/TOKEN_SAVINGS.md](docs/TOKEN_SAVINGS.md) | Token savings analysis for AI agents |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Development workflow and standards |

## License

[AGPL-3.0](LICENSE) -- Copyright (c) 2026 Maurizio Fonte

Commercial use without AGPL-3.0 copyleft obligations requires a separate license. Contact: <fonte.maurizio@gmail.com>

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for vendored dependency licenses.
