/*
 * test_symbol_kind.c -- Unit tests for symbol_kind module.
 */

#include "test_framework.h"
#include "symbol_kind.h"

#include <string.h>

/* ---- fromCtags mapping tests ---- */

TT_TEST(test_symbol_kind_from_ctags_identity)
{
    TT_ASSERT_EQ_INT(TT_KIND_CLASS,     tt_kind_from_ctags("class"));
    TT_ASSERT_EQ_INT(TT_KIND_INTERFACE, tt_kind_from_ctags("interface"));
    TT_ASSERT_EQ_INT(TT_KIND_TRAIT,     tt_kind_from_ctags("trait"));
    TT_ASSERT_EQ_INT(TT_KIND_ENUM,      tt_kind_from_ctags("enum"));
    TT_ASSERT_EQ_INT(TT_KIND_FUNCTION,  tt_kind_from_ctags("function"));
    TT_ASSERT_EQ_INT(TT_KIND_METHOD,    tt_kind_from_ctags("method"));
    TT_ASSERT_EQ_INT(TT_KIND_CONSTANT,  tt_kind_from_ctags("constant"));
    TT_ASSERT_EQ_INT(TT_KIND_PROPERTY,  tt_kind_from_ctags("property"));
    TT_ASSERT_EQ_INT(TT_KIND_VARIABLE,  tt_kind_from_ctags("variable"));
    TT_ASSERT_EQ_INT(TT_KIND_NAMESPACE, tt_kind_from_ctags("namespace"));
    TT_ASSERT_EQ_INT(TT_KIND_TYPE,      tt_kind_from_ctags("type"));
    TT_ASSERT_EQ_INT(TT_KIND_DIRECTIVE, tt_kind_from_ctags("directive"));
}

TT_TEST(test_symbol_kind_from_ctags_aliases)
{
    TT_ASSERT_EQ_INT(TT_KIND_CLASS,     tt_kind_from_ctags("struct"));
    TT_ASSERT_EQ_INT(TT_KIND_FUNCTION,  tt_kind_from_ctags("func"));
    TT_ASSERT_EQ_INT(TT_KIND_FUNCTION,  tt_kind_from_ctags("generator"));
    TT_ASSERT_EQ_INT(TT_KIND_FUNCTION,  tt_kind_from_ctags("macro"));
    TT_ASSERT_EQ_INT(TT_KIND_FUNCTION,  tt_kind_from_ctags("prototype"));
    TT_ASSERT_EQ_INT(TT_KIND_METHOD,    tt_kind_from_ctags("member"));
    TT_ASSERT_EQ_INT(TT_KIND_CONSTANT,  tt_kind_from_ctags("define"));
    TT_ASSERT_EQ_INT(TT_KIND_CONSTANT,  tt_kind_from_ctags("enumConstant"));
    TT_ASSERT_EQ_INT(TT_KIND_CONSTANT,  tt_kind_from_ctags("enumerator"));
    TT_ASSERT_EQ_INT(TT_KIND_PROPERTY,  tt_kind_from_ctags("field"));
    TT_ASSERT_EQ_INT(TT_KIND_VARIABLE,  tt_kind_from_ctags("externvar"));
    TT_ASSERT_EQ_INT(TT_KIND_NAMESPACE, tt_kind_from_ctags("module"));
    TT_ASSERT_EQ_INT(TT_KIND_NAMESPACE, tt_kind_from_ctags("packageName"));
    TT_ASSERT_EQ_INT(TT_KIND_TYPE,      tt_kind_from_ctags("typedef"));
    TT_ASSERT_EQ_INT(TT_KIND_TYPE,      tt_kind_from_ctags("union"));
}

TT_TEST(test_symbol_kind_unknown_falls_to_variable)
{
    TT_ASSERT_EQ_INT(TT_KIND_VARIABLE, tt_kind_from_ctags("totally_unknown"));
}

TT_TEST(test_symbol_kind_empty_string_falls_to_variable)
{
    TT_ASSERT_EQ_INT(TT_KIND_VARIABLE, tt_kind_from_ctags(""));
    TT_ASSERT_EQ_INT(TT_KIND_VARIABLE, tt_kind_from_ctags(NULL));
}

