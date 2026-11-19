#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef int (*nm_cb_t)(int x, int y);

static uint32_t g_init_magic = 0;
static char *g_init_buf = NULL;
static nm_cb_t g_host_cb = NULL;

__attribute__((constructor))
static void lifecycle_ctor(void) {
    g_init_magic = 0x4E4D4948u; /* "NMIH" */
    g_init_buf = (char *)malloc(128);
    if (g_init_buf) {
        snprintf(g_init_buf, 128, "LIFECYCLE_INIT_OK");
    }
}

__attribute__((destructor))
static void lifecycle_dtor(void) {
    const char *marker = getenv("NEWMINHH_FINI_MARKER");
    if (marker && marker[0] != '\0') {
        int fd = open(marker, O_CREAT | O_WRONLY | O_TRUNC, 0600);
        if (fd >= 0) {
            const char msg[] = "FINI_OK\n";
            ssize_t wr = write(fd, msg, sizeof(msg) - 1);
            (void)wr;
            close(fd);
        }
    }
    if (g_init_buf) {
        free(g_init_buf);
        g_init_buf = NULL;
    }
    g_init_magic = 0;
}

int lifecycle_check_init(void) {
    if (g_init_magic == 0x4E4D4948u && g_init_buf &&
        strcmp(g_init_buf, "LIFECYCLE_INIT_OK") == 0) {
        return 1;
    }
    return 0;
}

int lifecycle_get_pid_int80(void) {
#if defined(__i386__)
    int32_t pid = 0;
    __asm__ volatile (
        "int $0x80"
        : "=a"(pid)
        : "a"(20) /* SYS_getpid */
        : "memory"
    );
    return (int)pid;
#else
    return (int)getpid();
#endif
}

int lifecycle_stat_probe(const char *path, uint64_t *out_size, uint64_t *out_ino) {
    if (!path || !out_size || !out_ino) return -1;
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    *out_size = (uint64_t)st.st_size;
    *out_ino = (uint64_t)st.st_ino;
    return 0;
}

int lifecycle_timespec_rlimit_probe(int64_t *out_sec, int64_t *out_nsec,
                                    uint64_t *out_cur, uint64_t *out_max) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) return -1;
    if (out_sec) *out_sec = (int64_t)ts.tv_sec;
    if (out_nsec) *out_nsec = (int64_t)ts.tv_nsec;
    if (out_cur) *out_cur = (uint64_t)rl.rlim_cur;
    if (out_max) *out_max = (uint64_t)rl.rlim_max;
    return 0;
}

static int cmp_desc(const void *pa, const void *pb) {
    int a = *(const int *)pa;
    int b = *(const int *)pb;
    return (b > a) - (b < a);
}

void lifecycle_register_cb(nm_cb_t cb) {
    g_host_cb = cb;
}

int lifecycle_trigger_cb(int a, int b) {
    int arr[5] = {3, 1, 4, 5, 2};
    qsort(arr, 5, sizeof(int), cmp_desc);
    if (arr[0] != 5 || arr[4] != 1) {
        return -1;
    }
    if (!g_host_cb) {
        return -2;
    }
    return g_host_cb(a, b) + arr[0];
}

static int so_side_callback(int x, int y) {
    return (x * 10) + y + 7;
}

nm_cb_t lifecycle_get_guest_cb(void) {
    return so_side_callback;
}
