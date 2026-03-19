# Contributing to TokToken

## Prerequisites

| Tool | Required | Notes |
|------|----------|-------|
| CMake | >= 3.16 | Build system |
| GCC or Clang | C11 support | `gcc >= 9` or `clang >= 10` |
| universal-ctags | Required | Symbol extraction backend |
| pcre2 (`libpcre2-dev`) | Optional | Regex-based custom parsers |
| Valgrind | Recommended | Memory leak checking |

## Building

### Debug (with ASan/UBSan)

```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..
cmake --build . -j$(nproc)
```

Debug mode enables `-fsanitize=address -fsanitize=undefined` automatically.

### Release

```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . -j$(nproc)
```

### Static binary (Linux only)

```bash
cmake -DCMAKE_BUILD_TYPE=Release -DTT_STATIC=ON ..
cmake --build . -j$(nproc)
```

## Running Tests

### All tests via CTest

```bash
cd build && ctest --output-on-failure
```

### Individual test tiers

Run from the **project root**, not from `build/`:

```bash
./build/test_unit          # Unit tests (no I/O, no external deps)
./build/test_integration   # Integration tests (requires ctags, creates temp files)
./build/test_e2e           # E2E tests (requires compiled toktoken binary)
```

### Memory checks

```bash
# Build Release first (ASan conflicts with Valgrind)
mkdir -p build-valgrind && cd build-valgrind
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . -j$(nproc)
cd ..

valgrind --leak-check=full --error-exitcode=1 \
    --errors-for-leak-kinds=definite ./build-valgrind/test_unit
valgrind --leak-check=full --error-exitcode=1 \
    --errors-for-leak-kinds=definite ./build-valgrind/test_integration
```

### Adding new tests

1. Create `tests/test_<module>.c` with `TT_TEST()` macros from `test_framework.h`.
2. Add a `void run_<module>_tests(void)` function calling `TT_RUN()` for each test.
3. Register in the appropriate main runner (`test_unit_main.c`, `test_integration_main.c`, or `test_e2e_main.c`).
4. Add the `.c` file to the corresponding `add_executable()` in `CMakeLists.txt`.

## Submitting Changes

### Issues first

Before starting work on a non-trivial change, open an issue to discuss the approach. This avoids wasted effort on changes that don't fit the project direction.

### Branch from `main`

```bash
git checkout main
git pull
git checkout -b feat/my-feature
```

### Commit messages

Follow the format: `type: subject` (max 72 characters).

Types: `feat`, `fix`, `chore`, `test`, `docs`, `perf`.

```
feat: add Zig language support
fix: correct parent_id lookup in hierarchy query
test: add integration tests for search:cooccurrence
docs: update MCP tool count in README
```

### Pull requests

- One logical change per PR
- Fill out the PR template completely
- Ensure all tests pass before submitting
- If you add a new MCP tool, update both CLI and MCP registration plus all related documentation (see PR template checklist)

## Code Structure

```
src/
    <module>.h          Public API (one header per module)
    <module>.c          Implementation
    main.c              CLI entry point
vendor/
    sqlite3/            SQLite3 amalgamation (FTS5 enabled)
    cjson/              cJSON library
    sha256/             SHA-256 implementation
tests/
    test_framework.h    Minimal test framework (no external deps)
    test_helpers.h/c    Fixture paths, tmpdir, file utilities
    test_<module>.c     Unit tests
    test_int_<module>.c Integration tests
    test_e2e_<cmd>.c    E2E tests (run compiled binary via popen)
    fixtures/           Test fixture projects
```

### Naming conventions

- All public symbols use the `tt_` prefix.
- Functions: `tt_<module>_<action>()` (e.g., `tt_store_insert_file()`).
- Types: `tt_<name>_t` (e.g., `tt_symbol_t`, `tt_file_filter_t`).
- Enums: `tt_<name>_e` with `TT_` prefixed values.
- Each `.c`/`.h` pair is one module. No cross-module includes except through public headers.

### Ownership

Memory ownership is documented in headers with `[owns]` annotations:

- `[owns]` on struct fields = the struct's `_free()` function frees them.
- `[caller-frees]` on return values = the caller must free.

## Adding Language Support

Two paths depending on ctags coverage:

### Ctags-based (preferred)

If Universal Ctags already parses the language well:

1. Add extension mappings in `src/language_detector.c`
2. Add normalizer entries in `src/normalizer.c` if ctags uses a non-obvious language name
3. Update `docs/LANGUAGES.md`

### Custom parser

If ctags doesn't support the language or produces poor results:

1. Create `src/parser_<lang>.c` and `src/parser_<lang>.h`
2. Register the parser in `src/index_pipeline.c`
3. Add extension mappings in `src/language_detector.c`
4. Update `docs/LANGUAGES.md`

Custom parsers must handle malformed input gracefully (no crashes on truncated files, unmatched brackets, or binary content).

## Checklist for Contributors

Before submitting changes:

- [ ] Compiles with zero warnings (`-Wall -Wextra -Wpedantic`)
- [ ] All tests pass (`ctest --output-on-failure`)
- [ ] ASan/UBSan clean (Debug build runs tests without errors)
- [ ] Valgrind clean (zero definite leaks, zero errors)
- [ ] New tests added for new functionality
- [ ] No memory leaks in new code (every alloc has a matching free)

## Coding Style

- **Standard:** C11
- **Indentation:** 4 spaces (no tabs)
- **Line endings:** LF
- **Max line length:** 160 characters
- **Braces:** K&R style (opening brace on same line)
- **Comments:** `/* C-style */` for documentation, `//` for inline notes
- **Error handling:** Return codes (0 = success, -1 = error). No exceptions/longjmp.

## License

By contributing, you agree that your contributions will be licensed under the AGPL-3.0, the same license as the project.
