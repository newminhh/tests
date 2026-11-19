#include <stdint.h>
#include <stdio.h>

__attribute__((noinline))
static int indirect_jmp_fn(int a, int b) {
    int res = 0;
    /* Unresolved indirect jmp *%eax (not a switch jump table -> REQUIRES_DBT_FALLBACK),
     * also exercising %esi/%edi inside DBT while preserving i386 callee-saved ABI. */
    __asm__ volatile(
        "call 1f\n\t"
        "1:\n\t"
        "popl %%eax\n\t"
        "addl $(2f - 1b), %%eax\n\t"
        "movl %1, %%esi\n\t"
        "movl %2, %%edi\n\t"
        "jmp *%%eax\n\t"
        "2:\n\t"
        "imull $3, %%esi, %%esi\n\t"
        "imull $5, %%edi, %%edi\n\t"
        "leal (%%esi, %%edi, 1), %0\n\t"
        : "=r"(res)
        : "m"(a), "m"(b)
        : "eax", "esi", "edi", "cc"
    );
    return res;
}

__attribute__((noinline))
static int direct_and_live_regs_test(int a, int b) {
    uint32_t out_esi = 0;
    uint32_t out_edi = 0;
    int fn_ret = 0;
    int (*volatile fp)(int, int) = indirect_jmp_fn;

    __asm__ volatile(
        "movl $0x11223344, %%esi\n\t"
        "movl $0x55667788, %%edi\n\t"
        "pushl %%edx\n\t"
        "pushl %%ecx\n\t"
        "call *%%eax\n\t"
        "addl $8, %%esp\n\t"
        "movl %%eax, %0\n\t"
        "movl %%esi, %1\n\t"
        "movl %%edi, %2\n\t"
        : "=m"(fn_ret), "=m"(out_esi), "=m"(out_edi)
        : "a"(fp), "c"(a), "d"(b)
        : "esi", "edi", "memory", "cc"
    );

    int direct_ret = indirect_jmp_fn(a + 1, b + 2);
    printf("FB_OK indirect=%d direct=%d esi=0x%08x edi=0x%08x\n",
           fn_ret, direct_ret, out_esi, out_edi);

    if (fn_ret != (a * 3 + b * 5)) return 1;
    if (direct_ret != ((a + 1) * 3 + (b + 2) * 5)) return 2;
    if (out_esi != 0x11223344u || out_edi != 0x55667788u) return 3;
    return 0;
}

int main(void) {
    return direct_and_live_regs_test(7, 11);
}
