#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

static const char * const test_strings[] = {
    "hello",
    "world",
    "newminhh",
    "x32 long mode compatibility system",
    "container-less legacy i386 runtime execution",
    "specialized stub generator level 2a",
    "anchored symbolic att gas emitter",
    "benchmark string iteration payload"
};

int main(int argc, char **argv) {
    long iters = 1000000L;
    if (argc > 1) {
        char *endptr = NULL;
        long val = strtol(argv[1], &endptr, 10);
        if (endptr != argv[1] && val > 0) {
            iters = val;
        }
    }

    const size_t num_strings = sizeof(test_strings) / sizeof(test_strings[0]);
    uint64_t sum = 0;

    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    for (long i = 0; i < iters; i++) {
        const char * volatile s = test_strings[(size_t)i % num_strings];
        sum += (uint64_t)strlen((const char *)s) + (uint64_t)(i & 0xff);
    }

    clock_gettime(CLOCK_MONOTONIC, &ts_end);

    long elapsed_ms = (long)((ts_end.tv_sec - ts_start.tv_sec) * 1000L +
                            (ts_end.tv_nsec - ts_start.tv_nsec) / 1000000L);

    printf("STRLEN_BENCH_OK iters=%ld sum=0x%llx ms=%ld\n",
           iters, (unsigned long long)sum, elapsed_ms);

    return 0;
}
