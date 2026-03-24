cmake_minimum_required(VERSION 3.16)
project(toktoken VERSION 0.4.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Options
option(BUILD_TESTS "Build test suite" ON)
option(TT_STATIC "Build fully static binary" OFF)

# Compiler flags
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-g -O0 -fsanitize=address -fsanitize=undefined)
        add_link_options(-fsanitize=address -fsanitize=undefined)
    else()
        add_compile_options(-O2 -fstack-protector-strong)
    endif()
endif()

# Platform detection
if(WIN32)
    add_definitions(-DTT_PLATFORM_WINDOWS -D_CRT_SECURE_NO_WARNINGS -DUNICODE -D_UNICODE)
else()
    add_definitions(-DTT_PLATFORM_UNIX)
    if(APPLE)
        add_definitions(-DTT_PLATFORM_MACOS)
    else()
        add_definitions(-DTT_PLATFORM_LINUX)
    endif()
endif()

# Architecture detection (for auto-update binary name)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    add_definitions(-DTT_ARCH="x86_64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
    add_definitions(-DTT_ARCH="aarch64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "armv7|armv7l")
    add_definitions(-DTT_ARCH="armv7")
else()
    add_definitions(-DTT_ARCH="unknown")
endif()

# Vendor: sqlite3 - With FTS5 and perf optimizations
add_library(sqlite3 STATIC vendor/sqlite3/sqlite3.c)
target_include_directories(sqlite3 PUBLIC vendor/sqlite3)
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_ENABLE_FTS5
    SQLITE_ENABLE_JSON1
    SQLITE_THREADSAFE=0
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_DQS=0
    SQLITE_LIKE_DOESNT_MATCH_BLOBS
    SQLITE_MAX_EXPR_DEPTH=1000
    SQLITE_OMIT_DECLTYPE
    SQLITE_OMIT_DEPRECATED
    SQLITE_OMIT_PROGRESS_CALLBACK
    SQLITE_OMIT_SHARED_CACHE
    SQLITE_USE_ALLOCA
)
if(NOT WIN32)
    target_link_libraries(sqlite3 PRIVATE m dl)
endif()

# Vendor: cJSON
add_library(cjson STATIC vendor/cjson/cJSON.c)
target_include_directories(cjson PUBLIC vendor/cjson)
target_compile_definitions(cjson PRIVATE CJSON_HIDE_SYMBOLS)
target_compile_options(cjson PRIVATE -Wno-deprecated-declarations)

# Vendor: SHA-256 (kept for binary integrity verification in cmd_update.c)
add_library(sha256 STATIC vendor/sha256/sha256.c)
target_include_directories(sha256 PUBLIC vendor/sha256)

# Vendor: XXHash (fast non-crypto hash for change detection)
add_library(xxhash INTERFACE)
target_include_directories(xxhash INTERFACE vendor/xxhash)

# Vendor: yyjson (fast JSON parser for ctags output)
add_library(yyjson STATIC vendor/yyjson/yyjson.c)
target_include_directories(yyjson PUBLIC vendor/yyjson)
target_compile_definitions(yyjson PRIVATE
    YYJSON_DISABLE_WRITER=1
    YYJSON_DISABLE_UTILS=1
    YYJSON_DISABLE_INCR_READER=1
)

# Vendor: mpsc-queue (lock-free multi-producer single-consumer queue)
add_library(mpsc_queue INTERFACE)
target_include_directories(mpsc_queue INTERFACE vendor/mpsc-queue)

# Vendor: wildmatch (Git-grade glob pattern matching)
add_library(wildmatch STATIC vendor/wildmatch/wildmatch.c)
target_include_directories(wildmatch PUBLIC vendor/wildmatch)

# TokToken core library (everything but main.c, linked both on .exe and tests)
file(GLOB TT_SOURCES "src/*.c")
list(FILTER TT_SOURCES EXCLUDE REGEX ".*/main\\.c$")

add_library(toktoken_core STATIC ${TT_SOURCES})
target_include_directories(toktoken_core PUBLIC src vendor/sqlite3 vendor/cjson vendor/sha256 vendor/xxhash vendor/yyjson vendor/mpsc-queue vendor/wildmatch)
target_compile_definitions(toktoken_core PUBLIC TT_VERSION="${PROJECT_VERSION}")
target_link_libraries(toktoken_core PUBLIC sqlite3 cjson sha256 yyjson wildmatch)
if(NOT WIN32)
    target_link_libraries(toktoken_core PUBLIC pthread m)
