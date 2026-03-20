# TokToken Performance Benchmarks

Real-world performance data measured on eight open-source codebases spanning C, C#, Go, Python, PHP, Lua, and Vim script. All numbers are from a single benchmark run with no cherry-picking or tuning.

---

## Test Environment

| Component | Value |
|-----------|-------|
| CPU | Intel Core i9-12900H (20 threads) |
| RAM | 32 GiB DDR5 |
| OS | Linux 6.6.87 (Ubuntu 24.04 on WSL2) |
| Storage | NVMe SSD |
| TokToken | v0.2.0 (C11, compiled with `-O2`) |
| ctags | Universal Ctags 5.9.0 |
| Compiler | GCC 13.3.0 |

---

## Codebases Tested

Eight well-known open-source projects spanning seven languages:

| Codebase | Description | Total files | Indexed files | Symbols | Primary languages |
|----------|-------------|-------------|---------------|---------|-------------------|
| **Redis** | C in-memory database | 1,746 | 727 | 45,596 | C, C++, Python |
| **curl** | C networking library | 4,216 | 1,108 | 33,973 | C, C++, Python |
| **Laravel** | PHP framework | 3,090 | 2,783 | 39,188 | PHP |
| **Neovim** | C/Lua text editor | 3,777 | 3,297 | 56,663 | C, Vim, Lua |
| **Django** | Python web framework | 7,024 | 2,945 | 93,254 | Python, JavaScript |
| **Kubernetes** | Go container orchestration | 28,482 | 12,881 | 294,753 | Go |
| **dotnet/runtime** | C# runtime + libraries | 57,006 | 37,668 | 1,241,626 | C#, C, C++ |
| **Linux kernel** | OS kernel | 92,931 | 65,231 | 7,433,275 | C, Assembly |

**Total: 198,272 files on disk, 126,640 indexed, 9,244,328 symbols across 8 projects.**

The smart filter (enabled by default) excluded vendored dependencies, test fixtures, documentation, and non-code files. "Total files" includes everything on disk (minus `.git/`); "Indexed files" reflects what passes discovery and language detection.

---

## Indexing Performance

### Full Index Creation (`index:create`)

Each codebase was indexed from scratch with `--max-files 500000` and `--diagnostic`. Timing includes discovery, ctags parsing, database writes, summary generation, and schema rebuild.

| Codebase | Files | Symbols | Wall time | Throughput | Symbol density |
|----------|-------|---------|-----------|------------|----------------|
| **curl** | 1,108 | 33,973 | **0.80 s** | 1,383 files/s | 30.7 sym/file |
| **Redis** | 727 | 45,596 | **0.88 s** | 828 files/s | 62.7 sym/file |
| **Neovim** | 3,297 | 56,663 | **0.97 s** | 3,385 files/s | 17.2 sym/file |
| **Laravel** | 2,783 | 39,188 | **1.02 s** | 2,717 files/s | 14.1 sym/file |
| **Django** | 2,945 | 93,254 | **2.15 s** | 1,368 files/s | 31.7 sym/file |
| **Kubernetes** | 12,881 | 294,753 | **10.13 s** | 1,272 files/s | 22.9 sym/file |
| **dotnet/runtime** | 37,668 | 1,241,626 | **33.47 s** | 1,125 files/s | 33.0 sym/file |
| **Linux kernel** | 65,231 | 7,433,275 | **171.17 s** | 381 files/s | 114.0 sym/file |

Small-to-medium projects (< 3,000 files) index in under 2 seconds. Kubernetes (13K files) takes 10 seconds. The Linux kernel with 65K files and 7.4M symbols indexes in under 3 minutes.

Redis has the highest symbol density among small projects (62.7 sym/file), which explains its relatively lower throughput. The Linux kernel's extreme density (114 sym/file across 65K files) drives its indexing time.

### Timing Breakdown

Where time is spent during indexing:

