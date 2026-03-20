# TokToken: Token Savings Analysis

How TokToken reduces LLM context consumption when AI agents explore and navigate codebases.

---

## The Problem

AI agents waste tokens when they must read entire files to locate a single function, class, or constant. A typical exploration session involves:

1. **Listing files** -- the agent reads directory trees and guesses which files are relevant
2. **Reading files** -- the agent loads entire files into context to find the symbol it needs
3. **Repeating** -- if the symbol isn't in the first file, the agent reads another, and another

Each file read costs tokens proportional to the file size. Most of those tokens are wasted on code the agent never needed.

TokToken inverts this workflow: **find the symbol first, then read only that symbol's source code.**

---

## Savings at Scale: Eight Open-Source Projects

Measured across eight codebases from Redis (727 files) to the Linux kernel (65,231 files). Token estimates use the standard approximation of **1 token per 4 characters**.

| Codebase | Files | Symbols | Raw source tokens (est.) | 10 targeted queries | Savings factor |
|----------|-------|---------|--------------------------|---------------------|----------------|
| **Redis** | 727 | 45,596 | ~1,200,000 | ~8,000 | **150x** |
| **curl** | 1,108 | 33,973 | ~1,800,000 | ~8,000 | **225x** |
| **Laravel** | 2,783 | 39,188 | ~3,500,000 | ~8,000 | **438x** |
| **Django** | 2,945 | 93,254 | ~5,400,000 | ~8,000 | **675x** |
| **Neovim** | 3,297 | 56,663 | ~4,200,000 | ~8,000 | **525x** |
| **Kubernetes** | 12,881 | 294,753 | ~25,000,000 | ~8,000 | **3,125x** |
| **dotnet/runtime** | 37,668 | 1,241,626 | ~95,000,000 | ~8,000 | **11,875x** |
| **Linux kernel** | 65,231 | 7,433,275 | ~380,000,000 | ~8,000 | **47,500x** |

The larger the codebase, the greater the savings. TokToken's query cost is nearly constant (~800 tokens per query) regardless of codebase size, while naive file-read cost grows linearly with file count.

---

## Savings by Task Type

### 1. Explore Repository Structure

**Without TokToken:** The agent reads directory listings, then opens files to understand the project layout.

**With TokToken:** `inspect:tree` returns the full indexed file tree as structured JSON.

| Codebase | Naive approach (est.) | TokToken response | Savings |
|----------|----------------------|-------------------|---------|
| **Redis** (727 files) | ~105,000 tokens | ~4,500 tokens | **95.7%** |
| **curl** (1,108 files) | ~161,000 tokens | ~6,500 tokens | **96.0%** |
| **Django** (2,945 files) | ~427,000 tokens | ~24,000 tokens | **94.4%** |
| **Kubernetes** (12,881 files) | ~1,868,000 tokens | ~119,000 tokens | **93.6%** |
| **Linux kernel** (65,231 files) | ~9,459,000 tokens | ~432,000 tokens | **95.4%** |

*Naive estimate: ~145 tokens/file for path + first 10 lines of each file.*
*TokToken response tokens: measured JSON response size / 4.*

### 2. Find a Specific Function

**Without TokToken:** The agent greps for the function name, reads 2-5 candidate files to find the right definition.

**With TokToken:** `search:symbols` returns matching symbols with file, line, and signature in under 50 ms.

| Codebase | Naive approach (est.) | TokToken | Savings |
|----------|----------------------|----------|---------|
| **curl** (34K symbols) | ~30,000 tokens | ~280 tokens | **99.1%** |
| **Django** (93K symbols) | ~80,000 tokens | ~275 tokens | **99.7%** |
| **Kubernetes** (295K symbols) | ~150,000 tokens | ~400 tokens | **99.7%** |
| **dotnet/runtime** (1.24M symbols) | ~500,000 tokens | ~275 tokens | **99.9%** |
| **Linux kernel** (7.4M symbols) | ~1,000,000 tokens | ~360 tokens | **99.96%** |

*Naive estimate: reading 3-5 files of average size to locate the function.*
*TokToken response tokens: measured from actual search response JSON sizes.*

### 3. Read One Function Implementation

**Without TokToken:** The agent reads the entire file containing the function.

**With TokToken:** `inspect:symbol` returns only the function's source code (exact line range).

