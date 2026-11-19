#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static volatile uint32_t g_sink = 0;

__attribute__((noinline))
static uint32_t stripped_switch(int idx, uint32_t acc) {
    switch (idx) {
    case 0:  acc += 13u; break;
    case 1:  acc ^= 0x101u; break;
    case 2:  acc = acc * 3u + 7u; break;
    case 3:  acc ^= 0x202u; break;
    case 4:  acc += 29u; break;
    case 5:  acc = acc * 5u + 11u; break;
    case 6:  acc ^= 0x303u; break;
    case 7:  acc += 41u; break;
    case 8:  acc = acc * 7u + 17u; break;
    case 9:  acc ^= 0x404u; break;
    case 10: acc += 53u; break;
    case 11: acc = acc * 11u + 19u; break;
    case 12: acc ^= 0x505u; break;
    case 13: acc += 67u; break;
    case 14: acc = acc * 13u + 23u; break;
    case 15: acc ^= 0x606u; break;
    case 16: acc += 79u; break;
    case 17: acc = acc * 17u + 31u; break;
    default: acc ^= (uint32_t)(idx + 0x55u); break;
    }
    g_sink ^= acc;
    return acc;
}

static int cmp_i32(const void *a, const void *b) {
    int32_t va = *(const int32_t *)a;
    int32_t vb = *(const int32_t *)b;
    return (va > vb) - (va < vb);
}

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_shared = 0;

static void *th_worker(void *arg) {
    int delta = *(int *)arg;
    pthread_mutex_lock(&g_mu);
    g_shared += delta;
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

static void on_exit_stripped(void) {
    printf("STRIPPED_ATEXIT_OK\n");
    fflush(stdout);
}

int main(void) {
    uint32_t csum = 1u;
    for (int i = 0; i <= 19; i++) {
        csum = stripped_switch(i, csum);
    }

    char path[128];
    snprintf(path, sizeof(path), "/tmp/newminhh_c11_%ld.dat", (long)getpid());
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0) return 1;
    if (write(fd, "0123456789", 10) != 10) {
        close(fd);
        unlink(path);
        return 1;
    }
    struct stat st;
    int sok = (fstat(fd, &st) == 0 && st.st_size == 10);
    close(fd);
    unlink(path);
    if (!sok) return 1;

    struct timespec ts;
    struct rlimit rl;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0 || getrlimit(RLIMIT_NOFILE, &rl) != 0) {
        return 1;
    }

    int32_t arr[5] = {50, 10, 40, 20, 30};
    qsort(arr, 5, sizeof(int32_t), cmp_i32);
    if (arr[0] != 10 || arr[4] != 50) return 1;

    pthread_t t1, t2;
    int d1 = 15, d2 = 27;
    if (pthread_create(&t1, NULL, th_worker, &d1) != 0) return 1;
    if (pthread_create(&t2, NULL, th_worker, &d2) != 0) return 1;
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    if (g_shared != 42) return 1;

    atexit(on_exit_stripped);
    printf("STRIPPED_OK csum=0x%08x size=%lld sum=%d\n",
           csum, (long long)st.st_size, g_shared);
    fflush(stdout);
    return 0;
}
