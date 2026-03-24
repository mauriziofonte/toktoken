/*
 * test_str_util.c -- Unit tests for str_util module (strbuf, array,
 * string functions, UTF-8).
 */

#include "test_framework.h"
#include "str_util.h"

/* ---- strbuf ---- */

TT_TEST(test_strbuf_append_and_format)
{
    tt_strbuf_t sb;
    tt_strbuf_init(&sb);

    tt_strbuf_append_str(&sb, "Hello");
    tt_strbuf_append_char(&sb, ' ');
    tt_strbuf_appendf(&sb, "World %d", 42);

    TT_ASSERT_EQ_INT((int)sb.len, 14);
    TT_ASSERT_EQ_STR(sb.data, "Hello World 42");

    tt_strbuf_reset(&sb);
    TT_ASSERT_EQ_INT((int)sb.len, 0);

    tt_strbuf_append_str(&sb, "new");
    char *detached = tt_strbuf_detach(&sb);
    TT_ASSERT_EQ_STR(detached, "new");
    TT_ASSERT_NULL(sb.data);
    free(detached);

    tt_strbuf_free(&sb);
}

/* ---- array ---- */

TT_TEST(test_array_push_pop)
{
    tt_array_t a;
    tt_array_init(&a);

    char *s1 = tt_strdup("one");
    char *s2 = tt_strdup("two");
    char *s3 = tt_strdup("three");

    tt_array_push(&a, s1);
    tt_array_push(&a, s2);
    tt_array_push(&a, s3);

    TT_ASSERT_EQ_INT((int)a.len, 3);
    TT_ASSERT_EQ_STR((char *)a.items[0], "one");
    TT_ASSERT_EQ_STR((char *)a.items[2], "three");

    char *popped = tt_array_pop(&a);
    TT_ASSERT_EQ_STR(popped, "three");
    TT_ASSERT_EQ_INT((int)a.len, 2);
    free(popped);

    tt_array_free_items(&a);
}

/* ---- strdup / strndup / tolower ---- */

TT_TEST(test_strdup_funcs)
{
    char *d = tt_strdup("Hello");
    TT_ASSERT_EQ_STR(d, "Hello");
    free(d);

    d = tt_strndup("Hello World", 5);
    TT_ASSERT_EQ_STR(d, "Hello");
    free(d);

    d = tt_str_tolower("Hello WORLD");
    TT_ASSERT_EQ_STR(d, "hello world");
    free(d);

    TT_ASSERT_NULL(tt_strdup(NULL));
}

/* ---- predicates ---- */

TT_TEST(test_str_predicates)
{
    TT_ASSERT_TRUE(tt_str_starts_with("hello world", "hello"));
    TT_ASSERT_FALSE(tt_str_starts_with("hello", "hello world"));
    TT_ASSERT_TRUE(tt_str_ends_with("hello.php", ".php"));
    TT_ASSERT_FALSE(tt_str_ends_with("hello.js", ".php"));
    TT_ASSERT_TRUE(tt_str_contains("hello world", "lo wo"));
    TT_ASSERT_FALSE(tt_str_contains("hello", "xyz"));
    TT_ASSERT_TRUE(tt_str_contains_ci("Hello World", "HELLO"));
}

/* ---- split ---- */

TT_TEST(test_str_split)
{
    int count = 0;
    char **parts = tt_str_split("a,b,c", ',', &count);
    TT_ASSERT_EQ_INT(count, 3);
    TT_ASSERT_EQ_STR(parts[0], "a");
    TT_ASSERT_EQ_STR(parts[1], "b");
    TT_ASSERT_EQ_STR(parts[2], "c");
    TT_ASSERT_NULL(parts[3]);
    tt_str_split_free(parts);
}

/* ---- split_words ---- */

