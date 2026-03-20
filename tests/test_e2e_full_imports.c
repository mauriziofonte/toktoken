/*
 * test_e2e_full_imports.c -- Per-language import chain validation.
 *
 * Verifies find:importers correctly identifies the importing file for each
 * language family where import resolution works.
 *
 * Import resolution strategies tested:
 *   - Relative path: TS, JS, Python, Ruby (./module)
 *   - Same-directory: C, C++ (#include "header.h")
 *   - Module/mod: Rust (mod config), Haskell (import Module)
 *   - Suffix matching: Kotlin (import data.Store), Scala (import service.Processor)
 *   - Progressive prefix stripping: PHP (App\Service\Class), Java (com.pkg.Class)
 *   - Directory matching: Go (myapp/handlers), C# (using Namespace)
 *   - Cycle detection: Python (mutual imports)
 */

#include "test_e2e_full_helpers.h"

/* --- Relative path resolution --- */

/* TypeScript chain: app.ts -> service.ts -> types.ts */
E2E_IMPORTERS_LANG(test_import_ts_types,
                   "src/ts/types.ts", 1, "service.ts")

/* JavaScript chain: index.js -> router.js -> middleware.js */
E2E_IMPORTERS_LANG(test_import_js_middleware,
                   "src/js/middleware.js", 1, "router.js")

/* Python chain: main.py -> analyzer.py -> models.py */
E2E_IMPORTERS_LANG(test_import_py_models,
                   "src/python/models.py", 1, "analyzer.py")

/* Ruby chain: app.rb -> helpers.rb */
E2E_IMPORTERS_LANG(test_import_ruby_helpers,
                   "src/ruby/helpers.rb", 1, "app.rb")

/* --- Same-directory resolution (C/C++ includes) --- */

/* C chain: main.c -> engine.h, engine.c -> engine.h */
E2E_IMPORTERS_LANG(test_import_c_engine_h,
                   "src/c/engine.h", 2, "main.c")

/* C++ chain: app.cpp -> utils.hpp */
E2E_IMPORTERS_LANG(test_import_cpp_utils,
                   "src/cpp/utils.hpp", 1, "app.cpp")

/* --- Module/mod resolution --- */

/* Rust chain: main.rs -> config.rs */
E2E_IMPORTERS_LANG(test_import_rust_config,
                   "src/rust/config.rs", 1, "main.rs")

/* Haskell chain: Main.hs -> Lib/Parser.hs */
E2E_IMPORTERS_LANG(test_import_haskell_parser,
                   "src/haskell/Lib/Parser.hs", 1, "Main.hs")

/* --- Suffix matching resolution --- */

/* Kotlin chain: Main.kt -> data/Store.kt */
E2E_IMPORTERS_LANG(test_import_kotlin_store,
                   "src/kotlin/data/Store.kt", 1, "Main.kt")

/* Scala chain: App.scala -> service/Processor.scala */
E2E_IMPORTERS_LANG(test_import_scala_processor,
                   "src/scala/service/Processor.scala", 1, "App.scala")

/* --- Progressive prefix stripping (namespace/package) --- */

/* PHP chain: Controller.php -> Service/UserService.php -> Data/Repository.php */
E2E_IMPORTERS_LANG(test_import_php_userservice,
                   "src/php/Service/UserService.php", 1, "Controller.php")
E2E_IMPORTERS_LANG(test_import_php_repository,
                   "src/php/Data/Repository.php", 1, "UserService.php")

/* Java chain: Application.java -> service/OrderService.java */
E2E_IMPORTERS_LANG(test_import_java_orderservice,
                   "src/java/service/OrderService.java", 1, "Application.java")

/* --- Directory matching (module path / namespace) --- */

/* Go chain: main.go -> handlers/api.go */
E2E_IMPORTERS_LANG(test_import_go_handlers,
                   "src/go/handlers/api.go", 1, "main.go")

/* C# chain: Program.cs -> Services/DataService.cs */
E2E_IMPORTERS_LANG(test_import_csharp_dataservice,
                   "src/csharp/Services/DataService.cs", 1, "Program.cs")

/* --- Cycle detection --- */

/* Cycle: cycle_a.py <-> cycle_b.py */
E2E_IMPORTERS_LANG(test_import_cycle_a,
                   "cycle_a.py", 1, "cycle_b.py")
E2E_IMPORTERS_LANG(test_import_cycle_b,
                   "cycle_b.py", 1, "cycle_a.py")

void run_e2e_full_imports_tests(void)
{
    /* Relative path */
    TT_RUN(test_import_ts_types);
    TT_RUN(test_import_js_middleware);
    TT_RUN(test_import_py_models);
    TT_RUN(test_import_ruby_helpers);

    /* Same-directory (C/C++) */
    TT_RUN(test_import_c_engine_h);
    TT_RUN(test_import_cpp_utils);

    /* Module/mod */
    TT_RUN(test_import_rust_config);
    TT_RUN(test_import_haskell_parser);

    /* Suffix matching */
    TT_RUN(test_import_kotlin_store);
    TT_RUN(test_import_scala_processor);

    /* Progressive prefix stripping */
    TT_RUN(test_import_php_userservice);
    TT_RUN(test_import_php_repository);
    TT_RUN(test_import_java_orderservice);

    /* Directory matching */
    TT_RUN(test_import_go_handlers);
    TT_RUN(test_import_csharp_dataservice);

    /* Cycles */
    TT_RUN(test_import_cycle_a);
    TT_RUN(test_import_cycle_b);
}