TT_TEST(test_symbol_kind_labels)
{
    TT_ASSERT_EQ_STR("Class",           tt_kind_label(TT_KIND_CLASS));
    TT_ASSERT_EQ_STR("Method",          tt_kind_label(TT_KIND_METHOD));
    TT_ASSERT_EQ_STR("Function",        tt_kind_label(TT_KIND_FUNCTION));
    TT_ASSERT_EQ_STR("Type definition", tt_kind_label(TT_KIND_TYPE));
    TT_ASSERT_EQ_STR("Interface",       tt_kind_label(TT_KIND_INTERFACE));
    TT_ASSERT_EQ_STR("Trait",           tt_kind_label(TT_KIND_TRAIT));
    TT_ASSERT_EQ_STR("Enum",            tt_kind_label(TT_KIND_ENUM));
    TT_ASSERT_EQ_STR("Constant",        tt_kind_label(TT_KIND_CONSTANT));
    TT_ASSERT_EQ_STR("Property",        tt_kind_label(TT_KIND_PROPERTY));
    TT_ASSERT_EQ_STR("Variable",        tt_kind_label(TT_KIND_VARIABLE));
    TT_ASSERT_EQ_STR("Namespace",       tt_kind_label(TT_KIND_NAMESPACE));
    TT_ASSERT_EQ_STR("Directive",       tt_kind_label(TT_KIND_DIRECTIVE));
}

TT_TEST(test_symbol_kind_labels_all_nonempty)
{
    for (int i = 0; i < TT_KIND_COUNT; i++) {
        const char *label = tt_kind_label((tt_symbol_kind_e)i);
        TT_ASSERT_NOT_NULL(label);
        TT_ASSERT(label[0] != '\0', "label should be non-empty");
    }
}

TT_TEST(test_symbol_kind_is_single_line)
{
    TT_ASSERT_TRUE(tt_kind_is_single_line(TT_KIND_CONSTANT));
    TT_ASSERT_TRUE(tt_kind_is_single_line(TT_KIND_VARIABLE));
    TT_ASSERT_TRUE(tt_kind_is_single_line(TT_KIND_PROPERTY));
    TT_ASSERT_TRUE(tt_kind_is_single_line(TT_KIND_NAMESPACE));
    TT_ASSERT_TRUE(tt_kind_is_single_line(TT_KIND_TYPE));
    TT_ASSERT_TRUE(tt_kind_is_single_line(TT_KIND_DIRECTIVE));

    TT_ASSERT_FALSE(tt_kind_is_single_line(TT_KIND_CLASS));
    TT_ASSERT_FALSE(tt_kind_is_single_line(TT_KIND_FUNCTION));
    TT_ASSERT_FALSE(tt_kind_is_single_line(TT_KIND_METHOD));
    TT_ASSERT_FALSE(tt_kind_is_single_line(TT_KIND_INTERFACE));
    TT_ASSERT_FALSE(tt_kind_is_single_line(TT_KIND_TRAIT));
    TT_ASSERT_FALSE(tt_kind_is_single_line(TT_KIND_ENUM));
}

TT_TEST(test_symbol_kind_to_str_stable_values)
{
    const char *expected[] = {
        "class", "interface", "trait", "enum", "function", "method",
        "constant", "property", "variable", "namespace", "type", "directive",
        "chapter", "section", "subsection"
    };
    for (int i = 0; i < TT_KIND_COUNT; i++) {
        TT_ASSERT_EQ_STR(expected[i], tt_kind_to_str((tt_symbol_kind_e)i));
    }
}

TT_TEST(test_symbol_kind_is_valid)
{
    /* All canonical kinds are valid */
    TT_ASSERT_TRUE(tt_kind_is_valid("class"));
    TT_ASSERT_TRUE(tt_kind_is_valid("function"));
    TT_ASSERT_TRUE(tt_kind_is_valid("method"));
    TT_ASSERT_TRUE(tt_kind_is_valid("constant"));
    TT_ASSERT_TRUE(tt_kind_is_valid("variable"));
    TT_ASSERT_TRUE(tt_kind_is_valid("property"));
    TT_ASSERT_TRUE(tt_kind_is_valid("type"));
    TT_ASSERT_TRUE(tt_kind_is_valid("enum"));
    TT_ASSERT_TRUE(tt_kind_is_valid("interface"));
    TT_ASSERT_TRUE(tt_kind_is_valid("trait"));
    TT_ASSERT_TRUE(tt_kind_is_valid("namespace"));
    TT_ASSERT_TRUE(tt_kind_is_valid("directive"));

    /* Case insensitive */
    TT_ASSERT_TRUE(tt_kind_is_valid("Function"));
    TT_ASSERT_TRUE(tt_kind_is_valid("CLASS"));

    /* Invalid kinds */
    TT_ASSERT_FALSE(tt_kind_is_valid("struct"));
    TT_ASSERT_FALSE(tt_kind_is_valid("macro"));
    TT_ASSERT_FALSE(tt_kind_is_valid("foobar"));
    TT_ASSERT_FALSE(tt_kind_is_valid(""));
    TT_ASSERT_FALSE(tt_kind_is_valid(NULL));
}