TT_TEST(test_str_split_words)
{
    int count = 0;

    char **w = tt_str_split_words("hello_world.test case", &count);
    TT_ASSERT_EQ_INT(count, 4);
    TT_ASSERT_EQ_STR(w[0], "hello");
    TT_ASSERT_EQ_STR(w[1], "world");
    TT_ASSERT_EQ_STR(w[2], "test");
    TT_ASSERT_EQ_STR(w[3], "case");
    tt_str_split_free(w);

    w = tt_str_split_words("__hello..world__", &count);
    TT_ASSERT_EQ_INT(count, 2);
    TT_ASSERT_EQ_STR(w[0], "hello");
    TT_ASSERT_EQ_STR(w[1], "world");
    tt_str_split_free(w);

    w = tt_str_split_words("", &count);
    TT_ASSERT_EQ_INT(count, 0);
    tt_str_split_free(w);

    w = tt_str_split_words("one\ttwo  three", &count);
    TT_ASSERT_EQ_INT(count, 3);
    tt_str_split_free(w);
}

/* ---- trim ---- */

TT_TEST(test_str_trim)
{
    char s1[] = "  hello  ";
    TT_ASSERT_EQ_STR(tt_str_trim(s1), "hello");

    char s2[] = "no_trim";
    TT_ASSERT_EQ_STR(tt_str_trim(s2), "no_trim");

    char s3[] = "   ";
    TT_ASSERT_EQ_STR(tt_str_trim(s3), "");
}

/* ---- UTF-8 ---- */

TT_TEST(test_utf8_strlen)
{
    TT_ASSERT_EQ_INT((int)tt_utf8_strlen("hello"), 5);
    TT_ASSERT_EQ_INT((int)tt_utf8_strlen("caf\xc3\xa9"), 4);
    TT_ASSERT_EQ_INT((int)tt_utf8_strlen(""), 0);
}

TT_TEST(test_utf8_truncate)
{
    /* "cafe" in UTF-8: c(1) a(1) f(1) e-accent(2) = 5 bytes */
    char s[] = "caf\xc3\xa9";
    TT_ASSERT_EQ_INT((int)strlen(s), 5);

    /* Truncate at 4 bytes would cut the accent in half */
    tt_utf8_truncate(s, 4);
    TT_ASSERT_EQ_STR(s, "caf");
}

/* ---- memfind ---- */

TT_TEST(test_memfind)
{
    /* Basic match */
    const char *hay = "hello {{ world }}";
    TT_ASSERT_NOT_NULL(tt_memfind(hay, strlen(hay), "{{", 2));
    TT_ASSERT_EQ_INT((int)((const char *)tt_memfind(hay, strlen(hay), "{{", 2) - hay), 6);

    /* No match */
    TT_ASSERT_NULL(tt_memfind("hello world", 11, "{{", 2));

    /* Match at start */
    TT_ASSERT_EQ_INT((int)((const char *)tt_memfind("{{foo}}", 7, "{{", 2) - "{{foo}}"), 0);

    /* Match at end */
    TT_ASSERT_EQ_INT((int)((const char *)tt_memfind("foo{{", 5, "{{", 2) - "foo{{"), 3);

    /* Needle larger than haystack */
    TT_ASSERT_NULL(tt_memfind("ab", 2, "abc", 3));

    /* Empty needle returns haystack */
    TT_ASSERT_NOT_NULL(tt_memfind("abc", 3, "", 0));

    /* Single byte needle */
    TT_ASSERT_NOT_NULL(tt_memfind("abc", 3, "b", 1));
    TT_ASSERT_NULL(tt_memfind("abc", 3, "x", 1));

    /* Haystack with embedded NULs (binary safe) */
    const char bin[] = "ab\0cd{{ef";
    TT_ASSERT_NOT_NULL(tt_memfind(bin, sizeof(bin) - 1, "{{", 2));
    TT_ASSERT_EQ_INT((int)((const char *)tt_memfind(bin, sizeof(bin) - 1, "{{", 2) - bin), 5);
}

void run_str_util_tests(void)
{
    TT_RUN(test_strbuf_append_and_format);
    TT_RUN(test_array_push_pop);
    TT_RUN(test_strdup_funcs);
    TT_RUN(test_str_predicates);
    TT_RUN(test_str_split);
    TT_RUN(test_str_split_words);
    TT_RUN(test_str_trim);
    TT_RUN(test_utf8_strlen);
    TT_RUN(test_utf8_truncate);
    TT_RUN(test_memfind);
}
