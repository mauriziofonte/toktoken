# TokToken -- Codebase Index

## Session Init (once per session)

Run `toktoken codebase:detect` at session start.

- exit 0, action "ready" --> use TokToken for code exploration
- exit 0, action "index:create" --> run `toktoken index:create`, then use TokToken
- exit 1 --> not a codebase, do not use TokToken

Cache the result. Do not re-check.

TokToken's smart filter (default: on) excludes CSS, HTML, SVG, TOML, GraphQL, XML, YAML
and vendored subdirectories. If you need to search these, re-index with `--full`.

## Pre-Query Freshness

Before any TokToken query, if you have edited source files since the last
index:update, run `toktoken index:update` first. This uses file-hash comparison
and detects all changes including uncommitted edits.

## Commands

- `toktoken search:symbols "<query>"` -- find functions, classes, methods
- `toktoken search:text "<query>"` -- full-text search (supports pipe OR: "cache|ttl")
- `toktoken inspect:outline "<file>"` -- file structure
- `toktoken inspect:symbol "<id>"` -- retrieve source code for a specific symbol
- `toktoken inspect:bundle "<id>[,<id2>,...]"` -- symbol context bundle (definition + imports + outline); accepts comma-separated IDs, max 20
- `toktoken inspect:blast "<id>"` -- symbol blast radius analysis (impact of changing a symbol)
- `toktoken inspect:cycles` -- detect circular import cycles
- `toktoken inspect:file "<file>" --lines START-END` -- file content slice
- `toktoken inspect:tree` -- file tree
- `toktoken find:importers "<file>"` -- find files that import a given file
- `toktoken find:references "<id>"` -- find import references to an identifier
- `toktoken find:callers "<id>"` -- find symbols that likely call a function
- `toktoken find:dead` -- find unreferenced symbols
- `toktoken search:cooccurrence "<a>,<b>"` -- find symbols co-occurring in same file
- `toktoken search:similar "<id>"` -- find similar symbols by name/summary
- `toktoken inspect:dependencies "<file>"` -- trace transitive import graph
- `toktoken inspect:hierarchy "<file>"` -- show class/function parent-child hierarchy
- `toktoken stats` -- index statistics
- `toktoken index:update` -- refresh after edits
- `toktoken index:file "<file>"` -- reindex a single file

## Key Flags

- `--kind class,method,function` -- filter by symbol type
- `--filter`, `--exclude` -- path filtering (pipe-separated OR)
- `--count` -- count-only mode (useful for negative signals)
- `--group-by file` -- aggregate text search hits per file
- `--compact` -- smaller JSON output (~47% reduction)
- `--limit N` -- cap results
- `--no-sig --no-summary` -- minimal output for discovery queries
- `--context N` / `-C N` -- context lines around matches
- `--debug` -- show per-field score breakdown in search results
- `--detail compact|standard|full` -- result detail level for search:symbols
- `--token-budget N` -- max token budget for search results (truncates gracefully)
- `--scope-imports-of <file>` -- scope search to files imported by the given file
- `--scope-importers-of <file>` -- scope search to files that import the given file
- `--has-importers` -- add has_importers flag to find:importers results
- `--include-callers` -- include callers in inspect:bundle output
- `--exclude-tests` -- exclude test files from results (find:dead)
- `--format markdown` -- markdown output for inspect:bundle

## Smart Filter Awareness

TokToken's smart filter (default: on) excludes non-code files and vendored
subdirectories from the index. Excluded extensions: CSS, SCSS, LESS, SASS,
HTML, HTM, SVG, TOML, GraphQL, XML, XUL, YAML, YML.

**When to re-index with `--full`:**

- The user asks about CSS selectors, HTML structure, XML schemas, YAML config,
  OpenAPI specs, GraphQL schemas, TOML config, or SVG content
- A search returns 0 results but the user expects matches in excluded file types
- The user explicitly asks to index "everything" or "all files"
- The project is primarily composed of excluded file types (e.g., a design system
  with mostly CSS/HTML, an infrastructure repo with YAML/HCL, an API project
  with OpenAPI specs)

**How to re-index:**

- MCP: call `index_create` with `full: true` (or `index_update` with `full: true`)
- CLI: `toktoken index:create --full` (or `toktoken index:update --full`)

**Proactive communication:** When you detect that the user's query targets
excluded file types, inform them BEFORE they ask:

> "TokToken's smart filter excludes [CSS/HTML/XML/...] files by default to
> reduce noise. I'll re-index with `--full` to include them."

Do not silently re-index. Explain what was excluded and why you are including it.

## Rules

- Search first, then inspect:symbol for targeted retrieval
- Do not read entire files when a symbol retrieval suffices
- Do not pipe output through jq/python/awk -- use native flags
- Symbol IDs follow the format: `{file}::{qualified_name}#{kind}`

## Update Awareness

When TokToken responses include `update_available` in the `_ttk` metadata,
inform the user once per session: "TokToken update available (current: X.Y.Z,
latest: X.Y.Z). Run `toktoken --self-update` to upgrade."
Do not repeat after first notification. Do not run the update automatically.