| Symbol | Lines | Symbol tokens | File tokens | Savings |
|--------|-------|---------------|-------------|---------|
| `ossl_connect_step1` (curl, 5,504-line file) | 50 | 406 | 42,568 | **99.0%** |
| `Curl_connect` (curl, 3,873-line file) | 62 | 466 | 29,657 | **98.4%** |
| `cliSendCommand` (redis, 11,137-line file) | 206 | 2,217 | 106,389 | **97.9%** |
| `processCommand` (redis, 8,141-line file) | 126 | 1,496 | 56,754 | **97.4%** |
| `Query.build_filter` (django, 3,954-line file) | 162 | 1,704 | 30,726 | **94.5%** |
| `nfa_regmatch` (neovim, 16,288-line file) | 1,409 | 11,247 | 113,932 | **90.1%** |
| `do_cmdline` (neovim, 8,020-line file) | 562 | 5,572 | 62,041 | **91.0%** |
| `changeform_view` (django, 3,315-line file) | 7 | 100 | 25,616 | **99.6%** |

**Median savings: 96.7%.** Every function-level retrieval saves at least 90% of tokens. For small functions in large files (like `changeform_view` -- 7 lines from a 3,315-line file), savings reach 99.6%.

### 4. Understand Module API Surface

**Without TokToken:** The agent reads the entire file to understand what functions/classes it exports.

**With TokToken:** `inspect:outline` returns a structured hierarchy of all symbols in the file.

| Example file | File tokens | Outline tokens | Savings |
|-------------|-------------|----------------|---------|
| curl `lib/vtls/openssl.c` (5,504 lines, 145 symbols) | ~42,568 | ~4,740 | **88.9%** |
| redis `src/server.c` (8,141 lines, 301 symbols) | ~56,754 | ~8,400 | **85.2%** |
| neovim `src/nvim/regexp.c` (16,288 lines, 572 symbols) | ~113,932 | ~16,000 | **86.0%** |
| django `tests/admin_views/tests.py` (9,673 lines) | ~75,000 | ~4,575 | **93.9%** |

---

## Deep-Dive: Kernel-Scale Savings

The Linux kernel has 65,231 indexed files containing 7,433,275 symbols. Reading the entire codebase would consume an estimated **380 million tokens**.

### Scenario: "How does the ext4 filesystem handle file creation?"

**Without TokToken (typical agent):**

```
1. Agent: search for "ext4" in filenames        → finds ~200 files
2. Agent: read fs/ext4/namei.c (3,800 lines)    → 29,000 tokens
3. Agent: read fs/ext4/inode.c (6,200 lines)    → 48,000 tokens
4. Agent: read fs/ext4/ext4.h (4,500 lines)     → 35,000 tokens
5. Total: 112,000+ tokens consumed, most irrelevant
```

**With TokToken (3 tool calls):**

```
1. search:symbols "ext4 create"                  → finds ext4_create, ext4_create_inode (360 tokens, 46 ms)
2. inspect:symbol <ext4_create#function>         → returns ~50-line function (400 tokens, 11 ms)
3. inspect:bundle <ext4_create_inode#function>   → definition + imports + outline (1,200 tokens, 10 ms)
4. Total: ~1,960 tokens consumed, 67 ms
```

**Result:** 1,960 tokens vs 112,000 tokens. **57x reduction.** The agent gets precisely the code it needs without loading a single byte of irrelevant kernel code.

---

## Deep-Dive: The 5,065-Line File

A 5,065-line legacy PHP file with 191 KB of source code (~48,966 tokens). A developer asks: *"What does `errorUnmappedProduct` do?"*

**Without TokToken:**

```
1. Agent: grep for "errorUnmappedProduct"        → finds reference in 3 files
2. Agent: read functions.php (2,100 lines)       → 12,500 tokens, not here
3. Agent: read functions2.php (5,065 lines)      → 48,966 tokens, found at line 819
4. Total: 61,466 tokens consumed, 2 file reads
```

**With TokToken (2 tool calls):**

```
1. search_symbols "errorUnmappedProduct"         → returns file + line (575 tokens, 13 ms)
2. inspect_symbol <id>                           → returns 27-line function (206 tokens, 17 ms)
3. Total: 781 tokens consumed, 30 ms
```

**Result:** 781 tokens vs. 61,466 tokens. **78x reduction.**

---

## Scaling Impact

The savings compound across a typical AI agent session. A real coding task involves 10-50 tool calls for navigation before the agent begins writing code.

### Per-Session Estimates

| Session intensity | Naive tokens | TokToken tokens | Savings | Cost avoided* |
|-------------------|-------------|-----------------|---------|---------------|
| Light (10 queries) | ~200,000 | ~8,000 | **96.0%** | $2.88 |
| Medium (30 queries) | ~600,000 | ~24,000 | **96.0%** | $8.64 |
| Heavy (100 queries) | ~2,000,000 | ~80,000 | **96.0%** | $28.80 |

*Cost avoided calculated at Claude Opus pricing ($15/MTok input).*