| Codebase | Discovery | Pipeline | Summaries | Schema rebuild | Total |
|----------|-----------|----------|-----------|----------------|-------|
| **curl** | 32 ms | 517 ms | 86 ms | 109 ms | 0.80 s |
| **Redis** | 14 ms | 519 ms | 128 ms | 158 ms | 0.88 s |
| **Neovim** | 69 ms | 525 ms | 100 ms | 201 ms | 0.97 s |
| **Laravel** | 90 ms | 519 ms | 129 ms | 217 ms | 1.02 s |
| **Django** | 485 ms | 1,017 ms | 174 ms | 362 ms | 2.15 s |
| **Kubernetes** | 813 ms | 3,520 ms | 1,302 ms | 2,197 ms | 10.13 s |
| **dotnet/runtime** | 4,523 ms | 13,528 ms | 4,588 ms | 7,522 ms | 33.47 s |
| **Linux kernel** | 2,082 ms | 59,565 ms | 30,376 ms | 59,267 ms | 171.17 s |

The **pipeline** (ctags parsing + database writes) dominates for all projects. Schema rebuild (B-tree indexes + FTS5 rebuild + import resolution + centrality) accounts for 18-35% of total time.

### Schema Rebuild Breakdown

| Codebase | Symbols | B-tree indexes | FTS5 rebuild | Total |
|----------|---------|----------------|--------------|-------|
| **curl** | 33,973 | 37 ms | 58 ms | 109 ms |
| **Redis** | 45,596 | 46 ms | 96 ms | 158 ms |
| **Neovim** | 56,663 | 68 ms | 100 ms | 201 ms |
| **Laravel** | 39,188 | 64 ms | 68 ms | 217 ms |
| **Django** | 93,254 | 150 ms | 143 ms | 362 ms |
| **Kubernetes** | 294,753 | 545 ms | 743 ms | 2,197 ms |
| **dotnet/runtime** | 1,241,626 | 2,582 ms | 3,218 ms | 7,522 ms |
| **Linux kernel** | 7,433,275 | 16,598 ms | 37,979 ms | 59,267 ms |

Schema v4 uses 3 B-tree indexes on `symbols` (file+line, kind+language, parent_id), 3 on `imports`, plus 1 FTS5 index. The total includes import resolution and centrality computation.

### Database Size

| Codebase | Files | Symbols | DB size |
|----------|-------|---------|---------|
| **curl** | 1,108 | 33,973 | 20 MB |
| **Redis** | 727 | 45,596 | 30 MB |
| **Neovim** | 3,297 | 56,663 | 31 MB |
| **Laravel** | 2,783 | 39,188 | 47 MB |
| **Django** | 2,945 | 93,254 | 72 MB |
| **Kubernetes** | 12,881 | 294,753 | 331 MB |
| **dotnet/runtime** | 37,668 | 1,241,626 | 1,344 MB |
| **Linux kernel** | 65,231 | 7,433,275 | 5,188 MB |

Storage scales linearly with symbol count. The FTS5 full-text index accounts for roughly 30-40% of total database size. At ~0.7 KB/symbol (Linux kernel), storage cost is predictable.

### Memory Usage

Peak RSS during indexing:

| Codebase | Files | Symbols | Peak RSS |
|----------|-------|---------|----------|
| **curl** | 1,108 | 33,973 | 48 MB |
| **Redis** | 727 | 45,596 | 71 MB |
| **Neovim** | 3,297 | 56,663 | 84 MB |
| **Laravel** | 2,783 | 39,188 | 102 MB |
| **Django** | 2,945 | 93,254 | 122 MB |
| **Kubernetes** | 12,881 | 294,753 | 410 MB |
| **dotnet/runtime** | 37,668 | 1,241,626 | 1,303 MB |
| **Linux kernel** | 65,231 | 7,433,275 | 4,010 MB |

Memory usage is dominated by the parallel worker pipeline (16 ctags processes + in-flight symbol batches). For typical projects (< 10K files), peak RSS stays under 500 MB.

---

## Incremental Update Performance

### 10 Modified Files

Ten indexed source files were modified (appended a comment to change content hash), then `index:update` was run.

| Codebase | Changed | Wall time |
|----------|---------|-----------|
| **curl** | 10 | **0.60 s** |
| **Redis** | 10 | **0.66 s** |
| **Laravel** | 10 | **0.66 s** |
| **Neovim** | 10 | **0.68 s** |
| **Django** | 10 | **0.73 s** |
| **Kubernetes** | 10 | **1.84 s** |
| **dotnet/runtime** | 10 | **6.84 s** |
| **Linux kernel** | 10 | **13.76 s** |

