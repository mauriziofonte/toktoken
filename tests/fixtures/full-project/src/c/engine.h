#ifndef ENGINE_H
#define ENGINE_H

#include <stddef.h>

struct DataBuffer {
    unsigned char *data;
    size_t size;
    size_t used;
    unsigned int flags;
};

int init_engine(void);
int process_data(struct DataBuffer *buf);
unsigned int calculate_hash(const struct DataBuffer *buf);

#endif /* ENGINE_H */
