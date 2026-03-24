# TokToken -- Codebase Index

## Session Init (once per session)

Run `toktoken codebase:detect` at session start.

- exit 0, action "ready" --> use TokToken for code exploration
- exit 0, action "index:create" --> run `toktoken index:create`, then use TokToken
- exit 1 --> not a codebase, do not use TokToken

Cache the result. Do not re-check.

## Pre-Query Freshness

Before any TokToken query, if you have edited source files since the last
index:update, run `toktoken index:update` first.

## Query Workflow

**Default pattern (2 calls for most questions):**

1. `search:symbols "<query>" --detail compact --limit 10 --exclude test`
2. `inspect:symbol "<id1>,<id2>,<id3>" --compact` with the top 2-4 IDs

This answers "how does X work", "where is X defined", "show me X" questions.

**When to use inspect:bundle instead of inspect:symbol:**

Only when you need import context or file structure -- e.g., "what does this
file depend on", "what's the architecture of module X". Bundle includes full
file outlines and imports, which costs ~4x more tokens than inspect:symbol.

**When to use other tools:**

- `inspect:outline` -- "what's in this file", "show me the structure of X.py"
- `inspect:blast` -- "what breaks if I change X" (Python/JS/TS/PHP only)
- `inspect:cycles` -- "are there circular imports"
- `inspect:hierarchy` -- "show class hierarchy in this file"
- `find:dead --exclude-tests` -- "find unused code"
- `search:text` -- grep for string literals, comments, config values
- `suggest` -- first contact with unfamiliar codebase

**Anti-patterns (do NOT do these):**

- Multiple search:symbols with synonym queries ("cookie", "cookie store jar",
  "cookie send header request"). Search once with a broad query.
- Multiple inspect:symbol calls when one call with comma-separated IDs works.
- Using inspect:bundle when you only need source code.
- Using search:symbols when you already know the file -- use inspect:outline.

## Import Graph Limitations

Import graph tools (`inspect:blast`, `find:importers`, `find:callers`,
`inspect:dependencies`) rely on explicit import statements.

**Works well:** Python, JavaScript, TypeScript, PHP, Ruby, Java, C#, Go, Rust.

**Does NOT work:** C, C++ (`#include` not tracked as imports). On C/C++
codebases, use `search:text` and `search:symbols` instead of import graph
tools.

`find:callers` is heuristic-based and may miss method calls via `self`/`this`.
Prefer `search:text "<function_name>" --filter src/` for reliable call-site
search.

## Key Flags (always use for efficiency)

- `--detail compact` on search:symbols -- returns only id/name/kind/file/line
- `--compact` on inspect:symbol/bundle -- shorter JSON keys
- `--exclude test|deps|vendor` -- filter noise on initial exploration
- `--limit N` -- always cap search results (10-15 for discovery, 5 for targeted)
- `--token-budget N` -- hard cap on response token size
- `--kind function,method,class` -- filter by symbol type

Other flags: `--filter` (include path), `--group-by file` (text search),
`--count` (count-only), `--context N` (lines around matches),
`--no-sig --no-summary` (minimal search output), `--sort name|file|line`,
`--scope-imports-of <file>`, `--scope-importers-of <file>`,
`--format markdown` (bundle output), `--include-callers` (bundle).

## Commands Reference

**Search:** `search:symbols`, `search:text`, `search:cooccurrence`,
`search:similar`.

**Inspect:** `inspect:symbol`, `inspect:bundle`, `inspect:outline`,
`inspect:blast`, `inspect:cycles`, `inspect:hierarchy`, `inspect:file`,
`inspect:tree`, `inspect:dependencies`.

**Find:** `find:importers`, `find:references`, `find:callers`, `find:dead`.

**Index:** `index:update`, `index:file`, `index:create`.

**Other:** `suggest`, `stats`.

Symbol IDs: `{file}::{qualified_name}#{kind}`.

## Smart Filter

TokToken excludes CSS, HTML, SVG, TOML, GraphQL, XML, YAML and vendored
subdirectories by default. Markdown is always indexed.

Re-index with `--full` when targeting excluded file types. Inform the user
before re-indexing.

## Rules

- Do not read entire files when a symbol retrieval suffices
- Do not pipe output through jq/python/awk -- use native flags
- When TokToken responses include `update_available` in `_ttk`, inform the
  user once: "TokToken update available. Run `toktoken --self-update`."