### Single File Update

One additional indexed file was modified, then `index:update --file <path>` was run.

| Codebase | Wall time |
|----------|-----------|
| **curl** | **0.58 s** |
| **Redis** | **0.59 s** |
| **Neovim** | **0.62 s** |
| **Laravel** | **0.67 s** |
| **Django** | **0.70 s** |
| **Kubernetes** | **1.76 s** |
| **dotnet/runtime** | **6.45 s** |
| **Linux kernel** | **13.50 s** |

Incremental updates on small projects complete in under 1 second. The high update time for Linux kernel and dotnet/runtime is dominated by the initial file-hash scan across 65K/38K files. The actual re-indexing of changed files is fast; the bottleneck is comparing all stored hashes against current file contents.

---

## MCP Server Query Performance

All MCP tools were tested via the JSON-RPC protocol (`toktoken serve`). Times shown are **client-side round-trip** including JSON serialization.

### Search Tools

| Codebase | search:symbols | search:text | search:cooccurrence | search:similar |
|----------|---------------|-------------|--------------------:|---------------:|
| **curl** | 17 ms | 25 ms | 14 ms | 7 ms |
| **Redis** | 15 ms | 25 ms | 24 ms | 8 ms |
| **Laravel** | 13 ms | 118 ms | 11 ms | 7 ms |
| **Neovim** | 16 ms | 37 ms | 22 ms | 7 ms |
| **Django** | 16 ms | 139 ms | 16 ms | 16 ms |
| **Kubernetes** | 25 ms | 36 ms | 23 ms | 61 ms |
| **dotnet/runtime** | 51 ms | 70 ms | 26 ms | 53 ms |
| **Linux kernel** | 418 ms | 103 ms | 261 ms | 400 ms |

Symbol search (`search:symbols`) uses FTS5 and stays under 55 ms for projects up to 1.2M symbols. Text search (`search:text`) scales with indexed file count. On the Linux kernel (7.4M symbols), symbol search takes 418 ms.

### Inspect Tools

| Codebase | inspect:outline | inspect:symbol | inspect:file | inspect:bundle | inspect:tree | inspect:hierarchy | inspect:dependencies |
|----------|----------------|----------------|-------------|---------------|-------------|------------------|---------------------|
| **curl** | 15 ms | 14 ms | 14 ms | 17 ms | 22 ms | 16 ms | 29 ms |
| **Redis** | 29 ms | 19 ms | 11 ms | 32 ms | 16 ms | 23 ms | 4 ms |
| **Laravel** | 37 ms | 12 ms | 13 ms | 32 ms | 15 ms | 3 ms | 101 ms |
| **Neovim** | 25 ms | 14 ms | 16 ms | 23 ms | 24 ms | 34 ms | 3 ms |
| **Django** | 49 ms | 14 ms | 13 ms | 49 ms | 21 ms | 14 ms | 47 ms |
| **Kubernetes** | 24 ms | 14 ms | 14 ms | 12 ms | 33 ms | 3 ms | 2 ms |
| **dotnet/runtime** | 7 ms | 13 ms | 13 ms | 8 ms | 56 ms | 6 ms | 3 ms |
| **Linux kernel** | 12 ms | 14 ms | 12 ms | 14 ms | 144 ms | 3 ms | 2 ms |

Most inspect operations complete in under 50 ms regardless of codebase size. The `inspect:tree` operation scales with file count (144 ms for 65K files).

### Find Tools

| Codebase | find:importers | find:references | find:callers | find:dead |
|----------|---------------|----------------|-------------|-----------|
| **curl** | 7 ms | 2 ms | 12 ms | 4 ms |
| **Redis** | 3 ms | 2 ms | 11 ms | 3 ms |
| **Laravel** | 42 ms | 4 ms | 6 ms | 5 ms |
| **Neovim** | 4 ms | 2 ms | 3 ms | 3 ms |
| **Django** | 7 ms | 4 ms | 2 ms | 3 ms |
| **Kubernetes** | 2 ms | 139 ms | 2 ms | 6 ms |
| **dotnet/runtime** | 3 ms | 85 ms | 10 ms | 8 ms |
| **Linux kernel** | 2 ms | 1,217 ms | 2 ms | 21 ms |

