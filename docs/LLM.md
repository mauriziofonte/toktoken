# TokToken -- AI Agent Setup & Integration Guide

You are an AI coding agent reading this document. Follow the steps below **in order** to install TokToken, configure your agent platform, and start using it. Every step is designed to be executed literally. Do not improvise or skip steps.

TokToken is a single static binary that indexes source code via universal-ctags, stores symbols in SQLite/FTS5, and exposes fast search/inspect commands. It saves 80-95% of context tokens compared to reading raw files.

---

## Step 1: Check if TokToken is already installed

```bash
test -x ~/.local/bin/toktoken && ~/.local/bin/toktoken --version
```

If this prints a version string, skip to **Step 3**. Otherwise continue.

---

## Step 2: Install TokToken and its dependencies

### 2.1 Install universal-ctags (required)

TokToken requires `universal-ctags` (NOT exuberant-ctags) for symbol extraction. Check if it's already installed:

```bash
ctags --version 2>/dev/null | head -1
```

This must show `Universal Ctags`. If it shows `Exuberant Ctags`, nothing, or an error, it needs to be installed.

You're very likely **not** allowed to run `sudo` or package manager commands on behalf of the User. **Do not run the install commands below unless the User has explicitly granted you permission.** Instead, inform the User that they need to install universal-ctags themselves, and provide them with the appropriate command for their platform:

| Platform | Install command |
| -------- | --------------- |
| Ubuntu/Debian | `sudo apt-get install -y universal-ctags` |
| Fedora/RHEL | `sudo dnf install universal-ctags` |
| Arch | `sudo pacman -S ctags` |
| macOS | `brew install universal-ctags` |
| Windows | `choco install universal-ctags` |

**Do not proceed to Step 2.2 until `ctags --version` shows `Universal Ctags`.**

### 2.2 Detect platform and download TokToken

Use the mapping table below. Do NOT rely on `uname` output directly -- the binary names do not match `uname` values on all platforms.

| `uname -s` | `uname -m` | Binary name |
| ----------- | ---------- | ----------- |
| `Linux` | `x86_64` | `toktoken-linux-x86_64` |
| `Linux` | `aarch64` | `toktoken-linux-aarch64` |
| `Linux` | `armv7l` | `toktoken-linux-armv7` |
| `Darwin` | `x86_64` | `toktoken-macos-x86_64` |
| `Darwin` | `arm64` | `toktoken-macos-aarch64` |

Windows binary: `toktoken-win-x86_64.exe` (see Windows section below).

> **Windows users:** The bash scripts in this section use `uname` and are not compatible with PowerShell or cmd.exe. Skip directly to the **Windows (PowerShell)** section below.

Download using the `latest` release URL (no version number needed):

```bash
# Example for Linux x86_64:
mkdir -p ~/.local/bin
curl -fsSL https://github.com/mauriziofonte/toktoken/releases/latest/download/toktoken-linux-x86_64 \
  -o ~/.local/bin/toktoken && chmod +x ~/.local/bin/toktoken
```

Full platform detection script (copy-paste safe, Linux/macOS only):

```bash
case "$(uname -s)" in
    Linux)  TT_OS="linux" ;;
    Darwin) TT_OS="macos" ;;
    *)      echo "Unsupported OS"; exit 1 ;;
esac

case "$(uname -m)" in
    x86_64)         TT_ARCH="x86_64" ;;
    aarch64|arm64)  TT_ARCH="aarch64" ;;
    armv7l|armv7)   TT_ARCH="armv7" ;;
    *)              echo "Unsupported architecture"; exit 1 ;;
esac

TT_BINARY="toktoken-${TT_OS}-${TT_ARCH}"
TT_URL="https://github.com/mauriziofonte/toktoken/releases/latest/download/${TT_BINARY}"

# Install to ~/.local/bin (user-writable, no sudo needed)
mkdir -p ~/.local/bin
curl -fsSL "$TT_URL" -o ~/.local/bin/toktoken && chmod +x ~/.local/bin/toktoken
```

On macOS, remove the quarantine attribute after download:

```bash
xattr -d com.apple.quarantine ~/.local/bin/toktoken 2>/dev/null || true
```

On Windows (PowerShell):

```powershell
New-Item -ItemType Directory -Force -Path "$env:USERPROFILE\.local\bin" | Out-Null
Invoke-WebRequest -Uri "https://github.com/mauriziofonte/toktoken/releases/latest/download/toktoken-win-x86_64.exe" `
  -OutFile "$env:USERPROFILE\.local\bin\toktoken.exe"
