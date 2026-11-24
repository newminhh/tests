#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <sys/mman.h>
#include <unistd.h>

static __thread int g_tls_counter = 42;
static __thread int g_tls_magic = 0x12345678;

__attribute__((noinline))
static int test_switch(int val) {
    switch (val) {
        case 0: return 100;
        case 1: return 105;
        case 2: return 112;
        case 3: return 121;
        case 4: return 132;
        case 5: return 145;
        case 6: return 160;
        case 7: return 177;
        case 8: return 196;
        case 9: return 217;
        case 10: return 240;
        case 11: return 265;
        case 12: return 292;
        default: return -1;
    }
}

__attribute__((noinline))
static int test_loop_and_jecxz(void) {
    int jecxz_hit = 0;
    int loop_cycles = 0;
    const int target_count = 7;

    __asm__ volatile (
        "xorl %%ecx, %%ecx\n\t"
        "jecxz 1f\n\t"
        "movl $99, %0\n\t"
        "jmp 2f\n\t"
        "1:\n\t"
        "movl $1, %0\n\t"
        "2:\n\t"
        : "=r"(jecxz_hit)
        :
        : "ecx", "cc"
    );

    __asm__ volatile (
        "movl %2, %%ecx\n\t"
        "xorl %0, %0\n\t"
        "3:\n\t"
        "incl %0\n\t"
        "loop 3b\n\t"
        : "=&r"(loop_cycles)
        : "0"(0), "r"(target_count)
        : "ecx", "cc"
    );

    return (jecxz_hit == 1 && loop_cycles == target_count) ? 1 : 0;
}

int main(void) {
    /* 1. Switch jump table */
    int sw_sum = 0;
    for (int i = 0; i < 13; i++) {
        sw_sum += test_switch(i);
    }

    /* 2. Floating-point returns (double sqrt, float fabsf) */
    double (*volatile sqrt_fn)(double) = sqrt;
    float (*volatile fabsf_fn)(float) = fabsf;
    double d_val = sqrt_fn(144.0);
    float f_val = fabsf_fn(-42.5f);

    if (d_val < 11.99 || d_val > 12.01 || f_val < 42.49f || f_val > 42.51f) {
        fprintf(stderr, "FAIL: FP return mismatch (d_val=%f, f_val=%f)\n", d_val, f_val);
        return 1;
    }

    /* 3. 64-bit int64_t return (strtoll) */
    long long (*volatile strtoll_fn)(const char *, char **, int) = strtoll;
    long long i64_val = strtoll_fn("0x123456789abcdef0", NULL, 16);
    if (i64_val != 0x123456789abcdef0LL) {
        fprintf(stderr, "FAIL: strtoll mismatch (0x%llx)\n", (unsigned long long)i64_val);
        return 2;
    }

    /* 4. %gs: TLS access */
    g_tls_counter += 100;
    int tls_sum = g_tls_counter + (g_tls_magic ^ 0x12345678);

    /* 5. jecxz / loop constructs */
    int loop_ok = test_loop_and_jecxz();
    if (!loop_ok) {
        fprintf(stderr, "FAIL: loop / jecxz constructs failed\n");
        return 3;
    }

    /* 6. Multi-argument (6-arg) native/compat call: mmap */
    void * (*volatile mmap_fn)(void *, size_t, int, int, int, off_t) = mmap;
    int (*volatile munmap_fn)(void *, size_t) = munmap;

    void *ptr = mmap_fn(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED || ptr == NULL) {
        fprintf(stderr, "FAIL: mmap failed\n");
        return 4;
    }
    *(volatile uint32_t *)ptr = 0xa5a5a5a5;
    uint32_t map_val = *(volatile uint32_t *)ptr;
    munmap_fn(ptr, 4096);
    int mmap_ok = (map_val == 0xa5a5a5a5) ? 1 : 0;

    printf("L2_HIGHVA_OK switch=%d fp=%.1f/%.1f i64=0x%llx tls=%d loop=%d mmap=%d\n",
           sw_sum, d_val, f_val, (unsigned long long)i64_val, tls_sum, loop_ok, mmap_ok);

    return 0;
}
