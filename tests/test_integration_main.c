/*
 * test_integration_main.c -- Main runner for all integration tests.
 */

#include "test_framework.h"

TT_TEST_MAIN_VARS;

extern void run_int_platform_tests(void);
extern void run_int_storage_tests(void);
extern void run_int_file_filter_tests(void);
extern void run_int_parser_blade_tests(void);
extern void run_int_parser_razor_tests(void);
extern void run_int_parser_twig_tests(void);
extern void run_int_parsers_tests(void);
extern void run_int_parser_extra_tests(void);
extern void run_int_index_store_tests(void);
extern void run_int_search_tests(void);
extern void run_int_commands_tests(void);
extern void run_int_manage_tests(void);
extern void run_int_mcp_server_tests(void);
extern void run_int_github_tests(void);
extern void run_int_savings_tests(void);
extern void run_int_proc_io_tests(void);
extern void run_int_wildmatch_tests(void);
extern void run_int_change_detect_tests(void);
extern void run_int_mtime_tests(void);
extern void run_int_ctags_stream_tests(void);
extern void run_int_pipeline_tests(void);
extern void run_int_v020_tests(void);
extern void run_int_resolve_tests(void);
extern void run_int_multilang_tests(void);

int main(void)
{
    TT_SUITE("Platform (integration)");
    run_int_platform_tests();

    TT_SUITE("Process I/O & Signal Handling (integration)");
    run_int_proc_io_tests();

    TT_SUITE("Storage (integration)");
    run_int_storage_tests();

    TT_SUITE("FileFilter (integration)");
    run_int_file_filter_tests();

    TT_SUITE("BladeParser (integration)");
    run_int_parser_blade_tests();

    TT_SUITE("RazorParser (integration)");
    run_int_parser_razor_tests();

    TT_SUITE("TwigParser (integration)");
    run_int_parser_twig_tests();

    TT_SUITE("Parsers (integration)");
    run_int_parsers_tests();

    TT_SUITE("Parsers Extra (integration)");
    run_int_parser_extra_tests();

    TT_SUITE("IndexStore (integration)");
    run_int_index_store_tests();

    TT_SUITE("Search (integration)");
    run_int_search_tests();

    TT_SUITE("Commands (integration)");
    run_int_commands_tests();

    TT_SUITE("Manage (integration)");
    run_int_manage_tests();

    TT_SUITE("MCPServer (integration)");
    run_int_mcp_server_tests();

    TT_SUITE("GitHub (integration)");
    run_int_github_tests();

    TT_SUITE("Savings (integration)");
    run_int_savings_tests();

    TT_SUITE("Wildmatch (integration)");
    run_int_wildmatch_tests();

    TT_SUITE("ChangeDetect (integration)");
    run_int_change_detect_tests();

    TT_SUITE("Mtime Portability (integration)");
    run_int_mtime_tests();

    TT_SUITE("CtagsStream (integration)");
    run_int_ctags_stream_tests();

    TT_SUITE("Pipeline (integration)");
    run_int_pipeline_tests();

    TT_SUITE("v0.2.0 Features (integration)");
    run_int_v020_tests();

    TT_SUITE("Import Resolution (integration)");
    run_int_resolve_tests();

    TT_SUITE("Multi-Language (integration)");
    run_int_multilang_tests();

    TT_SUMMARY();
}
