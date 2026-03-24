/*
 * test_unit_main.c -- Main runner for all unit tests.
 */

#include "test_framework.h"

TT_TEST_MAIN_VARS;

/* Declarations for all unit test suite runners */
extern void run_sha256_tests(void);
extern void run_error_tests(void);
extern void run_platform_paths_tests(void);
extern void run_str_util_tests(void);
extern void run_hashmap_tests(void);
extern void run_arena_tests(void);
extern void run_symbol_kind_tests(void);
extern void run_symbol_tests(void);
extern void run_language_detector_tests(void);
extern void run_normalizer_tests(void);
extern void run_line_offsets_tests(void);
extern void run_source_analyzer_tests(void);
extern void run_symbol_scorer_tests(void);
extern void run_summarizer_tests(void);
extern void run_storage_paths_tests(void);
extern void run_config_tests(void);
extern void run_path_validator_tests(void);
extern void run_secret_patterns_tests(void);
extern void run_output_fmt_tests(void);
extern void run_json_output_tests(void);
extern void run_edge_case_tests(void);
extern void run_jinja_strip_tests(void);
extern void run_import_extractor_tests(void);
extern void run_update_check_tests(void);
extern void run_cli_tests(void);
extern void run_mcp_log_tests(void);

int main(void)
{
    TT_SUITE("SHA256");
    run_sha256_tests();

    TT_SUITE("Error");
    run_error_tests();

    TT_SUITE("PlatformPaths");
    run_platform_paths_tests();

    TT_SUITE("StrUtil");
    run_str_util_tests();

    TT_SUITE("Hashmap");
    run_hashmap_tests();

    TT_SUITE("Arena");
    run_arena_tests();

    TT_SUITE("SymbolKind");
    run_symbol_kind_tests();

    TT_SUITE("Symbol");
    run_symbol_tests();

    TT_SUITE("LanguageDetector");
    run_language_detector_tests();

    TT_SUITE("Normalizer");
    run_normalizer_tests();

    TT_SUITE("LineOffsets");
    run_line_offsets_tests();

    TT_SUITE("SourceAnalyzer");
    run_source_analyzer_tests();

    TT_SUITE("SymbolScorer");
    run_symbol_scorer_tests();

    TT_SUITE("Summarizer");
    run_summarizer_tests();

    TT_SUITE("StoragePaths");
    run_storage_paths_tests();

    TT_SUITE("Config");
    run_config_tests();

    TT_SUITE("PathValidator");
    run_path_validator_tests();

    TT_SUITE("SecretPatterns");
    run_secret_patterns_tests();

    TT_SUITE("OutputFmt");
    run_output_fmt_tests();

    TT_SUITE("JsonOutput");
    run_json_output_tests();

    TT_SUITE("EdgeCases");
    run_edge_case_tests();

    TT_SUITE("JinjaStrip");
    run_jinja_strip_tests();

    TT_SUITE("ImportExtractor");
    run_import_extractor_tests();

    TT_SUITE("UpdateCheck");
    run_update_check_tests();

    TT_SUITE("CLI");
    run_cli_tests();

    run_mcp_log_tests();

    TT_SUMMARY();
}
