# TokToken Security Model

TokToken is designed to index and expose source code to AI agents. This document describes the security measures that protect against common attack vectors.

---

## Threat Model

TokToken operates on the user's local filesystem and communicates over STDIO (MCP mode). It does not listen on network ports and does not collect telemetry. Network I/O is delegated to external subprocesses: `gh` for GitHub operations, and `curl` for version checks and self-update downloads.

The primary threats are:

- **Symlink escape**: a malicious repository could use symlinks to expose files outside the project root
- **Secret leakage**: accidental indexing of credentials, private keys, or environment files
- **Path traversal**: crafted file paths that escape the project directory
- **Binary/large file abuse**: indexing binary blobs or extremely large files
- **Code injection**: user-controlled input interpreted as shell commands

---

## Mitigations

### Symlink Escape Detection

`path_validator.c` validates every file path before indexing:

- Symlink targets are resolved to their canonical path via `realpath()`
- Symlinks are detected using `lstat()` (not `stat()`) to avoid following them prematurely
- If the resolved path is outside the project root, the file is silently skipped
- Broken symlinks (target does not exist) are conservatively treated as escapes
- Path containment uses boundary checking on `/` to prevent prefix confusion (e.g., `/root2` is not under `/root`)

### Secret Pattern Filtering

`secret_patterns.c` matches file names against 35 known secret patterns before indexing:

- `.env`, `.env.*`, `*.env` (environment files)
- `*.pem`, `*.key`, `*.p12`, `*.pfx`, `*.jks`, `*.keystore`, `*.crt`, `*.cer` (certificates and keys)
- `id_rsa`, `id_dsa`, `id_ecdsa`, `id_ed25519` and variants (SSH keys)
- `credentials.json`, `service-account*.json`, `*.credentials` (cloud credentials)
- `*.secret`, `*.secrets`, `*secret*`, `*.token` (generic secrets)
- `.htpasswd`, `.htaccess`, `.netrc`, `.npmrc`, `.pypirc` (auth config)
- `wp-config.php`, `database.yml`, `master.key` (application secrets)

Matched files are excluded from the index entirely -- not just from search results.

**False-positive handling**: broad patterns like `*secret*` are skipped for documentation files (`.md`, `.rst`, `.txt`, `.html`, `.ipynb`, etc.) and source code files (`.php`, `.py`, `.js`, `.ts`, `.go`, `.rs`, `.java`, `.c`, `.cpp`, and 25+ other code extensions) to avoid excluding files like `docs/secret-management.md` or `src/secret_manager.py`.

### Binary File Exclusion

`file_filter.c` detects binary files using a two-layer approach:

1. **Extension-based (fast path)**: 65 known binary extensions (executables, archives, images, fonts, databases, lock files) are rejected immediately via hashmap lookup
2. **Content-based (null byte detection)**: the first 8 KB of file content are scanned for null bytes via `memchr()`. Files containing null bytes are rejected as binary

### Path Traversal Protection

All file operations enforce the project root as a boundary:

- All paths are resolved to their canonical form via `realpath()`, which eliminates `..`, `.`, and symlink indirection
- The resolved path is checked for containment within the resolved project root with `/` boundary verification
- The `inspect:file` and `inspect:symbol` commands only serve files within the indexed project
- GitHub repository names are validated character-by-character with path traversal rejection (`..` is explicitly blocked)

### Code Injection Prevention

All subprocess invocations (ctags, `gh` CLI) use `execvp()` with explicit `argv` arrays:

- No `system()`, `popen()`, or shell string interpolation anywhere in the codebase
- User input (paths, queries, parameters) is passed as data, never as command strings
- GitHub repository names are validated against a character whitelist before being passed to `gh`

### File Size Limits

Files exceeding `max_file_size_kb` (default: 2048 KB / 2 MB) are skipped during indexing. This prevents memory exhaustion from extremely large generated files.

### Smart Filter (Noise Reduction)

By default, TokToken excludes file types that produce noisy symbol data (CSS selectors, HTML tags) and prunes vendored subdirectories detected via package manager manifests. This reduces false positives in search results and prevents third-party code from dominating the index. Markdown files are an exception: they are always indexed because headings produce high-quality documentation symbols (chapter, section, subsection). The smart filter does not replace security filtering -- secret pattern detection, symlink escape checks, and binary exclusion apply regardless of the smart filter setting.

### No External Telemetry

TokToken makes no outbound network connections from its own process. All network I/O is delegated to external subprocesses via `execvp()`:

- `gh` CLI for GitHub repository operations (`index:github`)
- `curl` for upstream version checks (`update_check.c`) and binary downloads (`cmd_update.c`)
- No anonymous usage tracking
- No crash reporting
- No "community meter" or savings sharing
- No phone-home of any kind beyond the version check (which is cached for 12 hours and fails silently)

All data stays on the user's machine.

---

## File Filter Pipeline

The file filtering pipeline applies 10 sequential checks to every candidate file (defense in depth):

| Step | Check | Source |
| ---- | ----- | ------ |
| B1 | Segment-aware skip directory check | `file_filter.c` |
| B2 | Symlink escape detection | `path_validator.c` |
| B3 | Source extension whitelist | `file_filter.c` |
| B4 | Binary extension blacklist | `file_filter.c` |
| B5 | Secret pattern detection | `secret_patterns.c` |
| B6 | Skip file patterns (lock files, minified) | `file_filter.c` |
| B7 | File size limit | `file_filter.c` |
| B8 | Binary content detection (null bytes) | `file_filter.c` |
| B9 | Gitignore rules | `file_filter.c` |
| B10 | Extra ignore patterns | `file_filter.c` |

A file must pass all 10 checks to be indexed.

---

## MCP Server Security

The MCP server (`toktoken serve`) communicates exclusively over STDIO:

- No TCP/HTTP listeners -- input via `getline(stdin)`, output via `fprintf(stdout)`
- No authentication (relies on the MCP client's trust model)
- Tool arguments are validated before execution (required parameters, type checks, GitHub repo format validation)
- Error messages are sanitized -- diagnostic paths go to stderr only, never to the client response
- `SIGPIPE` is handled gracefully to detect client disconnects

---

## Reporting Vulnerabilities

If you discover a security issue, please report it via GitHub Issues or email the maintainer directly. Do not open a public issue for vulnerabilities that could be exploited before a fix is available.
