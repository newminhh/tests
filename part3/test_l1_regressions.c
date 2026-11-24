/* Regression test suite for Level 1 patcher features:
 * - jecxz preserving EFLAGS (cmp before jecxz + je after)
 * - 16-bit inc %ax / dec %bx (66 40 / 66 4b)
 * - inline data bytes inside .text after jmp
 * - %gs: load/store with ah/bh (hi8 + REX)
 * - movl $fn, disp(%esp) callback pointer
 * - environ / optarg R_386_COPY
 * - __thread TLS variable
 * - ifunc resolver
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;
extern char *optarg;

/* 1. jecxz preserving EFLAGS */
static int test_jecxz(void) {
    int res = 0;
    __asm__ volatile (
        "movl $100, %%eax\n\t"
        "cmpl $100, %%eax\n\t"   /* ZF = 1 */
        "movl $1, %%ecx\n\t"     /* ECX != 0, movl preserves EFLAGS */
        "jecxz 1f\n\t"           /* should not branch because ecx != 0 */
        "je 2f\n\t"              /* should branch because ZF == 1 */
        "movl $1, %0\n\t"        /* failed: ZF was clobbered */
        "jmp 3f\n\t"
        "1:\n\t"
        "movl $2, %0\n\t"        /* failed: jecxz took branch */
        "jmp 3f\n\t"
        "2:\n\t"
        "movl $0, %0\n\t"        /* passed */
        "3:\n\t"
        : "=r"(res)
        :
        : "eax", "ecx", "cc"
    );
    return res;
}

/* 2. 16-bit inc %ax / dec %bx (66 40 / 66 4b) */
static int test_16bit_inc_dec(void) {
    int a = 0x1234;
    int b = 0x5678;
    __asm__ volatile (
        ".byte 0x66, 0x40\n\t"  /* inc %ax */
        ".byte 0x66, 0x4b\n\t"  /* dec %bx */
        : "+a"(a), "+b"(b)
        :
        : "cc"
    );
    if ((a & 0xffff) != 0x1235) return 1;
    if ((b & 0xffff) != 0x5677) return 2;
    return 0;
}

/* 3. inline data bytes inside .text after jmp */
static int test_inline_data(void) {
    int val = 0;
    __asm__ volatile (
        "jmp 1f\n\t"
        ".byte 0xde, 0xad, 0xbe, 0xef, 0x01, 0x02, 0x03, 0x04\n\t"
        "1:\n\t"
        "movl $777, %0\n\t"
        : "=r"(val)
    );
    return (val == 777) ? 0 : 1;
}

/* 4. %gs: load/store with ah/bh (hi8 + REX) */
static int test_gs_hi8(void) {
    unsigned int out_val = 0;
    __asm__ volatile (
        "movl $0x11223344, %%eax\n\t"
        "movb %%gs:0x14, %%ah\n\t"
        "movl %%eax, %0\n\t"
        : "=r"(out_val)
        :
        : "eax"
    );
    if ((out_val & 0xff) != 0x44 || ((out_val >> 16) & 0xffff) != 0x1122) return 1;
    return 0;
}

/* 5. movl $fn, disp(%esp) callback pointer */
static int callback_target(int x) {
    return x * 3;
}
typedef int (*cb_fn_t)(int);

static int test_movl_fn_mem(void) {
    volatile cb_fn_t cb_ptr;
    __asm__ volatile (
        "movl %1, %0\n\t"
        : "=m"(cb_ptr)
        : "r"(callback_target)
    );
    return (cb_ptr(14) == 42) ? 0 : 1;
}

/* 6. environ / optarg R_386_COPY */
static int test_environ_optarg_copy(void) {
    if (environ == NULL) return 1;
    if (environ[0] == NULL) return 2;
    static char test_str[] = "optarg_test_val";
    optarg = test_str;
    if (optarg != test_str) return 3;
    return 0;
}

/* 7. __thread TLS variable */
static __thread int g_tls_test_val = 999;

static int test_tls(void) {
    if (g_tls_test_val != 999) return 1;
    g_tls_test_val += 1001;
    if (g_tls_test_val != 2000) return 2;
    return 0;
}

/* 8. ifunc resolver (compiled with gcc -m32) */
static int ifunc_impl_1(int a, int b) { return a + b + 10; }
static int ifunc_impl_2(int a, int b) { return a + b + 20; }

static int (*resolve_ifunc_fn(void))(int, int) {
    return ifunc_impl_1;
}

int ifunc_tested(int a, int b) __attribute__((ifunc("resolve_ifunc_fn")));

static int test_ifunc_feature(void) {
    int r = ifunc_tested(5, 7);
    return (r == 22) ? 0 : 1;
}

int main(void) {
    if (test_jecxz() != 0) {
        printf("FAIL: jecxz\n");
        return 1;
    }
    if (test_16bit_inc_dec() != 0) {
        printf("FAIL: 16bit_inc_dec\n");
        return 2;
    }
    if (test_inline_data() != 0) {
        printf("FAIL: inline_data\n");
        return 3;
    }
    if (test_gs_hi8() != 0) {
        printf("FAIL: gs_hi8\n");
        return 4;
    }
    if (test_movl_fn_mem() != 0) {
        printf("FAIL: movl_fn_mem\n");
        return 5;
    }
    if (test_environ_optarg_copy() != 0) {
        printf("FAIL: environ_optarg_copy\n");
        return 6;
    }
    if (test_tls() != 0) {
        printf("FAIL: tls\n");
        return 7;
    }
    if (test_ifunc_feature() != 0) {
        printf("FAIL: ifunc_feature\n");
        return 8;
    }
    printf("L1_REGRESSIONS_OK\n");
    return 0;
}