endif()

# PCRE2 (system, optional -- skip for static builds to avoid dynamic .so conflict)
if(NOT TT_STATIC)
    find_library(PCRE2_LIB pcre2-8)
    find_path(PCRE2_INCLUDE pcre2.h)
    if(PCRE2_LIB AND PCRE2_INCLUDE)
        target_link_libraries(toktoken_core PUBLIC ${PCRE2_LIB})
        target_include_directories(toktoken_core PUBLIC ${PCRE2_INCLUDE})
        target_compile_definitions(toktoken_core PUBLIC TT_HAVE_PCRE2)
    endif()
endif()

# Main executable
add_executable(toktoken src/main.c)
target_link_libraries(toktoken PRIVATE toktoken_core)
if(TT_STATIC AND NOT APPLE)
    target_link_options(toktoken PRIVATE -static)
endif()

# Tests
if(BUILD_TESTS)
    enable_testing()

    # Unit tests (consolidated)
    add_executable(test_unit
        tests/test_sha256.c
        tests/test_error.c
        tests/test_platform_paths.c
        tests/test_str_util.c
        tests/test_hashmap.c
        tests/test_arena.c
        tests/test_symbol_kind.c
        tests/test_symbol.c
        tests/test_language_detector.c
        tests/test_normalizer.c
        tests/test_line_offsets.c
        tests/test_source_analyzer.c
        tests/test_symbol_scorer.c
        tests/test_summarizer.c
        tests/test_storage_paths.c
        tests/test_config.c
        tests/test_path_validator.c
        tests/test_secret_patterns.c
        tests/test_output_fmt.c
        tests/test_json_output.c
        tests/test_edge_cases.c
        tests/test_jinja_strip.c
        tests/test_import_extractor.c
        tests/test_update_check.c
        tests/test_cli.c
        tests/test_mcp_log.c
        tests/test_helpers.c
        tests/test_unit_main.c
    )
    target_include_directories(test_unit PRIVATE tests)
    target_link_libraries(test_unit PRIVATE toktoken_core)
    add_test(NAME test_unit COMMAND test_unit WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Integration tests (consolidated)
    add_executable(test_integration
        tests/test_int_proc_io.c
        tests/test_int_platform.c
        tests/test_int_storage.c
        tests/test_int_file_filter.c
        tests/test_int_parser_blade.c
        tests/test_int_parser_razor.c
        tests/test_int_parser_twig.c
        tests/test_int_parser_extra.c
        tests/test_int_parsers.c
        tests/test_int_index_store.c
        tests/test_int_search.c
        tests/test_int_commands.c
        tests/test_int_manage.c
        tests/test_int_mcp_server.c
        tests/test_int_github.c
        tests/test_int_savings.c
        tests/test_int_wildmatch.c
        tests/test_int_change_detect.c
        tests/test_int_mtime.c
        tests/test_int_ctags_stream.c
        tests/test_int_pipeline.c
        tests/test_int_v020.c
        tests/test_int_resolve.c
        tests/test_int_multilang.c
        tests/test_helpers.c
        tests/test_integration_main.c
    )
    target_include_directories(test_integration PRIVATE tests)
    target_link_libraries(test_integration PRIVATE toktoken_core)
    add_test(NAME test_integration COMMAND test_integration WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # E2E tests (consolidated)
    add_executable(test_e2e
        tests/test_e2e_index.c
        tests/test_e2e_search.c
        tests/test_e2e_inspect.c
        tests/test_e2e_manage.c
        tests/test_e2e_errors.c
        tests/test_e2e_full_index.c
        tests/test_e2e_full_search.c
        tests/test_e2e_full_inspect.c
        tests/test_e2e_full_find.c
        tests/test_e2e_full_manage.c
        tests/test_e2e_full_langs.c
        tests/test_e2e_full_imports.c
        tests/test_helpers.c
        tests/test_e2e_main.c
    )
    target_include_directories(test_e2e PRIVATE tests)
    target_link_libraries(test_e2e PRIVATE toktoken_core)
    add_test(NAME test_e2e COMMAND test_e2e WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# Install
install(TARGETS toktoken RUNTIME DESTINATION bin)
