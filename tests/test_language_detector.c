/*
 * test_language_detector.c -- Unit tests for language_detector module.
 *
 * Ref: tests/Unit/Parser/LanguageDetectorTest.php
 */

#include "test_framework.h"
#include "language_detector.h"

#include <string.h>

TT_TEST(test_lang_detect_common_extensions)
{
    TT_ASSERT_EQ_STR("php",        tt_detect_language("src/App.php"));
    TT_ASSERT_EQ_STR("javascript", tt_detect_language("src/index.js"));
    TT_ASSERT_EQ_STR("typescript", tt_detect_language("src/main.ts"));
    TT_ASSERT_EQ_STR("python",     tt_detect_language("src/app.py"));
    TT_ASSERT_EQ_STR("go",         tt_detect_language("src/main.go"));
    TT_ASSERT_EQ_STR("rust",       tt_detect_language("src/lib.rs"));
    TT_ASSERT_EQ_STR("java",       tt_detect_language("src/App.java"));
    TT_ASSERT_EQ_STR("c",          tt_detect_language("src/main.c"));
    TT_ASSERT_EQ_STR("cpp",        tt_detect_language("src/main.cpp"));
    TT_ASSERT_EQ_STR("bash",       tt_detect_language("src/script.sh"));
    TT_ASSERT_EQ_STR("ruby",       tt_detect_language("src/script.rb"));
}

TT_TEST(test_lang_detect_blade_overrides_php)
{
    TT_ASSERT_EQ_STR("blade", tt_detect_language("views/page.blade.php"));
    TT_ASSERT_EQ_STR("blade", tt_detect_language("resources/views/home.blade.php"));
}

TT_TEST(test_lang_detect_special_frameworks)
{
    TT_ASSERT_EQ_STR("vue",   tt_detect_language("src/App.vue"));
    TT_ASSERT_EQ_STR("nix",   tt_detect_language("src/main.nix"));
    TT_ASSERT_EQ_STR("gleam", tt_detect_language("src/main.gleam"));
}

TT_TEST(test_lang_detect_unknown_extension_passthrough)
{
    TT_ASSERT_EQ_STR("xyz", tt_detect_language("file.xyz"));
}

TT_TEST(test_lang_detect_no_extension)
{
    TT_ASSERT_EQ_STR("unknown", tt_detect_language("Makefile"));
}

TT_TEST(test_lang_detect_double_extensions)
{
    TT_ASSERT_EQ_STR("javascript",  tt_detect_language("app.test.js"));
    TT_ASSERT_EQ_STR("typescript",  tt_detect_language("app.spec.tsx"));
}

TT_TEST(test_lang_detect_alternative_php)
{
    TT_ASSERT_EQ_STR("php", tt_detect_language("page.phtml"));
    TT_ASSERT_EQ_STR("php", tt_detect_language("config.inc"));
}

TT_TEST(test_lang_detect_alternative_js)
{
    TT_ASSERT_EQ_STR("javascript", tt_detect_language("module.mjs"));
    TT_ASSERT_EQ_STR("javascript", tt_detect_language("module.cjs"));
    TT_ASSERT_EQ_STR("javascript", tt_detect_language("Button.jsx"));
}

TT_TEST(test_lang_detect_header_as_cpp)
{
    TT_ASSERT_EQ_STR("cpp", tt_detect_language("include/vector.h"));
    TT_ASSERT_EQ_STR("cpp", tt_detect_language("lib.hpp"));
}

TT_TEST(test_lang_from_extension_direct)
{
    TT_ASSERT_EQ_STR("kotlin",  tt_language_from_extension("kt"));
    TT_ASSERT_EQ_STR("swift",   tt_language_from_extension("swift"));
    TT_ASSERT_EQ_STR("dart",    tt_language_from_extension("dart"));
    TT_ASSERT_EQ_STR("lua",     tt_language_from_extension("lua"));
    TT_ASSERT_EQ_STR("haskell", tt_language_from_extension("hs"));
    TT_ASSERT_EQ_STR("elixir",  tt_language_from_extension("ex"));
    TT_ASSERT_EQ_STR("clojure", tt_language_from_extension("clj"));
    TT_ASSERT_EQ_STR("ejs",     tt_language_from_extension("ejs"));
}