/* ---- Documentation kind tests ---- */

TT_TEST(test_symbol_kind_from_ctags_doc_kinds)
{
    /* Identity mappings */
    TT_ASSERT_EQ_INT(TT_KIND_CHAPTER,    tt_kind_from_ctags("chapter"));
    TT_ASSERT_EQ_INT(TT_KIND_SECTION,    tt_kind_from_ctags("section"));
    TT_ASSERT_EQ_INT(TT_KIND_SUBSECTION, tt_kind_from_ctags("subsection"));

    /* Alias collapsing: H4-H6 all map to subsection */
    TT_ASSERT_EQ_INT(TT_KIND_SUBSECTION, tt_kind_from_ctags("subsubsection"));
    TT_ASSERT_EQ_INT(TT_KIND_SUBSECTION, tt_kind_from_ctags("l4subsection"));
    TT_ASSERT_EQ_INT(TT_KIND_SUBSECTION, tt_kind_from_ctags("l5subsection"));
}

TT_TEST(test_symbol_kind_doc_labels)
{
    TT_ASSERT_EQ_STR("Chapter",    tt_kind_label(TT_KIND_CHAPTER));
    TT_ASSERT_EQ_STR("Section",    tt_kind_label(TT_KIND_SECTION));
    TT_ASSERT_EQ_STR("Subsection", tt_kind_label(TT_KIND_SUBSECTION));
}

TT_TEST(test_symbol_kind_doc_kinds_are_multiline)
{
    TT_ASSERT_FALSE(tt_kind_is_single_line(TT_KIND_CHAPTER));
    TT_ASSERT_FALSE(tt_kind_is_single_line(TT_KIND_SECTION));
    TT_ASSERT_FALSE(tt_kind_is_single_line(TT_KIND_SUBSECTION));
}

TT_TEST(test_symbol_kind_doc_kinds_are_valid)
{
    /* Canonical names are valid */
    TT_ASSERT_TRUE(tt_kind_is_valid("chapter"));
    TT_ASSERT_TRUE(tt_kind_is_valid("section"));
    TT_ASSERT_TRUE(tt_kind_is_valid("subsection"));

    /* Case insensitive */
    TT_ASSERT_TRUE(tt_kind_is_valid("Chapter"));
    TT_ASSERT_TRUE(tt_kind_is_valid("SECTION"));

    /* Aliases are NOT valid canonical kind names */
    TT_ASSERT_FALSE(tt_kind_is_valid("subsubsection"));
    TT_ASSERT_FALSE(tt_kind_is_valid("l4subsection"));
    TT_ASSERT_FALSE(tt_kind_is_valid("l5subsection"));
}

TT_TEST(test_symbol_kind_count_is_15)
{
    TT_ASSERT_EQ_INT(15, TT_KIND_COUNT);
}

void run_symbol_kind_tests(void)
{
    TT_RUN(test_symbol_kind_from_ctags_identity);
    TT_RUN(test_symbol_kind_from_ctags_aliases);
    TT_RUN(test_symbol_kind_unknown_falls_to_variable);
    TT_RUN(test_symbol_kind_empty_string_falls_to_variable);
    TT_RUN(test_symbol_kind_labels);
    TT_RUN(test_symbol_kind_labels_all_nonempty);
    TT_RUN(test_symbol_kind_is_single_line);
    TT_RUN(test_symbol_kind_to_str_stable_values);
    TT_RUN(test_symbol_kind_is_valid);
    TT_RUN(test_symbol_kind_from_ctags_doc_kinds);
    TT_RUN(test_symbol_kind_doc_labels);
    TT_RUN(test_symbol_kind_doc_kinds_are_multiline);
    TT_RUN(test_symbol_kind_doc_kinds_are_valid);
    TT_RUN(test_symbol_kind_count_is_15);
}
