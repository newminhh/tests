#define _GNU_SOURCE
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

__attribute__((noinline))
static uint32_t compute_step(uint32_t state, uint32_t i) {
    switch (i & 7u) {
    case 0: return state + 0x1357u;
    case 1: return state ^ (i * 17u);
    case 2: return (state * 5u) + 11u;
    case 3: return state ^ 0x9e3779b9u;
    case 4: return (state << 3) | (state >> 29);
    case 5: return state + (i ^ 0xa5a5u);
    case 6: return (state * 13u) - 7u;
    case 7: return state ^ (state >> 11);
    default: return state;
    }
}

static int run_compute(uint32_t iters) {
    uint32_t acc = 0xdeadbeefu;
    for (uint32_t i = 0; i < iters; i++) {
        acc = compute_step(acc, i);
    }
    printf("BENCH_COMPUTE_OK csum=0x%08x iters=%u\n", acc, iters);
    return 0;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t va = *(const uint32_t *)a;
    uint32_t vb = *(const uint32_t *)b;
    return (va > vb) - (va < vb);
}

static int run_libc_heavy(uint32_t iters) {
    uint32_t csum = 0;
    struct timespec ts;
    struct stat st;
    char buf[64];

    for (uint32_t i = 0; i < iters; i++) {
        uint32_t *arr = (uint32_t *)malloc(8 * sizeof(uint32_t));
        if (!arr) return 1;
        for (int j = 0; j < 8; j++) {
            arr[j] = (i * 13u + (uint32_t)(7 - j)) & 0xffffu;
        }
        qsort(arr, 8, sizeof(uint32_t), cmp_u32);
        int n = snprintf(buf, sizeof(buf), "%u:%u", arr[0], arr[7]);
        clock_gettime(CLOCK_MONOTONIC, &ts);
        if (stat("/etc/hosts", &st) == 0) {
            csum ^= (uint32_t)st.st_size;
        }
        csum += arr[0] + (uint32_t)n + (uint32_t)(ts.tv_nsec & 0xff);
        free(arr);
    }
    printf("BENCH_LIBC_OK csum=0x%08x iters=%u\n", csum, iters);
    return 0;
}

static int run_daemon(int seconds, const char *marker) {
    char buf[128];
    void *p = malloc(4096);
    if (p) {
        memset(p, 0x5a, 4096);
    }
    struct stat st;
    stat("/etc/hosts", &st);
    double v = sqrt(144.0) + cos(0.0);
    snprintf(buf, sizeof(buf), "daemon_pid=%ld size=%lld v=%.1f",
             (long)getpid(), (long long)st.st_size, v);
    if (marker && marker[0] != '\0') {
        int fd = open(marker, O_CREAT | O_WRONLY | O_TRUNC, 0600);
        if (fd >= 0) {
            ssize_t wr = write(fd, buf, strlen(buf));
            (void)wr;
            close(fd);
        }
    }
    sleep((unsigned int)seconds);
    free(p);
    printf("BENCH_DAEMON_OK %s\n", buf);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s --compute [iters] | --libc-heavy [iters] | --daemon <sec> [marker]\n",
                argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "--compute") == 0) {
        uint32_t iters = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 10) : 5000000u;
        return run_compute(iters);
    }
    if (strcmp(argv[1], "--libc-heavy") == 0) {
        uint32_t iters = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 10) : 10000u;
        return run_libc_heavy(iters);
    }
    if (strcmp(argv[1], "--daemon") == 0) {
        int secs = (argc > 2) ? atoi(argv[2]) : 5;
        const char *marker = (argc > 3) ? argv[3] : NULL;
        return run_daemon(secs, marker);
    }
    fprintf(stderr, "Unknown mode: %s\n", argv[1]);
    return 2;
}