```

Then add `%USERPROFILE%\.local\bin` to your PATH via System Settings > Environment Variables, or run:

```powershell
$binPath = "$env:USERPROFILE\.local\bin"
$currentPath = [Environment]::GetEnvironmentVariable("PATH", "User")
if ($currentPath -notlike "*$binPath*") {
    [Environment]::SetEnvironmentVariable("PATH", "$binPath;$currentPath", "User")
}
```

### 2.3 Ensure `~/.local/bin` is in PATH (Linux/macOS)

```bash
case ":$PATH:" in
    *":$HOME/.local/bin:"*) ;; # already in PATH
    *) echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.profile
       export PATH="$HOME/.local/bin:$PATH" ;;
esac
```

### 2.4 Verify installation

```bash
toktoken --version
```

This must print `toktoken X.Y.Z`. If it fails, the binary is not in PATH or the wrong platform binary was downloaded.

---

## Step 3: Configure agent integration

TokToken supports two integration modes:

- **MCP server** (preferred): `toktoken serve` -- the agent calls tools directly via Model Context Protocol (stdio transport)
- **CLI fallback**: the agent runs shell commands like `toktoken search:symbols "query"`

### 3.1 MCP server configuration

TokToken runs as a local stdio MCP server. It exposes 26 tools via JSON-RPC 2.0 on stdin/stdout. Find your agent below and follow the linked setup guide:

| Agent | Config format | Config file | Setup guide |
| ----- | ------------- | ----------- | ----------- |
| **Claude Code** | CLI registration | `~/.claude.json` | [setup/claude-code.md](setup/claude-code.md) |
| **Claude Desktop** | `"mcpServers"` JSON | `~/Library/Application Support/Claude/claude_desktop_config.json` | [setup/claude-desktop.md](setup/claude-desktop.md) |
| **VS Code / GitHub Copilot** | `"servers"` JSON | `.vscode/mcp.json` | [setup/copilot-vscode.md](setup/copilot-vscode.md) |
| **Cursor** | `"mcpServers"` JSON | `.cursor/mcp.json` or `~/.cursor/mcp.json` | [setup/cursor.md](setup/cursor.md) |
| **Windsurf** | `"mcpServers"` JSON | `~/.codeium/windsurf/mcp_config.json` | [setup/windsurf.md](setup/windsurf.md) |
| **Gemini CLI** | `"mcpServers"` JSON | `~/.gemini/settings.json` | [setup/gemini-cli.md](setup/gemini-cli.md) |
| **Gemini Code Assist** | `"mcpServers"` JSON | `.gemini/settings.json` | [setup/gemini-code-assist.md](setup/gemini-code-assist.md) |
| **OpenAI Codex CLI** | Rules file (no MCP) | `AGENTS.md` | [setup/codex-cli.md](setup/codex-cli.md) |

> **Warning:** VS Code / GitHub Copilot uses `"servers"` as the top-level key, **not** `"mcpServers"`. Using the wrong format will silently fail. See [copilot-vscode.md](setup/copilot-vscode.md) for the correct configuration.

For most MCP clients (all except Claude Code, VS Code, and Codex CLI), the JSON configuration follows this pattern:

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

**If the config file already exists, READ it first, MERGE the toktoken entry into the existing object, and WRITE back. Do NOT overwrite the file.**

### 3.2 CLI rules setup (for agents without MCP, or as complement)

Copy the rules template from [docs/rules-template.md](rules-template.md) into your agent's instructions file. Each vendor setup guide above includes the specific file path and format for rules placement.

### 3.3 Auto-approve tools (recommended)

Most MCP clients prompt for permission on each tool call. TokToken tools are read-only queries against a local SQLite index — they are safe to auto-approve. Each setup guide above includes platform-specific auto-approve instructions. At minimum, auto-approve these high-frequency tools: `search_symbols`, `inspect_symbol`, `inspect_outline`, `inspect_bundle`, `search_text`, `stats`.

---

## Step 4: Index the current project

```bash
cd /path/to/project
toktoken codebase:detect
```

If exit code is 0 and the `action` field in the JSON output is `"index:create"`, run:

```bash
toktoken index:create
```

If `action` is `"ready"`, the index already exists.

After editing files, refresh the index before querying:

```bash
toktoken index:update
```

---

## Step 5: Start using TokToken

### Available MCP tools

| Tool | Description |
| ---- | ----------- |
| `codebase_detect` | Detect if a directory contains indexable source code |
| `index_create` | Create a full symbol index. Markdown files are always indexed (documentation kinds: chapter, section, subsection). Pass `full: true` to also include CSS, HTML, YAML, etc. |
| `index_update` | Incrementally update the index. Pass `full: true` to include all file types |
| `index_github` | Clone and index a GitHub repository |
| `search_symbols` | Search symbols by name (returns IDs for inspect_symbol) |
| `search_text` | Grep-like text search across indexed files |
| `inspect_outline` | Show file symbol outline with line numbers and signatures |
| `inspect_symbol` | Retrieve full source code for symbol(s) by ID |
| `inspect_file` | Retrieve file content, optionally limited to a line range |
| `inspect_bundle` | Get self-contained symbol context: definition source, imports, file outline, optionally importers |
| `inspect_tree` | Show directory tree of indexed source files |
| `stats` | Project statistics: file/symbol counts, staleness, token savings |
| `projects_list` | List all indexed projects |
| `cache_clear` | Delete the index for a project |
| `find_importers` | Find all files that import a given file path |
| `find_references` | Find import statements referencing an identifier or module |
| `find_callers` | Find symbols that likely call a given function/method (heuristic) |
| `search_cooccurrence` | Find symbols that co-occur in the same file |
| `search_similar` | Find symbols similar to a given one by name/summary keywords |
| `inspect_dependencies` | Trace transitive import graph recursively |
| `inspect_hierarchy` | Show class/function hierarchy with parent-child relationships |
| `inspect_blast_radius` | Symbol blast radius analysis (BFS on reverse import graph) |
| `inspect_cycles` | Detect circular import cycles (Tarjan's SCC) |
| `find_dead` | Find unreferenced symbols (dead/unreferenced classification, confidence levels) |
| `index_file` | Reindex a single file without rebuilding the full index |
| `help` | Get usage details for a TokToken tool, or list all tools |

### CLI command reference

#### search:symbols

```bash
toktoken search:symbols "auth"
toktoken search:symbols "auth" --kind class,method,function
toktoken search:symbols "auth" --unique --limit 20
toktoken search:symbols "cache" --filter "Frontend|Entity"
toktoken search:symbols "post" --exclude vendor,staging
toktoken search:symbols "ldap" --count
toktoken search:symbols "route" --sort name --kind function --unique
toktoken search:symbols "auth" --no-sig --no-summary
toktoken search:symbols "handler" --language go
toktoken search:symbols "cache" --detail compact
toktoken search:symbols "auth" --token-budget 2000
toktoken search:symbols "validator" --scope-imports-of src/Auth.php
toktoken search:symbols "handler" --scope-importers-of src/Events/Dispatcher.php
```

| Flag | Description |
| ---- | ----------- |
| `--kind <list>` | Comma-separated. Code: class, method, function, property, variable, interface, trait, constant, enum, namespace, type, directive. Documentation: chapter, section, subsection |
| `--unique` | Deduplicate by file:line:name |
| `--sort <field>` | score (default), name, file, line, kind |
| `--count` | Return count only |
| `--no-sig` | Omit signatures |
| `--no-summary` | Omit summaries |
| `--filter <pat>` | Include files matching substring (pipe-separated OR) |
| `--exclude <pat>` | Exclude files matching substring (pipe-separated OR) |
| `--limit <n>` | Cap output results |
| `--max <n>` | Cap index query results (default 50, max 200) |
| `--language <lang>` | Filter by language |
| `--debug` | Show per-field score breakdown (name, signature, summary, keyword, docstring) |
| `--file <glob>` | Filter by file glob |
| `--compact` | Compact JSON output |
| `--detail <level>` | Output verbosity: `compact` (name+file+kind only), `standard` (default), `full` (all fields) |
| `--token-budget <n>` | Stop returning results once cumulative token estimate exceeds N |
| `--scope-imports-of <file>` | Restrict results to symbols imported by the given file |
| `--scope-importers-of <file>` | Restrict results to symbols found in files that import the given file |

#### search:text

```bash
toktoken search:text "TODO"
toktoken search:text "cache|Cache|ttl"
toktoken search:text "author" --group-by file
toktoken search:text "cache" --filter "Frontend|Entity" --group-by file
toktoken search:text "cache" --filter Frontend --count
toktoken search:text "ldap_bind" --context 3
```

| Flag | Description |
| ---- | ----------- |
| `--group-by file` | Hit count per file instead of individual lines |
| `--context <n>` / `-C <n>` | Context lines around each match |
| `--count` | Return count only |
| `--case-sensitive` | Case-sensitive matching |
| `--filter <pat>` | Include files matching substring |
| `--exclude <pat>` | Exclude files matching substring |
| `--limit <n>` | Cap output results |
| `--max <n>` | Cap search results (default 100, max 500) |

#### inspect:outline

```bash
toktoken inspect:outline src/Controller.php
toktoken inspect:outline src/Controller.php --kind class,method
toktoken inspect:outline src/Controller.php --no-sig --no-summary
```

#### inspect:symbol

```bash
toktoken inspect:symbol "src/Auth.php::Auth.login#method"
toktoken inspect:symbol "src/Auth.php::Auth.login#method" --context 5
```

#### inspect:bundle

```bash
toktoken inspect:bundle "src/Auth.php::Auth.login#method"
toktoken inspect:bundle "src/Auth.php::Auth.login#method" --full
toktoken inspect:bundle "src/Auth.php::Auth.login#method" --compact
toktoken inspect:bundle "src/Auth.php::Auth.login#method,src/Auth.php::Auth.logout#method"
toktoken inspect:bundle "src/Auth.php::Auth.login#method" --include-callers
toktoken inspect:bundle "src/Auth.php::Auth.login#method" --format markdown
```

Returns a self-contained context bundle: symbol definition with source code, import lines from the same file, sibling symbols (file outline). Use `--full` to also include importers (files that import this symbol's file). Pass comma-separated IDs to bundle multiple symbols in a single call. Use `--include-callers` to append caller symbols to the bundle. Use `--format markdown` to get the bundle as a Markdown document instead of JSON.

#### inspect:file

```bash
toktoken inspect:file src/Auth.php
toktoken inspect:file src/Auth.php --lines 10-50
```

#### inspect:tree

```bash
toktoken inspect:tree
toktoken inspect:tree --depth 2
```

#### Other commands

| Command | Description |
| ------- | ----------- |
| `stats` | Index statistics + token savings report (includes per-tool savings breakdown) |
| `index:create [path]` | Full index from scratch. Use `--full` to include all file types and vendors |
| `index:update [path]` | Incremental re-index. Use `--full` to include all file types and vendors |
| `index:file <file>` | Reindex a single file without rebuilding the full index |
| `index:github <owner/repo>` | Clone and index a GitHub repository |
| `inspect:bundle <id>` | Symbol context bundle (definition + imports + outline). Use `--full` for importers. Supports comma-separated IDs, `--include-callers`, `--format markdown` |
| `inspect:blast <id>` | Symbol blast radius analysis via BFS on the reverse import graph |
| `inspect:cycles` | Detect circular import cycles using Tarjan's SCC algorithm |
| `find:importers <file>` | Find files that import a given file. Use `--has-importers` to filter to only files that have at least one importer |
| `find:dead` | Find unreferenced symbols with dead/unreferenced classification and confidence levels |
| `find:references <id>` | Find import statements referencing an identifier |
| `find:callers <id>` | Find symbols that likely call a given function/method |
| `search:cooccurrence "<a>,<b>"` | Find symbols that co-occur in the same file |
| `search:similar <id>` | Find symbols similar to a given one |
| `inspect:dependencies <file>` | Trace transitive import graph. Use `--depth N` (default 3, max 10) |
| `inspect:hierarchy <file>` | Show class/function hierarchy with parent-child relationships |
| `codebase:detect [path]` | Detect if directory is a codebase |
| `projects:list` | List all indexed projects |
| `cache:clear` | Delete the index database |
| `help [command]` | List all tools or show detailed usage for a specific tool |

---

## Workflow Patterns

These patterns show how to combine TokToken commands for common tasks. Each pattern is designed to minimize token usage by narrowing the search space before retrieving source code.

### "I need to understand how authentication works in this codebase"

Start broad, then zoom in. Never read entire files.

```text
1. toktoken search:symbols "auth" --kind class,method,function --unique --limit 20
   --> Find all authentication-related symbols. Note the file paths and symbol IDs.

