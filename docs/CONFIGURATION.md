# TokToken Configuration Reference

---

## Configuration Hierarchy

Settings are merged in this order (highest priority last):

1. **Hardcoded defaults** -- always present
2. **Global config** `~/.toktoken.json` -- all sections
3. **Project config** `{project}/.toktoken.json` -- only `index` section
4. **Environment variables** -- highest priority overrides

Later sources override earlier ones. Missing or invalid config files are silently ignored.

---

## Config File Format

Both global and project config files use the same JSON format:

```json
{
  "index": {
    "max_file_size_kb": 2048,
    "max_files": 200000,
    "staleness_days": 7,
    "ctags_timeout_seconds": 120,
    "smart_filter": true,
    "extra_ignore_patterns": [],
    "languages": [],
    "extra_extensions": {}
  },
  "logging": {
    "level": "info"
  }
}
```

All fields are optional. Only specified fields override defaults.

---

## Index Options

| Option | Type | Default | Description |
| ------ | ---- | ------- | ----------- |
| `max_file_size_kb` | int | 2048 | Skip files larger than this (KB) |
| `max_files` | int | 200000 | Maximum number of files to index |
| `staleness_days` | int | 7 | Days before the index is considered stale |
| `ctags_timeout_seconds` | int | 120 | Timeout for ctags process execution |
| `extra_ignore_patterns` | string[] | [] | Additional gitignore-style patterns to exclude |
| `languages` | string[] | [] | Only index these languages (empty = all) |
| `workers` | int | 0 | Number of parallel indexing workers. 0 = auto (CPU count, capped at 16) |
| `smart_filter` | bool | true | Exclude non-code file types and vendored subdirectories |
| `extra_extensions` | object | {} | Map file extensions to language parsers |

### extra_ignore_patterns

Gitignore-style glob patterns. Added to the default ignore list (which includes `node_modules/`, `vendor/`, `.git/`, etc.).

```json
{
  "index": {
    "extra_ignore_patterns": [
      "*.generated.go",
      "dist/",
      "coverage/",
      "__snapshots__/"
    ]
  }
}
```

### languages

When non-empty, only files matching these languages are indexed. Language names are lowercase.

```json
{
  "index": {
    "languages": ["python", "javascript", "typescript", "go"]
  }
}
```

### smart_filter

When true (default), TokToken applies automatic exclusions:

- **Non-code extensions excluded**: css, scss, less, sass, html, htm, svg, toml, graphql, gql, xml, xul, yaml, yml
- **Vendored subdirectories pruned**: any non-root directory containing a package manager manifest (composer.json, package.json, setup.py, pyproject.toml, Cargo.toml, go.mod, pom.xml, build.gradle) is skipped

**Note:** Markdown files (`.md`, `.markdown`, `.mdx`) are always indexed regardless of the smart filter setting. Their headings are extracted as documentation-specific symbol kinds: `chapter` (H1), `section` (H2), `subsection` (H3-H6).

**Workspace-aware pruning**: In monorepo setups, TokToken detects workspace members (npm `workspaces`, Cargo `[workspace] members`, Go `go.work use` directives) and exempts them from vendor manifest pruning. Additionally, directories under conventional source paths (`src/`, `lib/`, `packages/`, `apps/`, `internal/`, `modules/`, `crates/`) are never treated as vendored, even if they contain their own package manifests.

These exclusions are additive to .gitignore, `.toktokenignore`, and extra_ignore_patterns.

To index everything:

```json
{
  "index": {
    "smart_filter": false
  }
}
```

Or use the `--full` CLI flag: `toktoken index:create --full`

### .toktokenignore

TokToken supports a `.toktokenignore` file in the project root. This file uses the same gitignore-style syntax as `.gitignore` and is loaded alongside it during file discovery. Use it for TokToken-specific exclusions that should not affect your `.gitignore`.

```text
# .toktokenignore example
*.generated.go
dist/
__snapshots__/
*.test.js
```

This is equivalent to using `extra_ignore_patterns` in `.toktoken.json`, but may be more convenient for projects that prefer file-based configuration.

### extra_extensions

Map custom file extensions to existing language parsers. The key is the extension (without dot), the value is the target language name.

```json
{
  "index": {
    "extra_extensions": {
      "blade": "php",
      "svx": "svelte",
      "tsx": "typescript"
    }
  }
}
```

---

## Logging Options

| Option | Type | Default | Description |
| ------ | ---- | ------- | ----------- |
| `level` | string | "info" | Log level: "debug", "info", "warn", "error" |

Only available in global config (`~/.toktoken.json`).

---

## Environment Variables

Environment variables override all config file values.

| Variable | Format | Description |
| -------- | ------ | ----------- |
| `TOKTOKEN_EXTRA_IGNORE` | JSON array or comma-separated | Additional ignore patterns |
| `TOKTOKEN_STALENESS_DAYS` | integer (min 1) | Staleness threshold in days |
| `TOKTOKEN_EXTRA_EXTENSIONS` | "ext1:lang1,ext2:lang2" | Additional extension mappings |

### Examples

```bash
# JSON array format
export TOKTOKEN_EXTRA_IGNORE='["*.generated.go", "dist/"]'

# Comma-separated format
export TOKTOKEN_EXTRA_IGNORE="*.generated.go,dist/,coverage/"

# Staleness
export TOKTOKEN_STALENESS_DAYS=14

# Extra extensions
export TOKTOKEN_EXTRA_EXTENSIONS="blade:php,svx:svelte"
```

---

## Common Configurations

### Monorepo with generated code

```json
{
  "index": {
    "max_files": 200000,
    "extra_ignore_patterns": [
      "*.generated.*",
      "*/generated/*",
      "*/proto/*.pb.go"
    ]
  }
}
```

### Python-only project

```json
{
  "index": {
    "languages": ["python"],
    "extra_ignore_patterns": [
      "__pycache__/",
      "*.pyc",
      ".venv/"
    ]
  }
}
```

### Full index (include CSS, HTML, vendored code — Markdown is always included)

```json
{
  "index": {
    "smart_filter": false
  }
}
```

### Large files allowed

```json
{
  "index": {
    "max_file_size_kb": 2000
  }
}
```
