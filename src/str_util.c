/*
 * str_util.c -- String utilities, dynamic string buffer, and dynamic array.
 */

#include "str_util.h"
#include "platform.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ===== Dynamic string buffer ===== */

void tt_strbuf_init(tt_strbuf_t *sb)
{
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

void tt_strbuf_init_cap(tt_strbuf_t *sb, size_t initial_cap)
{
    sb->len = 0;
    sb->cap = initial_cap > 0 ? initial_cap : 64;
    sb->data = malloc(sb->cap);
    if (sb->data) sb->data[0] = '\0';
}

static bool strbuf_grow(tt_strbuf_t *sb, size_t needed)
{
    size_t new_cap = sb->cap ? sb->cap : 64;
    while (new_cap < sb->len + needed + 1)
        new_cap *= 2;

    char *new_data = realloc(sb->data, new_cap);
    if (!new_data) return false;
    sb->data = new_data;
    sb->cap = new_cap;
    return true;
}

void tt_strbuf_append(tt_strbuf_t *sb, const char *s, size_t len)
{
    if (!sb || !s || len == 0) return;
    if (sb->len + len + 1 > sb->cap) {
        if (!strbuf_grow(sb, len)) return;
    }
    memcpy(sb->data + sb->len, s, len);
    sb->len += len;
    sb->data[sb->len] = '\0';
}

void tt_strbuf_append_str(tt_strbuf_t *sb, const char *s)
{
    if (!s) return;
    tt_strbuf_append(sb, s, strlen(s));
}

void tt_strbuf_appendf(tt_strbuf_t *sb, const char *fmt, ...)
{
    if (!sb || !fmt) return;

    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);

    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (needed < 0) { va_end(ap2); return; }

    if (sb->len + (size_t)needed + 1 > sb->cap) {
        if (!strbuf_grow(sb, (size_t)needed)) { va_end(ap2); return; }
    }

    vsnprintf(sb->data + sb->len, (size_t)needed + 1, fmt, ap2);
    va_end(ap2);
    sb->len += (size_t)needed;
}

void tt_strbuf_append_char(tt_strbuf_t *sb, char c)
{
    tt_strbuf_append(sb, &c, 1);
}

char *tt_strbuf_detach(tt_strbuf_t *sb)
{
    char *result = sb->data;
    if (!result) {
        result = malloc(1);
        if (result) result[0] = '\0';
    }
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
    return result;
}

