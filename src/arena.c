/*
 * arena.c -- Arena allocator for batch allocation.
 *
 * Allocates large blocks and bumps a pointer for each allocation.
 * Oversized allocations get their own dedicated block.
 *
 * Design notes:
 *
 *   Alignment is fixed at 8 bytes, not max_align_t (typically 16 on x86_64).
 *   All arena-allocated data in toktoken is char* strings and pointer arrays;
 *   no SIMD, long double, or over-aligned types are ever arena-allocated.
 *   8-byte alignment satisfies all actual use cases and halves the padding
 *   waste compared to 16.
 *
 *   New blocks are prepended to the list (become head), which means residual
 *   space in the previous block is abandoned.  This is a deliberate trade-off:
 *   the arena is used in short-lived scan phases (read rows, discard most,
 *   deep-copy survivors, free everything) where allocations are sequential,
 *   small, and fit comfortably in 64KB blocks.  Maintaining a free-list or
 *   separate oversized chain would add complexity for zero practical gain.
 *
 *   NULL-arena is handled with a silent return NULL rather than an assert().
 *   In a CLI tool, crashing the process on a programming error is worse than
 *   propagating NULL -- and ASAN catches the dereference immediately in debug
 *   builds anyway.
 *
 *   tt_arena_used() reports aligned-bytes-dispensed, not gross RSS.  It is
 *   used exclusively in unit tests to verify the allocator works, not as a
 *   memory profiler.  Block-header overhead and tail waste are intentionally
 *   excluded to keep the accounting trivial.
 */

#include "arena.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TT_ARENA_DEFAULT_BLOCK_SIZE (64 * 1024)  /* 64 KB */
#define TT_ARENA_ALIGNMENT 8

typedef struct tt_arena_block {
    struct tt_arena_block *next;
    size_t size;     /* total usable size */
    size_t used;     /* bytes consumed */
    /* data follows in memory */
} tt_arena_block_t;

struct tt_arena {
    tt_arena_block_t *head;   /* most recent block */
    size_t block_size;        /* default block allocation size */
    size_t total_used;        /* running total of allocated bytes */
};

static size_t align_up(size_t n, size_t align)
{
    return (n + align - 1) & ~(align - 1);
}

static tt_arena_block_t *block_new(size_t data_size)
{
    tt_arena_block_t *b = malloc(sizeof(*b) + data_size);
    if (!b) return NULL;
    b->next = NULL;
    b->size = data_size;
    b->used = 0;
    return b;
}

tt_arena_t *tt_arena_new(size_t block_size)
{
    tt_arena_t *a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->block_size = block_size > 0 ? block_size : TT_ARENA_DEFAULT_BLOCK_SIZE;
    return a;
}

void *tt_arena_alloc(tt_arena_t *a, size_t size)
{
    if (!a || size == 0) return NULL;

    /* Guard against size_t wraparound in align_up().  No real call path in
     * toktoken ever approaches this limit, but the check is free. */
    if (size > SIZE_MAX - TT_ARENA_ALIGNMENT) return NULL;

    size_t aligned = align_up(size, TT_ARENA_ALIGNMENT);

    /* Try current block */
    if (a->head && (a->head->size - a->head->used) >= aligned) {
        void *ptr = (char *)(a->head + 1) + a->head->used;
        a->head->used += aligned;
        a->total_used += aligned;
        return ptr;
    }

    /* Need a new block. Oversized allocations get a dedicated block. */
    size_t bsize = aligned > a->block_size ? aligned : a->block_size;
    tt_arena_block_t *b = block_new(bsize);
    if (!b) return NULL;

    b->next = a->head;
    a->head = b;

    void *ptr = (char *)(b + 1) + b->used;
    b->used += aligned;
    a->total_used += aligned;
    return ptr;
}

char *tt_arena_strdup(tt_arena_t *a, const char *s)
{
    if (!a || !s) return NULL;
    size_t len = strlen(s);
    char *d = tt_arena_alloc(a, len + 1);
    if (d) memcpy(d, s, len + 1);
    return d;
}

char *tt_arena_strndup(tt_arena_t *a, const char *s, size_t n)
{
    if (!a || !s) return NULL;
    /* Use strnlen() instead of strlen() so the function stays safe even if
     * the caller passes a buffer that is not null-terminated (read at most
     * n bytes to find the length, never beyond). */
    size_t len = strnlen(s, n);
    char *d = tt_arena_alloc(a, len + 1);
    if (d) {
        memcpy(d, s, len);
        d[len] = '\0';
    }
    return d;
}

void tt_arena_free(tt_arena_t *a)
{
    if (!a) return;
    tt_arena_block_t *b = a->head;
    while (b) {
        tt_arena_block_t *next = b->next;
        free(b);
        b = next;
    }
    free(a);
}

size_t tt_arena_used(const tt_arena_t *a)
{
    return a ? a->total_used : 0;
}