2. toktoken inspect:outline src/Auth/AuthManager.php --kind class,method
   --> See the structure of the key file without reading its source code.

3. toktoken inspect:symbol "src/Auth/AuthManager.php::AuthManager.authenticate#method"
   --> Read ONLY the authenticate() method, not the 800-line file.

4. toktoken inspect:bundle "src/Auth/AuthManager.php::AuthManager.authenticate#method" --full
   --> Get authenticate() + its imports + file outline + who imports this file.
   --> This single call replaces reading 3-4 files.
```

**Key principle:** `inspect:bundle` is the most token-efficient way to understand a symbol in context. Use it before reaching for `inspect:file` or reading raw files.

### "I need to fix a bug in the processOrder() function"

Before touching code, understand the blast radius.

```text
1. toktoken search:symbols "processOrder" --kind function,method
   --> Find the symbol ID.

2. toktoken inspect:symbol "src/Orders/OrderService.php::OrderService.processOrder#method"
   --> Read the function source code.

3. toktoken find:callers "src/Orders/OrderService.php::OrderService.processOrder#method"
   --> Who calls processOrder()? These callers might break if you change the signature.

4. toktoken find:importers "src/Orders/OrderService.php"
   --> Which files import OrderService? These are all potentially affected.

5. toktoken inspect:dependencies "src/Orders/OrderService.php" --depth 2
   --> What does OrderService depend on? Trace the import graph to understand
   --> which services, models, and utilities it uses.