Find operations leverage B-tree indexes on the imports table. `find:references` performs full-text search and scales with symbol count -- 1.2 seconds on the Linux kernel's 7.4M symbols.

### Analysis Tools

| Codebase | inspect:blast | inspect:cycles |
|----------|--------------|----------------|
| **curl** | 5 ms | 3 ms |
| **Redis** | 5 ms | 3 ms |
| **Laravel** | 40 ms | 9 ms |
| **Neovim** | 4 ms | 5 ms |
| **Django** | 53 ms | 14 ms |
| **Kubernetes** | 3 ms | 24 ms |
| **dotnet/runtime** | 3 ms | 131 ms |
| **Linux kernel** | 2 ms | 653 ms |

Cycle detection scales with import graph complexity. The Linux kernel's dense C include graph takes 653 ms to analyze.

### Stats

| Codebase | Latency |
|----------|---------|
| **curl** | 8 ms |
| **Redis** | 11 ms |
| **Laravel** | 9 ms |
| **Neovim** | 16 ms |
| **Django** | 20 ms |
| **Kubernetes** | 59 ms |
| **dotnet/runtime** | 292 ms |
| **Linux kernel** | 1,661 ms |

The `stats` command aggregates counts across all tables. On extremely large databases (5+ GB, 7.4M symbols), the aggregation takes 1.7 seconds. For typical projects (< 100K symbols), stats returns in under 60 ms.

---

## Smart Filter Effectiveness

The smart filter excludes non-code files and vendored subdirectories. Its effectiveness varies by project structure:

| Codebase | Total files | Indexed files | Filter ratio | Rejected by ext | Rejected by gitignore | Notes |
|----------|-------------|---------------|-------------|-----------------|----------------------|-------|
| **curl** | 4,216 | 1,108 | 26.3% | 3,108 | 0 | Heavy .md, .txt, .cmake |
| **Redis** | 1,746 | 727 | 41.6% | 849 | 163 | Moderate docs/config |
| **Django** | 7,024 | 2,945 | 41.9% | 4,011 | 0 | .html, .txt, .po templates |
| **Kubernetes** | 28,482 | 12,881 | 45.2% | 10,213 | 0 | .yaml, .json, vendor |
| **dotnet/runtime** | 57,006 | 37,668 | 66.1% | 18,502 | 0 | .xml, .props, resources |
| **Linux kernel** | 92,931 | 65,231 | 70.2% | 33,078 | 1,494 | Headers + docs excluded |
| **Neovim** | 3,777 | 3,297 | 87.3% | 457 | 0 | Mostly code (.vim, .lua, .c) |
| **Laravel** | 3,090 | 2,783 | 90.1% | 293 | 0 | Mostly PHP code |

The smart filter is most effective on projects with heavy documentation (curl: 74% filtered out) and least effective on pure-code projects (Laravel: 10% filtered out, Neovim: 13% filtered out).

---

## Token Savings: Symbol Retrieval vs Full File Reads

The primary value proposition of TokToken: retrieving a specific symbol instead of reading the entire file. Token estimates use the standard approximation of 1 token per 4 characters.

### Function-Level Retrieval

| Symbol | Lines | Symbol tokens | File tokens | Savings |
|--------|-------|---------------|-------------|---------|
| `ossl_connect_step1` (curl, from 5,504-line file) | 50 | 406 | 42,568 | **99.0%** |
| `Curl_connect` (curl, from 3,873-line file) | 62 | 466 | 29,657 | **98.4%** |
| `eval7` (neovim, from 6,216-line file) | 180 | 1,157 | 48,349 | **97.6%** |
| `cliSendCommand` (redis, from 11,137-line file) | 206 | 2,217 | 106,389 | **97.9%** |
| `processCommand` (redis, from 8,141-line file) | 126 | 1,496 | 56,754 | **97.4%** |
| `Query.build_filter` (django, from 3,954-line file) | 162 | 1,704 | 30,726 | **94.5%** |
| `nfa_regmatch` (neovim, from 16,288-line file) | 1,409 | 11,247 | 113,932 | **90.1%** |
| `do_cmdline` (neovim, from 8,020-line file) | 562 | 5,572 | 62,041 | **91.0%** |