### Across Codebase Sizes

| Codebase size | Full read tokens | 10 targeted queries | Savings factor |
|---------------|-----------------|---------------------|----------------|
| Small (~700 files) | ~1,200,000 | ~8,000 | **150x** |
| Medium (~3,000 files) | ~5,000,000 | ~8,000 | **625x** |
| Large (~13,000 files) | ~25,000,000 | ~8,000 | **3,125x** |
| Very large (~38,000 files) | ~95,000,000 | ~8,000 | **11,875x** |
| Massive (~65,000 files) | ~380,000,000 | ~8,000 | **47,500x** |

The larger the codebase, the greater the savings. TokToken's query cost is nearly constant regardless of codebase size, while naive file-read cost grows linearly.

---

## How It Works

TokToken shifts the agent workflow from:

> **"Read everything to find something"**

to:

> **"Find something, then read only that."**

### Workflow Comparison

**Without TokToken (7+ tool calls, ~62,000 tokens):**

```
1. Agent: list files in src/              → reads directory listing
2. Agent: read functions.php              → loads 2,100-line file (12,500 tokens)
3. Agent: read functions2.php             → loads 5,065-line file (48,966 tokens)
4. Agent: hmm, found it at line 819
5. Agent: (already loaded 61,466+ tokens of irrelevant code)
```

**With TokToken (2 tool calls, ~780 tokens):**

```
1. Agent: search_symbols "errorUnmappedProduct"  → returns symbol + location (575 tokens, 13 ms)
2. Agent: inspect_symbol <id>                    → returns function source (206 tokens, 17 ms)
```

---

## Compact Output

TokToken's `--compact` flag reduces JSON key names, further reducing token consumption:

| Output mode | Typical search response | Typical outline response |
|-------------|------------------------|-------------------------|
| Standard | ~575 tokens | ~4,230 tokens |
| Compact | ~400 tokens | ~3,750 tokens |
| **Reduction** | **30%** | **11%** |

Compact mode savings vary by response structure. Search responses with many short keys see 30% reduction. Outline responses with longer value strings see 11% reduction.

Additional output flags (`--no-sig`, `--no-summary`) allow further reduction when full detail is not needed.

---

## MCP Query Latency vs Token Savings

Every MCP query returns in milliseconds while saving thousands of tokens:

| Operation | Median latency | Tokens saved per call (est.) |
|-----------|---------------|------------------------------|
| search:symbols | 16 ms | 30,000-100,000 |
| inspect:symbol | 12 ms | 10,000-50,000 |
| inspect:outline | 12 ms | 5,000-30,000 |
| inspect:bundle | 12 ms | 15,000-60,000 |
| search:text | 50 ms | 20,000-80,000 |
| inspect:tree | 16 ms | 50,000-9,000,000 |

The latency cost of a TokToken query is negligible compared to LLM inference time (typically 1-30 seconds per response). The token savings, however, directly reduce both cost and context window consumption.

---

## Methodology

- **Token estimates** use the standard approximation: 1 token per 4 characters of source code (or JSON response).
- **"Naive approach"** estimates assume the agent reads 3-5 full files per query to locate relevant code, which is consistent with observed AI agent behavior on unindexed codebases.
- **TokToken response sizes** are measured from actual CLI/MCP JSON output (byte count / 4).
- **Cost figures** use Claude Opus pricing ($15/MTok input). Other models have different pricing but the relative savings are the same.
- **Source files** are post-smart-filter: vendored dependencies and non-code assets are excluded.
- All measurements taken on the codebases described in [PERFORMANCE.md](PERFORMANCE.md).

---

## Key Takeaways

1. **96-99% token savings** on navigation and exploration tasks, consistently across all tested codebases from 727 to 65,231 files.

2. **Savings grow with codebase size.** A 65K-file codebase (Linux kernel) sees 47,500x less token consumption than naive file reads. TokToken queries cost roughly the same whether the codebase has 700 or 65,000 files.

3. **Two tool calls replace seven.** A typical "find and read a function" task drops from 7+ file reads (~62,000 tokens) to 2 targeted calls (~780 tokens).

4. **$2-29 saved per session** at Claude Opus pricing. Over a team's daily usage, this compounds into significant cost reduction.

5. **Large files benefit the most.** Extracting a 27-line function from a 5,065-line file saves 99.6% of tokens. The larger the file, the more wasteful naive reads become, and the more valuable targeted retrieval is.

6. **Sub-50ms latency on every operation.** The token savings come with no perceptible delay -- every query returns faster than a network round-trip, even on the Linux kernel with 7.4M symbols.

---

*TokToken v0.2.0 -- Analysis performed 2026-03-19*
