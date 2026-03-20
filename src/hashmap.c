/*
 * hashmap.c -- Open-addressing hash table with FNV-1a and linear probing.
 *
 * Load factor max 0.75, doubles on resize. Tombstone-based deletion.
 */

#include "hashmap.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Bucket states */
#define BUCKET_EMPTY     0
#define BUCKET_OCCUPIED  1
#define BUCKET_TOMBSTONE 2

typedef struct {
    char *key;       /* [owns] copied key, NULL if empty/tombstone */
    void *value;
    uint8_t state;
} bucket_t;

struct tt_hashmap {
    bucket_t *buckets;
    size_t cap;      /* always a power of 2 */
    size_t count;    /* live entries */
    size_t used;     /* live + tombstones (for load factor) */
};

/* FNV-1a hash for strings */
static uint64_t fnv1a(const char *key)
{
    uint64_t h = 14695981039346656037ULL;
    for (const unsigned char *p = (const unsigned char *)key; *p; p++) {
        h ^= *p;
        h *= 1099511628211ULL;
    }
    return h;
}

static size_t next_pow2(size_t n)
{
    if (n < 8) return 8;
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
#if SIZE_MAX > 0xFFFFFFFFUL
    n |= n >> 32;
#endif
    return n + 1;
}

static void hashmap_resize(tt_hashmap_t *m, size_t new_cap);

tt_hashmap_t *tt_hashmap_new(size_t initial_cap)
{
    tt_hashmap_t *m = calloc(1, sizeof(*m));
    if (!m) return NULL;

    m->cap = next_pow2(initial_cap);
    m->buckets = calloc(m->cap, sizeof(bucket_t));
    if (!m->buckets) { free(m); return NULL; }

    return m;
}

void tt_hashmap_free(tt_hashmap_t *m)
{
    if (!m) return;
    for (size_t i = 0; i < m->cap; i++) {
        if (m->buckets[i].state == BUCKET_OCCUPIED)
            free(m->buckets[i].key);
    }
    free(m->buckets);
    free(m);
}

void tt_hashmap_free_with_values(tt_hashmap_t *m)
{
    if (!m) return;
    for (size_t i = 0; i < m->cap; i++) {
        if (m->buckets[i].state == BUCKET_OCCUPIED) {
            free(m->buckets[i].key);
            free(m->buckets[i].value);
        }
    }
    free(m->buckets);
    free(m);
}

static size_t find_slot(const tt_hashmap_t *m, const char *key, bool for_insert)
{
    uint64_t h = fnv1a(key);
    size_t mask = m->cap - 1;
    size_t idx = (size_t)(h & mask);
    size_t first_tombstone = (size_t)-1;

    for (size_t i = 0; i < m->cap; i++) {
        size_t pos = (idx + i) & mask;
        bucket_t *b = &m->buckets[pos];

        if (b->state == BUCKET_EMPTY) {
            return for_insert ? (first_tombstone != (size_t)-1 ? first_tombstone : pos) : (size_t)-1;
        }
        if (b->state == BUCKET_TOMBSTONE) {
            if (for_insert && first_tombstone == (size_t)-1) first_tombstone = pos;
            continue;
        }
        /* BUCKET_OCCUPIED */
        if (strcmp(b->key, key) == 0) return pos;
    }

    return for_insert ? (first_tombstone != (size_t)-1 ? first_tombstone : (size_t)-1) : (size_t)-1;
}

static void hashmap_resize(tt_hashmap_t *m, size_t new_cap)
{
    bucket_t *old_buckets = m->buckets;
    size_t old_cap = m->cap;

    m->cap = new_cap;
    m->buckets = calloc(new_cap, sizeof(bucket_t));
    if (!m->buckets) {
        /* Restore on failure */
        m->buckets = old_buckets;
        m->cap = old_cap;
        return;
    }
    m->count = 0;
    m->used = 0;

    for (size_t i = 0; i < old_cap; i++) {
        if (old_buckets[i].state == BUCKET_OCCUPIED) {
            size_t pos = find_slot(m, old_buckets[i].key, true);
            if (pos == (size_t)-1) continue;
            m->buckets[pos].key = old_buckets[i].key;
            m->buckets[pos].value = old_buckets[i].value;
            m->buckets[pos].state = BUCKET_OCCUPIED;
            m->count++;
            m->used++;
        }
    }

    free(old_buckets);
}

void *tt_hashmap_set(tt_hashmap_t *m, const char *key, void *value)
{
    if (!m || !key) return NULL;

    /* Resize at 75% load */
    if (m->used * 4 >= m->cap * 3) {
        hashmap_resize(m, m->cap * 2);
    }

    size_t pos = find_slot(m, key, true);
    if (pos == (size_t)-1) return NULL; /* should not happen after resize */

    bucket_t *b = &m->buckets[pos];

    if (b->state == BUCKET_OCCUPIED) {
        /* Update existing */
        void *old = b->value;
        b->value = value;
        return old;
    }

    /* New entry */
    b->key = strdup(key);
    if (!b->key) return NULL;
    b->value = value;
    if (b->state != BUCKET_TOMBSTONE) m->used++;
    b->state = BUCKET_OCCUPIED;
    m->count++;
    return NULL;
}

void *tt_hashmap_get(const tt_hashmap_t *m, const char *key)
{
    if (!m || !key) return NULL;
    size_t pos = find_slot(m, key, false);
    if (pos == (size_t)-1) return NULL;
    return m->buckets[pos].value;
}

bool tt_hashmap_has(const tt_hashmap_t *m, const char *key)
{
    if (!m || !key) return false;
    return find_slot(m, key, false) != (size_t)-1;
}

void *tt_hashmap_remove(tt_hashmap_t *m, const char *key)
{
    if (!m || !key) return NULL;
    size_t pos = find_slot(m, key, false);
    if (pos == (size_t)-1) return NULL;

    bucket_t *b = &m->buckets[pos];
    void *old = b->value;
    free(b->key);
    b->key = NULL;
    b->value = NULL;
    b->state = BUCKET_TOMBSTONE;
    m->count--;
    /* used stays the same (tombstones count toward load) */
    return old;
}

size_t tt_hashmap_count(const tt_hashmap_t *m)
{
    return m ? m->count : 0;
}

void tt_hashmap_iter(const tt_hashmap_t *m, tt_hashmap_iter_cb cb, void *userdata)
{
    if (!m || !cb) return;
    for (size_t i = 0; i < m->cap; i++) {
        if (m->buckets[i].state == BUCKET_OCCUPIED) {
            if (cb(m->buckets[i].key, m->buckets[i].value, userdata) != 0)
                return;
        }
    }
}