```

### "Does this codebase use caching? Where and how?"

Prove presence or absence of a concept across the entire codebase, without reading a single file.

```text
1. toktoken search:text "cache" --exclude vendor --group-by file
   --> Which files mention "cache"? Grouped by file = one line per file, not per match.

2. toktoken search:symbols "cache" --kind class --count
   --> {"count": 3}
   --> There are exactly 3 cache-related classes. Not 0, not 50.

3. toktoken search:text "cache" --filter "Entity|Model" --count
   --> {"count": 0}
   --> Cache is NOT used in the Entity/Model layer. Skip those 40+ files entirely.

4. toktoken search:symbols "cache" --kind class
   --> Get the symbol IDs for the 3 cache classes.

5. toktoken inspect:bundle "src/Cache/RedisCache.php::RedisCache#class"
   --> Read the main cache class with its full context.
```

**Key principle:** `--count` is critical for negative signals. It proves a concept does NOT exist in a file set, saving hundreds of lines of unnecessary reading.

### "I need to refactor UserRepository -- what depends on it?"

Map the full dependency graph before making changes.

```text
1. toktoken inspect:outline src/Repository/UserRepository.php
   --> See all public methods (the contract you might break).

2. toktoken find:callers "src/Repository/UserRepository.php::UserRepository.findByEmail#method"
   --> Who calls findByEmail()? These are the direct consumers.