void tt_strbuf_free(tt_strbuf_t *sb)
{
    if (!sb) return;
    free(sb->data);
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

void tt_strbuf_reset(tt_strbuf_t *sb)
{
    if (!sb) return;
    sb->len = 0;
    if (sb->data) sb->data[0] = '\0';
}

/* ===== Dynamic array ===== */

void tt_array_init(tt_array_t *a)
{
    a->items = NULL;
    a->len = 0;
    a->cap = 0;
}

void tt_array_push(tt_array_t *a, void *item)
{
    if (a->len >= a->cap) {
        size_t new_cap = a->cap ? a->cap * 2 : 16;
        void **new_items = realloc(a->items, new_cap * sizeof(void *));
        if (!new_items) return;
        a->items = new_items;
        a->cap = new_cap;
    }
    a->items[a->len++] = item;
}

void *tt_array_pop(tt_array_t *a)
{
    if (!a || a->len == 0) return NULL;
    return a->items[--a->len];
}

void tt_array_free(tt_array_t *a)
{
    if (!a) return;
    free(a->items);
    a->items = NULL;
    a->len = 0;
    a->cap = 0;
}

void tt_array_free_items(tt_array_t *a)
{
    if (!a) return;
    for (size_t i = 0; i < a->len; i++)
        free(a->items[i]);
    free(a->items);
    a->items = NULL;
    a->len = 0;
    a->cap = 0;
}

void tt_array_sort(tt_array_t *a, int (*cmp)(const void *, const void *))
{
    if (!a || !cmp || a->len < 2) return;
    qsort(a->items, a->len, sizeof(void *), cmp);
}

/* ===== String utilities ===== */

char *tt_strdup(const char *s)
{
    if (!s) return NULL;
    size_t len = strlen(s);
    char *d = malloc(len + 1);
    if (!d) return NULL;
    memcpy(d, s, len + 1);
    return d;
}

char *tt_strndup(const char *s, size_t n)
{
    if (!s) return NULL;
    size_t len = strlen(s);
    if (n < len) len = n;
    char *d = malloc(len + 1);
    if (!d) return NULL;
    memcpy(d, s, len);
    d[len] = '\0';
    return d;
}

char *tt_str_tolower(const char *s)
{
    if (!s) return NULL;
    size_t len = strlen(s);
    char *d = malloc(len + 1);
    if (!d) return NULL;
    for (size_t i = 0; i < len; i++)
        d[i] = (char)tolower((unsigned char)s[i]);
    d[len] = '\0';
    return d;
}

bool tt_str_starts_with(const char *s, const char *prefix)
{
    if (!s || !prefix) return false;
    size_t plen = strlen(prefix);
    return strncmp(s, prefix, plen) == 0;
}

bool tt_str_ends_with(const char *s, const char *suffix)
{
    if (!s || !suffix) return false;
    size_t slen = strlen(s);
    size_t xlen = strlen(suffix);
    if (xlen > slen) return false;
    return strcmp(s + slen - xlen, suffix) == 0;
}

bool tt_str_contains(const char *haystack, const char *needle)
{
    if (!haystack || !needle) return false;
    return strstr(haystack, needle) != NULL;
}

bool tt_str_contains_ci(const char *haystack, const char *needle)
{
    return tt_strcasestr(haystack, needle) != NULL;
}

char **tt_str_split(const char *s, char delim, int *out_count)
{
    if (!s) {
        if (out_count) *out_count = 0;
        return NULL;
    }

    /* Count parts */
    int count = 1;
    for (const char *p = s; *p; p++) {
        if (*p == delim) count++;
    }

    char **parts = malloc(((size_t)count + 1) * sizeof(char *));
    if (!parts) { if (out_count) *out_count = 0; return NULL; }

    int idx = 0;
    const char *start = s;
    for (const char *p = s; ; p++) {
        if (*p == delim || *p == '\0') {
            size_t len = (size_t)(p - start);
            parts[idx] = malloc(len + 1);
            if (parts[idx]) {
                memcpy(parts[idx], start, len);
                parts[idx][len] = '\0';
            }
            idx++;
            if (!*p) break;
            start = p + 1;
        }
    }
    parts[idx] = NULL;

    if (out_count) *out_count = count;
    return parts;
}

void tt_str_split_free(char **parts)
{
    if (!parts) return;
    for (char **p = parts; *p; p++) free(*p);
    free(parts);
}

/*
 * Check if character is a word separator: whitespace, underscore, or dot.
 * Matches PHP preg_split('/[\s_\.]+/', ...).
 */
static bool is_word_sep(unsigned char c)
{
    return isspace(c) || c == '_' || c == '.';
}

char **tt_str_split_words(const char *s, int *out_count)
{
    if (!s) {
        if (out_count) *out_count = 0;
        return NULL;
    }

    /* First pass: count words */
    int count = 0;
    const char *p = s;
    while (*p) {
        /* Skip separators */
        while (*p && is_word_sep((unsigned char)*p)) p++;
        if (!*p) break;
        count++;
        /* Skip non-separators */
        while (*p && !is_word_sep((unsigned char)*p)) p++;
    }

    char **parts = malloc(((size_t)count + 1) * sizeof(char *));
    if (!parts) { if (out_count) *out_count = 0; return NULL; }

    /* Second pass: extract words */
    int idx = 0;
    p = s;
    while (*p) {
        while (*p && is_word_sep((unsigned char)*p)) p++;
        if (!*p) break;
        const char *start = p;
        while (*p && !is_word_sep((unsigned char)*p)) p++;
        size_t len = (size_t)(p - start);
        parts[idx] = malloc(len + 1);
        if (parts[idx]) {
            memcpy(parts[idx], start, len);
            parts[idx][len] = '\0';
        }
        idx++;
    }
    parts[idx] = NULL;

    if (out_count) *out_count = count;
    return parts;
}

char *tt_str_trim(char *s)
{
    if (!s) return s;

    /* Trim leading */
    char *start = s;
    while (*start && isspace((unsigned char)*start)) start++;

    /* Trim trailing */
    size_t len = strlen(start);
    while (len > 0 && isspace((unsigned char)start[len - 1])) len--;
    start[len] = '\0';

    if (start != s) memmove(s, start, len + 1);
    return s;
}

/* ===== UTF-8 ===== */

void tt_utf8_truncate(char *s, size_t max_bytes)
{
    if (!s) return;
    size_t len = strlen(s);
    if (len <= max_bytes) return;

    s[max_bytes] = '\0';

    /* Walk back to find valid UTF-8 boundary */
    while (max_bytes > 0 && (s[max_bytes - 1] & 0xC0) == 0x80) {
        max_bytes--;
    }
    /* Remove the lead byte of the incomplete sequence */
    if (max_bytes > 0 && (s[max_bytes - 1] & 0x80) != 0) {
        unsigned char lead = (unsigned char)s[max_bytes - 1];
        size_t seq_len;
        if ((lead & 0xE0) == 0xC0) seq_len = 2;
        else if ((lead & 0xF0) == 0xE0) seq_len = 3;
        else if ((lead & 0xF8) == 0xF0) seq_len = 4;
        else seq_len = 1;

        /* Check if the sequence would be complete */
        size_t remaining = strlen(s + max_bytes - 1);
        if (remaining < seq_len) {
            s[max_bytes - 1] = '\0';
        }
    }
}

size_t tt_utf8_strlen(const char *s)
{
    if (!s) return 0;
    size_t count = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; ) {
        if ((*p & 0x80) == 0) { p += 1; }
        else if ((*p & 0xE0) == 0xC0) { p += 2; }
        else if ((*p & 0xF0) == 0xE0) { p += 3; }
        else if ((*p & 0xF8) == 0xF0) { p += 4; }
        else { p += 1; } /* Invalid byte, skip */
        count++;
    }
    return count;
}

const void *tt_memfind(const void *haystack, size_t hlen,
                       const void *needle, size_t nlen)
{
    if (nlen == 0) return haystack;
    if (nlen > hlen) return NULL;
    const unsigned char *h = (const unsigned char *)haystack;
    const unsigned char *n = (const unsigned char *)needle;
    size_t limit = hlen - nlen + 1;
    for (size_t i = 0; i < limit; i++) {
        if (h[i] == n[0] && memcmp(h + i, n, nlen) == 0)
            return h + i;
    }
    return NULL;
}
