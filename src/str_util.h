/*
 * str_util.h -- String utilities, dynamic string buffer, and dynamic array.
 */

#ifndef TT_STR_UTIL_H
#define TT_STR_UTIL_H

#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

/* === Dynamic string buffer (growable) === */

typedef struct {
    char *data;      /* [owns] Buffer, null-terminated */
    size_t len;      /* Current length (excluding null) */
    size_t cap;      /* Allocated capacity */
} tt_strbuf_t;

void tt_strbuf_init(tt_strbuf_t *sb);
void tt_strbuf_init_cap(tt_strbuf_t *sb, size_t initial_cap);
void tt_strbuf_append(tt_strbuf_t *sb, const char *s, size_t len);
void tt_strbuf_append_str(tt_strbuf_t *sb, const char *s);
void tt_strbuf_appendf(tt_strbuf_t *sb, const char *fmt, ...);
void tt_strbuf_append_char(tt_strbuf_t *sb, char c);

/*
 * tt_strbuf_detach -- Transfer ownership of the buffer to the caller.
 *
 * [caller-frees] The strbuf is reset to empty state after detach.
 */
char *tt_strbuf_detach(tt_strbuf_t *sb);

/*
 * tt_strbuf_free -- Free the internal buffer.
 */
void tt_strbuf_free(tt_strbuf_t *sb);

/*
 * tt_strbuf_reset -- Reset length to 0 but keep the allocated buffer.
 */
void tt_strbuf_reset(tt_strbuf_t *sb);

/* === Dynamic array (growable, void* items) === */

typedef struct {
    void **items;    /* [owns] Array of pointers */
    size_t len;      /* Number of elements */
    size_t cap;      /* Allocated capacity */
} tt_array_t;

void tt_array_init(tt_array_t *a);
void tt_array_push(tt_array_t *a, void *item);
void *tt_array_pop(tt_array_t *a);

/*
 * tt_array_free -- Free the items array. Does NOT free individual items.
 */
void tt_array_free(tt_array_t *a);

/*
 * tt_array_free_items -- Free each item with free(), then the array.
 */
void tt_array_free_items(tt_array_t *a);

/*
 * tt_array_sort -- Sort using qsort. Comparator receives void** pointers.
 */
void tt_array_sort(tt_array_t *a, int (*cmp)(const void *, const void *));

/* === String utilities === */

/*
 * tt_strdup -- NULL-safe strdup.
 *
 * [caller-frees] Returns NULL if s is NULL.
 */
char *tt_strdup(const char *s);

/*
 * tt_strndup -- Duplicate at most n bytes.
 *
 * [caller-frees]
 */
char *tt_strndup(const char *s, size_t n);

/*
 * tt_str_tolower -- Create a new lowercase copy.
 *
 * [caller-frees]
 */
char *tt_str_tolower(const char *s);

bool tt_str_starts_with(const char *s, const char *prefix);
bool tt_str_ends_with(const char *s, const char *suffix);
bool tt_str_contains(const char *haystack, const char *needle);
bool tt_str_contains_ci(const char *haystack, const char *needle);

/*
 * tt_str_split -- Split string by single-char delimiter.
 *
 * [caller-frees] Returns NULL-terminated array. Free with tt_str_split_free.
 * *out_count receives the number of parts (may be NULL).
 */
char **tt_str_split(const char *s, char delim, int *out_count);

/*
 * tt_str_split_free -- Free array returned by tt_str_split / tt_str_split_words.
 */
void tt_str_split_free(char **parts);

/*
 * tt_str_split_words -- Split by whitespace, underscore, or dot.
 *
 * Replicates PHP: preg_split('/[\s_\.]+/', ..., PREG_SPLIT_NO_EMPTY).
 * [caller-frees] Returns NULL-terminated array. Free with tt_str_split_free.
 */
char **tt_str_split_words(const char *s, int *out_count);

/*
 * tt_str_trim -- Trim whitespace in-place. Returns same pointer.
 */
char *tt_str_trim(char *s);

/* === UTF-8 === */

/*
 * tt_utf8_truncate -- Truncate string at UTF-8 boundary.
 *
 * Ensures no partial multibyte sequences at the end.
 */
void tt_utf8_truncate(char *s, size_t max_bytes);

/*
 * tt_utf8_strlen -- Count UTF-8 codepoints (not bytes).
 */
size_t tt_utf8_strlen(const char *s);

/*
 * tt_memfind -- Portable memmem replacement (C11-safe, no _GNU_SOURCE needed).
 *
 * Returns pointer to first occurrence of needle in haystack, or NULL.
 */
const void *tt_memfind(const void *haystack, size_t hlen,
                       const void *needle, size_t nlen);

#endif /* TT_STR_UTIL_H */
