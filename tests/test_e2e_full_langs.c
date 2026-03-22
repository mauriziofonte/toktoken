/*
 * test_e2e_full_langs.c -- Per-language symbol search + outline tests.
 *
 * Validates every language with ctags/custom parser support is correctly
 * indexed, searchable, and outlineable.
 *
 * Languages excluded (no ctags parser on this system, no custom parser):
 * dart, scala, swift.
 */

#include "test_e2e_full_helpers.h"

/* ================================================================
 * SEARCH tests: verify a known symbol exists for each language
 *
 * ctags maps some kinds differently:
 *   - Rust struct -> "class"
 *   - SQL table -> "variable" (CREATE TABLE)
 *
 * Custom parsers use their own kind names:
 *   - HCL resource -> "class"
 *   - GraphQL type -> "class"
 *   - Nix attribute -> "variable"
 *   - Elixir module -> "namespace"
 * ================================================================ */

/* ctags languages */
E2E_SEARCH_LANG(test_lang_search_php,
                "Controller", "class", "php/Controller")
E2E_SEARCH_LANG(test_lang_search_typescript,
                "DataService", "class", "ts/service")
E2E_SEARCH_LANG(test_lang_search_javascript,
                "createRouter", "function", "js/router")
E2E_SEARCH_LANG(test_lang_search_python,
                "Analyzer", "class", "python/analyzer")
E2E_SEARCH_LANG(test_lang_search_go,
                "HandleGet", "function", "go/handlers")
E2E_SEARCH_LANG(test_lang_search_rust,
                "AppConfig", "class", "rust/config")
E2E_SEARCH_LANG(test_lang_search_c,
                "process_data", "function", "c/engine")
E2E_SEARCH_LANG(test_lang_search_cpp,
                "App", "class", "cpp/app")
E2E_SEARCH_LANG(test_lang_search_java,
                "OrderService", "class", "java/service")
E2E_SEARCH_LANG(test_lang_search_kotlin,
                "Store", "class", "kotlin/data")
E2E_SEARCH_LANG(test_lang_search_ruby,
                "format_output", "function", "ruby/helpers")
E2E_SEARCH_LANG(test_lang_search_csharp,
                "DataService", "class", "csharp/Services")
E2E_SEARCH_LANG(test_lang_search_haskell,
                "parseExpression", "function", "haskell/Lib")
E2E_SEARCH_LANG(test_lang_search_r,
                "analyze_data", "function", "r/analysis")
E2E_SEARCH_LANG(test_lang_search_lua,
                "update", "function", "lua/game")
E2E_SEARCH_LANG(test_lang_search_perl,
                "parse_input", "function", "perl/processor")
E2E_SEARCH_LANG(test_lang_search_bash,
                "deploy_app", "function", "bash/deploy")
E2E_SEARCH_LANG(test_lang_search_sql,
                "orders", "variable", "sql/schema")
E2E_SEARCH_LANG(test_lang_search_elixir,
                "Worker", "namespace", "elixir/worker")

/* Custom parser languages */
E2E_SEARCH_LANG(test_lang_search_vue,
                "fetchData", "function", "vue/Dashboard")
E2E_SEARCH_LANG(test_lang_search_gleam,
                "start", "function", "gleam/server")
E2E_SEARCH_LANG(test_lang_search_gdscript,
                "attack", "function", "gdscript/enemy")
E2E_SEARCH_LANG(test_lang_search_hcl,
                "aws_instance.web", "class", "hcl/infrastructure")
E2E_SEARCH_LANG(test_lang_search_nix,
                "pname", "variable", "nix/package")
E2E_SEARCH_LANG(test_lang_search_graphql,
                "User", "class", "graphql/schema")
E2E_SEARCH_LANG(test_lang_search_julia,
                "simulate", "function", "julia/simulation")

/* ================================================================
 * OUTLINE tests: verify each file produces a reasonable symbol count
 *
 * ctags nests methods under classes, so the top-level count may be
 * lower than expected (e.g. Python class = 1 top-level symbol).
 * Custom parsers typically produce flat symbol lists.
 * ================================================================ */

/* ctags languages */
E2E_OUTLINE_LANG(test_lang_outline_php,
                 "src/php/Controller.php", 1)
E2E_OUTLINE_LANG(test_lang_outline_typescript,
                 "src/ts/app.ts", 2)
E2E_OUTLINE_LANG(test_lang_outline_javascript,
                 "src/js/router.js", 3)
E2E_OUTLINE_LANG(test_lang_outline_python,
                 "src/python/analyzer.py", 1)
E2E_OUTLINE_LANG(test_lang_outline_go,
                 "src/go/main.go", 1)
E2E_OUTLINE_LANG(test_lang_outline_rust,
                 "src/rust/main.rs", 3)
E2E_OUTLINE_LANG(test_lang_outline_c,
                 "src/c/engine.c", 3)
E2E_OUTLINE_LANG(test_lang_outline_cpp,
                 "src/cpp/app.cpp", 2)
E2E_OUTLINE_LANG(test_lang_outline_java,
                 "src/java/Application.java", 2)
E2E_OUTLINE_LANG(test_lang_outline_kotlin,
                 "src/kotlin/Main.kt", 3)