TT_TEST(test_lang_detect_new_extensions)
{
    TT_ASSERT_EQ_STR("objc",     tt_language_from_extension("m"));
    TT_ASSERT_EQ_STR("objcpp",   tt_language_from_extension("mm"));
    TT_ASSERT_EQ_STR("protobuf", tt_language_from_extension("proto"));
    TT_ASSERT_EQ_STR("scala",    tt_language_from_extension("sc"));
    TT_ASSERT_EQ_STR("haskell",  tt_language_from_extension("lhs"));
    TT_ASSERT_EQ_STR("groovy",   tt_language_from_extension("gradle"));
    TT_ASSERT_EQ_STR("css",      tt_language_from_extension("css"));
    TT_ASSERT_EQ_STR("toml",     tt_language_from_extension("toml"));
    TT_ASSERT_EQ_STR("hcl",      tt_language_from_extension("tf"));
    TT_ASSERT_EQ_STR("hcl",      tt_language_from_extension("hcl"));
    TT_ASSERT_EQ_STR("hcl",      tt_language_from_extension("tfvars"));
    TT_ASSERT_EQ_STR("graphql",  tt_language_from_extension("graphql"));
    TT_ASSERT_EQ_STR("graphql",  tt_language_from_extension("gql"));
    TT_ASSERT_EQ_STR("julia",    tt_language_from_extension("jl"));
    TT_ASSERT_EQ_STR("gdscript", tt_language_from_extension("gd"));
    TT_ASSERT_EQ_STR("verse",    tt_language_from_extension("verse"));
    TT_ASSERT_EQ_STR("html",     tt_language_from_extension("html"));
    TT_ASSERT_EQ_STR("html",     tt_language_from_extension("htm"));
}

TT_TEST(test_lang_extra_extensions_override)
{
    const char *keys[] = {"xyz", "abc"};
    const char *langs[] = {"python", "ruby"};
    tt_lang_set_extra_extensions(keys, langs, 2);
    TT_ASSERT_EQ_STR("python", tt_language_from_extension("xyz"));
    TT_ASSERT_EQ_STR("ruby",   tt_language_from_extension("abc"));
    tt_lang_clear_extra_extensions();
    TT_ASSERT_NULL(tt_language_from_extension("xyz"));
}

TT_TEST(test_lang_from_extension_unknown)
{
    TT_ASSERT_NULL(tt_language_from_extension("zzz"));
    TT_ASSERT_NULL(tt_language_from_extension(""));
    TT_ASSERT_NULL(tt_language_from_extension(NULL));
}

TT_TEST(test_lang_detect_markdown)
{
    TT_ASSERT_EQ_STR("markdown", tt_language_from_extension("md"));
    TT_ASSERT_EQ_STR("markdown", tt_language_from_extension("markdown"));
    TT_ASSERT_EQ_STR("markdown", tt_language_from_extension("mdx"));
}

TT_TEST(test_lang_detect_markdown_path)
{
    TT_ASSERT_EQ_STR("markdown", tt_detect_language("docs/README.md"));
    TT_ASSERT_EQ_STR("markdown", tt_detect_language("guide.markdown"));
    TT_ASSERT_EQ_STR("markdown", tt_detect_language("components/Button.mdx"));
}

void run_language_detector_tests(void)
{
    TT_RUN(test_lang_detect_common_extensions);
    TT_RUN(test_lang_detect_blade_overrides_php);
    TT_RUN(test_lang_detect_special_frameworks);
    TT_RUN(test_lang_detect_unknown_extension_passthrough);
    TT_RUN(test_lang_detect_no_extension);
    TT_RUN(test_lang_detect_double_extensions);
    TT_RUN(test_lang_detect_alternative_php);
    TT_RUN(test_lang_detect_alternative_js);
    TT_RUN(test_lang_detect_header_as_cpp);
    TT_RUN(test_lang_from_extension_direct);
    TT_RUN(test_lang_detect_new_extensions);
    TT_RUN(test_lang_extra_extensions_override);
    TT_RUN(test_lang_from_extension_unknown);
    TT_RUN(test_lang_detect_markdown);
    TT_RUN(test_lang_detect_markdown_path);
}