3. toktoken find:importers "src/Repository/UserRepository.php"
   --> Every file that imports UserRepository. This is the maximum blast radius.

4. toktoken inspect:dependencies "src/Repository/UserRepository.php" --depth 3
   --> What does UserRepository itself depend on? If you change its constructor,
   --> you need to know what it injects.

5. toktoken search:cooccurrence "UserRepository,Transaction"
   --> Which files use BOTH UserRepository and Transaction?
   --> These are the files where refactoring is most complex.
```

### "I want to understand the architecture of an unfamiliar project"

Top-down exploration using structural commands only (no source reading).

```text
1. toktoken inspect:tree --depth 2
   --> Get the directory structure. Understand the project layout.

2. toktoken search:symbols "main|app|server|bootstrap" --kind function,class --unique --limit 10
   --> Find entry points.

3. toktoken inspect:outline src/app.py --kind class,function
   --> See the structure of the main entry point.

4. toktoken inspect:hierarchy src/models/base.py
   --> See the class hierarchy. Which classes extend BaseModel?
   --> Understand the inheritance tree without reading any source.

5. toktoken inspect:dependencies src/app.py --depth 2
   --> What does the entry point import? This shows the top-level architecture.

6. toktoken search:cooccurrence "Router,Controller"
   --> Where are routing and controllers wired together?

7. toktoken stats
   --> How big is this project? How many files, symbols, languages?
```

### "I want to review a library before adopting it"

Index a remote repository and explore its API surface.

```text
1. toktoken index:github owner/repo-name
   --> Clone and index the repository in one step.

2. toktoken inspect:tree --depth 2
   --> Understand the project structure.

3. toktoken search:symbols "" --kind class,interface --unique --limit 30
   --> List all public classes and interfaces (the API surface).

