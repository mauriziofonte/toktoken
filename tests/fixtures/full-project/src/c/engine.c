#include "engine.h"
#include <string.h>
#include <stdio.h>

static int engine_ready = 0;

int init_engine(void) {
    if (engine_ready) {
        return 0;
    }
    engine_ready = 1;
    printf("engine initialized\n");
    return 0;
}

int process_data(struct DataBuffer *buf) {
    if (!buf || !buf->data || buf->size == 0) {
        return -1;
    }
    if (!engine_ready) {
        fprintf(stderr, "engine not initialized\n");
        return -1;
    }

    /* Fill buffer with sample pattern for demonstration */
    size_t fill = buf->size < 256 ? buf->size : 256;
    for (size_t i = 0; i < fill; i++) {
        buf->data[i] = (unsigned char)(i & 0xFF);
    }
    buf->used = fill;
    buf->flags = 1;
    return 0;
}

unsigned int calculate_hash(const struct DataBuffer *buf) {
    if (!buf || !buf->data || buf->used == 0) {
        return 0;
    }
    /* FNV-1a hash */
    unsigned int hash = 2166136261u;
    for (size_t i = 0; i < buf->used; i++) {
        hash ^= (unsigned int)buf->data[i];
        hash *= 16777619u;
    }
    return hash;
}