E2E_OUTLINE_LANG(test_lang_outline_ruby,
                 "src/ruby/app.rb", 1)
E2E_OUTLINE_LANG(test_lang_outline_csharp,
                 "src/csharp/Program.cs", 1)
E2E_OUTLINE_LANG(test_lang_outline_haskell,
                 "src/haskell/Main.hs", 2)
E2E_OUTLINE_LANG(test_lang_outline_r,
                 "src/r/analysis.r", 2)
E2E_OUTLINE_LANG(test_lang_outline_lua,
                 "src/lua/game.lua", 3)
E2E_OUTLINE_LANG(test_lang_outline_perl,
                 "src/perl/processor.pl", 3)
E2E_OUTLINE_LANG(test_lang_outline_bash,
                 "src/bash/deploy.sh", 3)
E2E_OUTLINE_LANG(test_lang_outline_sql,
                 "src/sql/schema.sql", 3)
E2E_OUTLINE_LANG(test_lang_outline_elixir,
                 "src/elixir/worker.ex", 1)

/* Custom parser languages */
E2E_OUTLINE_LANG(test_lang_outline_vue,
                 "src/vue/Dashboard.vue", 5)
E2E_OUTLINE_LANG(test_lang_outline_gleam,
                 "src/gleam/server.gleam", 5)
E2E_OUTLINE_LANG(test_lang_outline_gdscript,
                 "src/gdscript/enemy.gd", 5)
E2E_OUTLINE_LANG(test_lang_outline_hcl,
                 "src/hcl/infrastructure.tf", 5)
E2E_OUTLINE_LANG(test_lang_outline_nix,
                 "src/nix/package.nix", 5)
E2E_OUTLINE_LANG(test_lang_outline_graphql,
                 "src/graphql/schema.graphql", 5)
E2E_OUTLINE_LANG(test_lang_outline_julia,
                 "src/julia/simulation.jl", 5)

/* Markdown (ctags) */
E2E_SEARCH_LANG(test_lang_search_markdown,
                "Installation", "section", "markdown/guide")
E2E_OUTLINE_LANG(test_lang_outline_markdown,
                 "src/markdown/guide.md", 3)

void run_e2e_full_langs_tests(void)
{
    /* Search tests -- ctags languages */
    TT_RUN(test_lang_search_php);
    TT_RUN(test_lang_search_typescript);
    TT_RUN(test_lang_search_javascript);
    TT_RUN(test_lang_search_python);
    TT_RUN(test_lang_search_go);
    TT_RUN(test_lang_search_rust);
    TT_RUN(test_lang_search_c);
    TT_RUN(test_lang_search_cpp);
    TT_RUN(test_lang_search_java);
    TT_RUN(test_lang_search_kotlin);
    TT_RUN(test_lang_search_ruby);
    TT_RUN(test_lang_search_csharp);
    TT_RUN(test_lang_search_haskell);
    TT_RUN(test_lang_search_r);
    TT_RUN(test_lang_search_lua);
    TT_RUN(test_lang_search_perl);
    TT_RUN(test_lang_search_bash);
    TT_RUN(test_lang_search_sql);
    TT_RUN(test_lang_search_elixir);

    /* Search tests -- custom parser languages */
    TT_RUN(test_lang_search_vue);
    TT_RUN(test_lang_search_gleam);
    TT_RUN(test_lang_search_gdscript);
    TT_RUN(test_lang_search_hcl);
    TT_RUN(test_lang_search_nix);
    TT_RUN(test_lang_search_graphql);
    TT_RUN(test_lang_search_julia);

    /* Outline tests -- ctags languages */
    TT_RUN(test_lang_outline_php);
    TT_RUN(test_lang_outline_typescript);
    TT_RUN(test_lang_outline_javascript);
    TT_RUN(test_lang_outline_python);
    TT_RUN(test_lang_outline_go);
    TT_RUN(test_lang_outline_rust);
    TT_RUN(test_lang_outline_c);
    TT_RUN(test_lang_outline_cpp);
    TT_RUN(test_lang_outline_java);
    TT_RUN(test_lang_outline_kotlin);
    TT_RUN(test_lang_outline_ruby);
    TT_RUN(test_lang_outline_csharp);
    TT_RUN(test_lang_outline_haskell);
    TT_RUN(test_lang_outline_r);
    TT_RUN(test_lang_outline_lua);
    TT_RUN(test_lang_outline_perl);
    TT_RUN(test_lang_outline_bash);
    TT_RUN(test_lang_outline_sql);
    TT_RUN(test_lang_outline_elixir);

    /* Outline tests -- custom parser languages */
    TT_RUN(test_lang_outline_vue);
    TT_RUN(test_lang_outline_gleam);
    TT_RUN(test_lang_outline_gdscript);
    TT_RUN(test_lang_outline_hcl);
    TT_RUN(test_lang_outline_nix);
    TT_RUN(test_lang_outline_graphql);
    TT_RUN(test_lang_outline_julia);

    /* Markdown */
    TT_RUN(test_lang_search_markdown);
    TT_RUN(test_lang_outline_markdown);
}