4. toktoken inspect:outline src/Client.php --kind method
   --> See the public methods of the main client class.

5. toktoken search:text "throw|raise|error|exception" --group-by file
   --> How does the library handle errors? Which files have error handling?

6. toktoken inspect:dependencies src/Client.php --depth 3
   --> What does the library depend on internally? Heavy dependency trees
   --> are a red flag.

7. toktoken search:symbols "deprecated" --count
   --> How much deprecated API surface exists?
```

### "I need to find similar implementations across the codebase"

Use semantic similarity and co-occurrence to find patterns.

```text
1. toktoken search:similar "src/Services/EmailNotifier.php::EmailNotifier.send#method"
   --> Find methods similar to send() -- likely other notification channels
   --> (SlackNotifier.send, SmsNotifier.send, etc.)

2. toktoken search:cooccurrence "Logger,HttpClient"
   --> Which files use both Logger and HttpClient?
   --> These are likely API integration points with logging.

3. toktoken inspect:hierarchy src/Notifications/BaseNotifier.php
   --> See all classes that extend BaseNotifier.
   --> This shows the full notification subsystem without reading any file.

4. toktoken find:references "EmailNotifier"
   --> Find every import statement that references EmailNotifier.
   --> Different from find:importers (which works on files, not identifiers).
```

### "I need to read a specific section of a large file"

When you need file content but not the whole file.

```text
1. toktoken inspect:outline src/database/migrations.py
   --> See the structure with line numbers. Identify the section you need.

2. toktoken inspect:file src/database/migrations.py --lines 142-198
   --> Read only lines 142-198 (the specific migration you care about).
   --> On a 2000-line file, this saves ~95% of tokens.
```

**Key principle:** `inspect:outline` first gives you line numbers, then `inspect:file --lines` or `inspect:symbol` retrieves only what you need. Never read a whole file when you know which section you want.

### "I want to find and remove dead code"

Identify unreferenced symbols and verify they are safe to delete.

```text
1. toktoken find:dead
   --> List all symbols classified as unreferenced, with confidence levels.
   --> High-confidence = no callers, no importers, not exported. Low-confidence = heuristic only.

2. toktoken inspect:blast "src/Utils/LegacyFormatter.php::LegacyFormatter#class"
   --> Even if find:dead flagged it, confirm the blast radius is truly zero
   --> before deleting. BFS on the reverse import graph shows every file
   --> that would be affected by its removal.

3. toktoken find:importers "src/Utils/LegacyFormatter.php" --has-importers
   --> Double-check: are there any importers? --has-importers returns only
   --> files that actually have at least one importer, making the negative
   --> signal easy to confirm.

4. toktoken inspect:symbol "src/Utils/LegacyFormatter.php::LegacyFormatter#class"
   --> Read the symbol source to confirm it is safe to remove (no side effects
   --> in constructors, no global state, no auto-registration hooks).
```

**Key principle:** Never delete based on `find:dead` alone. Always confirm with `inspect:blast` and `find:importers --has-importers`. Confidence levels are heuristic -- low-confidence results require manual review.

### "I want to detect and break circular dependencies"

Find import cycles before they cause runtime issues or make refactoring impossible.

```text
1. toktoken inspect:cycles
   --> Run Tarjan's SCC algorithm across the full import graph.
   --> Each reported cycle is a strongly connected component with 2+ files.

2. toktoken inspect:dependencies src/ServiceA.php --depth 3
   --> For each file in the cycle, trace its import graph to understand
   --> which dependency introduced the loop.

3. toktoken inspect:bundle "src/ServiceA.php::ServiceA#class" --include-callers
   --> Get the full context of the class at the root of the cycle,
   --> including its callers, to decide which dependency to invert or extract.

4. toktoken find:importers "src/ServiceA.php"
   --> Who imports ServiceA? If you extract an interface to break the cycle,
   --> these are the files that will need to be updated.
```

### "I need to navigate documentation structure (headings, sections)"

Markdown files (`.md`, `.markdown`, `.mdx`) are indexed by default. Headings become
symbols: chapter (H1), section (H2), subsection (H3-H6).

```text
1. toktoken search:symbols "Installation" --kind section
   --> Find the "Installation" section across all indexed .md files.

2. toktoken inspect:symbol "docs/INSTALL.md::Installation#section"
   --> Read ONLY the Installation section content (between this heading and the next).