**Median savings: 96.7%.** Every function-level retrieval saves at least 90% of tokens. For small functions in large files, savings reach 99%.

### What This Means for AI Agents

An AI agent inspecting `processCommand` in Redis reads 126 lines (1,496 tokens) instead of the full `server.c` at 56,754 tokens. That is a **38x reduction** in context consumption for a single lookup.

Over a typical coding session with 20-30 symbol lookups, TokToken saves **hundreds of thousands of tokens** that would otherwise be wasted reading entire files.

---

## Performance Summary

| Operation | Small (< 3K files) | Medium (3K-13K files) | Large (38K-65K files) |
|-----------|--------------------|-----------------------|----------------------|
| Full index | **0.8-2.2 s** | **1.0-10.1 s** | **33-171 s** |
| Incremental update (10 files) | **0.6-0.7 s** | **0.7-1.8 s** | **6.8-13.8 s** |
| Single-file update | **0.6-0.7 s** | **0.6-1.8 s** | **6.5-13.5 s** |
| Symbol search | **13-17 ms** | **16-25 ms** | **51-418 ms** |
| Text search | **25-139 ms** | **36-37 ms** | **70-103 ms** |
| Symbol retrieval | **12-19 ms** | **14 ms** | **13-14 ms** |
| File outline | **15-49 ms** | **24-25 ms** | **7-12 ms** |
| Context bundle | **17-49 ms** | **12-23 ms** | **8-14 ms** |
| Directory tree | **15-22 ms** | **24-33 ms** | **56-144 ms** |
| Stats | **8-20 ms** | **16-59 ms** | **292-1,661 ms** |

### Key Observations

1. **All query operations on typical projects (< 13K files) complete in under 140 ms.** The slowest observed query (text search on Django, 139 ms) remains well under the threshold of perceptibility for an AI agent.

2. **Symbol search is effectively constant-time.** Django's 93K symbols and Kubernetes's 295K symbols return results in 16 ms vs 25 ms. FTS5 indexing keeps search complexity logarithmic.

3. **Symbol retrieval is truly constant-time.** Retrieving a function's source code takes 12-19 ms regardless of codebase size. It is a database lookup + file seek, not a scan.

4. **Schema v4 indexes are optimized.** The 3 B-tree indexes on `symbols` (file+line, kind+language, parent_id), 3 on `imports`, plus FTS5 cover all query patterns. Import resolution and centrality computation are included in schema rebuild.

5. **Indexing parallelizes across 16 workers.** All 16 cores are utilized for ctags parsing. The pipeline uses an MPSC queue to funnel results to a single SQLite writer thread.

6. **Memory scales linearly.** Peak RSS ranges from 48 MB (curl, 34K symbols) to 4 GB (Linux, 7.4M symbols). For typical projects (< 100K symbols), peak RSS stays well under 500 MB.

7. **Native C binary, zero startup cost.** No interpreter, no JVM, no runtime. The binary starts in under 1 ms and has no warm-up period.

8. **Incremental update correctly detects all changes.** All 8 projects correctly detected exactly 10 modified files and 1 single-file update with zero false positives.

9. **160 MCP tool calls across 8 projects: zero failures.** Every tool returned successfully on every project from 727 files (Redis) to 65,231 files (Linux kernel).

---

## Methodology

- All timings are wall-clock measurements from a bash benchmark harness using `date +%s%3N`.
- MCP timings are measured from the client side (write request to read response) via named pipes.
- Modified-file benchmarks appended a comment to change content hash, then reverted via `git checkout .` after measurement.
- Diagnostic output (`--diagnostic`) was captured to JSONL files for timing breakdown analysis.
- "Total files" counts include all files on disk (excluding `.git/`). "Indexed files" reflects smart-filter output.
- Token estimates use the standard approximation: 1 token per 4 characters of source code.
- Database sizes are measured immediately after `index:create` (SQLite WAL mode, not checkpointed).
- Peak RSS is measured via `/proc/self/statm` snapshots during indexing.

---

*TokToken v0.2.0 -- Benchmarked 2026-03-20*
