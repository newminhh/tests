#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#if defined(__i386__)

#define NM_SYS_write         4
#define NM_SYS_getpid        20
#define NM_SYS_munmap        91
#define NM_SYS_uname         122
#define NM_SYS_mmap2         192
#define NM_SYS_exit_group    252
#define NM_SYS_clock_gettime 265

typedef struct {
    int32_t tv_sec;
    int32_t tv_nsec;
} nm_timespec32_t;

__attribute__((noinline))
static int32_t raw_int80(uint32_t nr, uint32_t a1, uint32_t a2, uint32_t a3,
                         uint32_t a4, uint32_t a5, uint32_t a6) {
    uint32_t args[6] = {a1, a2, a3, a4, a5, a6};
    int32_t ret;
    __asm__ volatile (
        "pushl %%ebx\n\t"
        "pushl %%esi\n\t"
        "pushl %%edi\n\t"
        "pushl %%ebp\n\t"
        "movl 0(%%ecx), %%ebx\n\t"
        "movl 4(%%ecx), %%edx\n\t"
        "movl 8(%%ecx), %%esi\n\t"
        "movl 12(%%ecx), %%edi\n\t"
        "movl 16(%%ecx), %%ebp\n\t"
        "pushl %%edx\n\t"
        "pushl %%esi\n\t"
        "pushl %%edi\n\t"
        "pushl %%ebp\n\t"
        "movl 20(%%ecx), %%ebp\n\t"
        "movl %%edi, %%edi\n\t"
        "movl 12(%%esp), %%ecx\n\t"
        "movl 8(%%esp), %%edx\n\t"
        "movl 4(%%esp), %%esi\n\t"
        "movl 0(%%esp), %%edi\n\t"
        "addl $16, %%esp\n\t"
        "int $0x80\n\t"
        "popl %%ebp\n\t"
        "popl %%edi\n\t"
        "popl %%esi\n\t"
        "popl %%ebx\n\t"
        : "=a"(ret)
        : "a"(nr), "c"(args)
        : "edx", "memory", "cc"
    );
    return ret;
}

int main(void) {
    int32_t pid = raw_int80(NM_SYS_getpid, 0, 0, 0, 0, 0, 0);
    if (pid <= 0) {
        raw_int80(NM_SYS_exit_group, 1, 0, 0, 0, 0, 0);
    }

    char uts[512];
    memset(uts, 0, sizeof(uts));
    int32_t uret = raw_int80(NM_SYS_uname, (uint32_t)(uintptr_t)uts, 0, 0, 0, 0, 0);
    if (uret != 0 || strcmp(uts, "Linux") != 0) {
        raw_int80(NM_SYS_exit_group, 2, 0, 0, 0, 0, 0);
    }

    int32_t map_res = raw_int80(NM_SYS_mmap2, 0, 4096, 3 /* PROT_READ|PROT_WRITE */,
                                0x22 /* MAP_PRIVATE|MAP_ANONYMOUS */, (uint32_t)-1, 0);
    if ((uint32_t)map_res >= 0xfffff000u) {
        raw_int80(NM_SYS_exit_group, 3, 0, 0, 0, 0, 0);
    }

    volatile uint32_t *page = (volatile uint32_t *)(uintptr_t)(uint32_t)map_res;
    uint32_t mmap_sum = 0;
    for (uint32_t i = 0; i < 16; i++) {
        page[i] = (i + 1u) * 10u;
        mmap_sum += page[i];
    }

    int32_t unmap_res = raw_int80(NM_SYS_munmap, (uint32_t)(uintptr_t)page, 4096, 0, 0, 0, 0);
    if (unmap_res != 0) {
        raw_int80(NM_SYS_exit_group, 4, 0, 0, 0, 0, 0);
    }

    nm_timespec32_t ts = {0, -1};
    int32_t clk_res = raw_int80(NM_SYS_clock_gettime, 1 /* CLOCK_MONOTONIC */,
                                (uint32_t)(uintptr_t)&ts, 0, 0, 0, 0);
    if (clk_res != 0 || ts.tv_nsec < 0) {
        raw_int80(NM_SYS_exit_group, 5, 0, 0, 0, 0, 0);
    }

    char out[128];
    int len = snprintf(out, sizeof(out),
                       "INT80_OK pid_ok=1 sysname=%s mmap_sum=%u clock_ok=1\n",
                       uts, mmap_sum);
    if (len > 0) {
        raw_int80(NM_SYS_write, 1, (uint32_t)(uintptr_t)out, (uint32_t)len, 0, 0, 0);
    }
    raw_int80(NM_SYS_exit_group, 0, 0, 0, 0, 0, 0);
    return 0;
}

#else /* Native x32 baseline using standard POSIX/libc */

#include <sys/mman.h>
#include <sys/utsname.h>
#include <time.h>

int main(void) {
    pid_t pid = getpid();
    if (pid <= 0) {
        _exit(1);
    }

    struct utsname uts;
    if (uname(&uts) != 0 || strcmp(uts.sysname, "Linux") != 0) {
        _exit(2);
    }

    void *mem = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        _exit(3);
    }

    volatile uint32_t *page = (volatile uint32_t *)mem;
    uint32_t mmap_sum = 0;
    for (uint32_t i = 0; i < 16; i++) {
        page[i] = (i + 1u) * 10u;
        mmap_sum += page[i];
    }

    if (munmap(mem, 4096) != 0) {
        _exit(4);
    }

    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0 || ts.tv_nsec < 0) {
        _exit(5);
    }

    char out[128];
    int len = snprintf(out, sizeof(out),
                       "INT80_OK pid_ok=1 sysname=%s mmap_sum=%u clock_ok=1\n",
                       uts.sysname, mmap_sum);
    if (len > 0) {
        ssize_t wr = write(1, out, (size_t)len);
        (void)wr;
    }
    _exit(0);
    return 0;
}

#endif
