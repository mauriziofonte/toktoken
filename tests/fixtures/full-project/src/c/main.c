#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"

static void setup(const char *config_path) {
    printf("loading configuration from %s\n", config_path);
    if (init_engine() != 0) {
        fprintf(stderr, "engine initialization failed\n");
        exit(1);
    }
}

int main(int argc, char *argv[]) {
    const char *config = "default.cfg";
    int verbose = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            config = argv[++i];
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    setup(config);

    struct DataBuffer buf;
    buf.data = malloc(1024);
    buf.size = 1024;
    buf.used = 0;
    if (!buf.data) {
        fprintf(stderr, "allocation failed\n");
        return 1;
    }

    int result = process_data(&buf);
    if (verbose) {
        printf("processed %zu bytes, hash=%u\n", buf.used, calculate_hash(&buf));
    }

    free(buf.data);
    return result;
}