```

Documentation kinds are separate from code kinds:

- Code: class, method, function, property, variable, interface, trait, constant, enum, namespace, type, directive
- Documentation: chapter, section, subsection

---

## Configuration

> **Schema version:** The index database is schema v4. v4 adds a `file_centrality` table that boosts search ranking for high-import files. Indexes created with older versions are automatically migrated on the first `index:update`.

### Project config: `.toktoken.json`

Place in the project root to customize indexing behavior:

```json
{
    "index": {
        "max_file_size_kb": 2048,
        "max_files": 10000,
        "staleness_days": 7,
        "ctags_timeout_seconds": 120,
        "extra_ignore_patterns": ["*.generated.go", "dist/"],
        "languages": ["python", "javascript", "typescript"],
        "extra_extensions": {"blade": "php", "svx": "svelte"},
        "smart_filter": true
    }
}
```

Set `"smart_filter": false` to index all file types including CSS, HTML, and vendored subdirectories. Note: Markdown files are always indexed regardless of the smart filter setting.

Global config at `~/.toktoken.json` supports all sections (`index`, `logging`). Project config only supports `index`.

Environment variables (highest priority):

- `TOKTOKEN_EXTRA_IGNORE` -- JSON array or comma-separated patterns
- `TOKTOKEN_STALENESS_DAYS` -- integer (min 1)
- `TOKTOKEN_EXTRA_EXTENSIONS` -- "ext1:lang1,ext2:lang2"

---

## Troubleshooting

### `index:create` fails with "ctags not found" or "exuberant-ctags detected"

TokToken requires **universal-ctags**, not the older exuberant-ctags. Run `ctags --version` to check. If it shows `Exuberant Ctags` or nothing, install universal-ctags (see Step 2.1). On some systems, `exuberant-ctags` is installed as the default `ctags` -- you must replace it.

### `index:create` produces 0 symbols

Likely causes:

1. **Wrong directory:** `toktoken codebase:detect` must return `action: "index:create"` or `action: "ready"`. If it returns exit code 1, the directory has no indexable source files.
2. **Smart filter excluded everything:** If your project is primarily CSS/HTML/YAML, run `toktoken index:create --full` to disable the smart filter. Note: Markdown files (`.md`) are never excluded by the smart filter.
3. **Language not supported:** Check [docs/LANGUAGES.md](LANGUAGES.md) for the list of supported languages. If your language uses non-standard file extensions, configure `extra_extensions` in `.toktoken.json`.

### `index:create` is very slow (> 60 seconds for a small project)

1. **Large files:** Files over 2MB are skipped by default. If you have auto-generated files (e.g., `bundle.js`, `*.min.js`), add them to `extra_ignore_patterns` in `.toktoken.json`.
2. **ctags timeout:** For very large files that ctags struggles with, increase `ctags_timeout_seconds` in `.toktoken.json` (default: 120).
3. **Too many files:** Check `toktoken stats` for the file count. If it's indexing vendored/generated code, use `--exclude` or `extra_ignore_patterns`.

### MCP server not recognized by the agent

1. **Config file format:** Each agent uses a different JSON format. Claude Code uses `claude mcp add-json`, Cursor uses `mcpServers`, VS Code/Copilot uses `servers` (NOT `mcpServers`). Check the agent-specific setup guide in [Step 3](#step-3-configure-agent-integration).
2. **Restart required:** Most agents require a restart after changing MCP configuration.
3. **Binary not in PATH:** The MCP config uses `"command": "toktoken"` which requires the binary to be in PATH. Run `which toktoken` to verify. If it's not found, add `~/.local/bin` to your PATH.

### `search:symbols` returns no results but the symbol exists

1. **Index is stale:** Run `toktoken index:update` to refresh.
2. **Symbol is in an excluded file type:** If the symbol is in a CSS/HTML/YAML file, re-index with `--full`. Markdown symbols (headings) are always indexed — no `--full` needed.
3. **Try text search:** `toktoken search:text "symbolName"` searches raw file content, not the symbol index. If text search finds it but symbol search doesn't, the symbol might not be recognized by ctags for that language.

### Permission denied errors

TokToken stores its index under `~/.cache/.toktoken/`. If this directory has wrong permissions, run:

```bash
mkdir -p ~/.cache/.toktoken && chmod 755 ~/.cache/.toktoken
```

On Windows, the cache directory is `%LOCALAPPDATA%\.toktoken\`.
