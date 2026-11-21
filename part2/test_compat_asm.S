.text
.p2align 4

/*
 * cdecl ASM callee: int asm_cdecl_add2(int a, int b)
 * Reads [esp+4] and [esp+8], returns a*3 + b*7 in %eax,
 * and returns via 4-byte stack stride (Part 3 / cdecl convention).
 */
.globl asm_cdecl_add2
.type asm_cdecl_add2, @function
asm_cdecl_add2:
    movl    4(%rsp), %eax
    imull   $3, %eax, %eax
    movl    8(%rsp), %ecx
    imull   $7, %ecx, %ecx
    addl    %ecx, %eax
    movl    (%rsp), %r11d
    leaq    4(%rsp), %rsp
    jmp     *%r11
.size asm_cdecl_add2, .-asm_cdecl_add2

/*
 * cdecl ASM callee: double asm_cdecl_f64_mul_add(double a, float b)
 * [esp+4] = double a (8B), [esp+12] = float b (4B)
 * Returns a * 2.5 + b in x87 ST(0).
 */
.globl asm_cdecl_f64_mul_add
.type asm_cdecl_f64_mul_add, @function
asm_cdecl_f64_mul_add:
    fldl    4(%rsp)
    fmull   .Lconst_2_5(%rip)
    flds    12(%rsp)
    faddp   %st(0), %st(1)
    movl    (%rsp), %r11d
    leaq    4(%rsp), %rsp
    jmp     *%r11
.size asm_cdecl_f64_mul_add, .-asm_cdecl_f64_mul_add

/*
 * cdecl ASM callee: int64_t asm_cdecl_i64_combine(int32_t hi, uint32_t lo)
 * Returns ((int64_t)hi << 32) | lo in EDX:EAX.
 */
.globl asm_cdecl_i64_combine
.type asm_cdecl_i64_combine, @function
asm_cdecl_i64_combine:
    movl    4(%rsp), %edx
    movl    8(%rsp), %eax
    movl    (%rsp), %r11d
    leaq    4(%rsp), %rsp
    jmp     *%r11
.size asm_cdecl_i64_combine, .-asm_cdecl_i64_combine

/*
 * cdecl ASM callee: int asm_cdecl_qsort_cmp_i32(const void *a, const void *b)
 */
.globl asm_cdecl_qsort_cmp_i32
.type asm_cdecl_qsort_cmp_i32, @function
asm_cdecl_qsort_cmp_i32:
    movl    4(%rsp), %ecx
    movl    8(%rsp), %edx
    movl    (%ecx), %ecx
    movl    (%edx), %edx
    xorl    %eax, %eax
    cmpl    %edx, %ecx
    je      .Lqcmp_done
    movl    $-1, %eax
    jl      .Lqcmp_done
    movl    $1, %eax
.Lqcmp_done:
    movl    (%rsp), %r11d
    leaq    4(%rsp), %rsp
    jmp     *%r11
.size asm_cdecl_qsort_cmp_i32, .-asm_cdecl_qsort_cmp_i32

/*
 * cdecl ASM callee: void *asm_cdecl_pthread_worker(void *arg)
 * arg points to uint32_t out[2]; reads %gs:0 and %gs:4, stores sum in out[0],
 * returns out[0] + 100 in %eax.
 */
.globl asm_cdecl_pthread_worker
.type asm_cdecl_pthread_worker, @function
asm_cdecl_pthread_worker:
    movl    4(%rsp), %ecx
    movl    %gs:0, %eax
    addl    %gs:4, %eax
    movl    %eax, (%ecx)
    addl    $100, %eax
    movl    (%rsp), %r11d
    leaq    4(%rsp), %rsp
    jmp     *%r11
.size asm_cdecl_pthread_worker, .-asm_cdecl_pthread_worker

/*
 * cdecl ASM callees for atexit LIFO verification:
 * append '1' / '2' / arg byte to g_asm_atexit_seq[g_asm_atexit_pos++]
 */
.globl asm_cdecl_atexit_1
.type asm_cdecl_atexit_1, @function
asm_cdecl_atexit_1:
    movl    g_asm_atexit_pos(%rip), %eax
    leaq    g_asm_atexit_seq(%rip), %rcx
    movb    $'1', (%rcx, %rax, 1)
    incl    %eax
    movl    %eax, g_asm_atexit_pos(%rip)
    movl    (%rsp), %r11d
    leaq    4(%rsp), %rsp
    jmp     *%r11
.size asm_cdecl_atexit_1, .-asm_cdecl_atexit_1

.globl asm_cdecl_atexit_2
.type asm_cdecl_atexit_2, @function
asm_cdecl_atexit_2:
    movl    g_asm_atexit_pos(%rip), %eax
    leaq    g_asm_atexit_seq(%rip), %rcx
    movb    $'2', (%rcx, %rax, 1)
    incl    %eax
    movl    %eax, g_asm_atexit_pos(%rip)
    movl    (%rsp), %r11d
    leaq    4(%rsp), %rsp
    jmp     *%r11
.size asm_cdecl_atexit_2, .-asm_cdecl_atexit_2

.globl asm_cdecl_cxa_handler
.type asm_cdecl_cxa_handler, @function
asm_cdecl_cxa_handler:
    movl    4(%rsp), %edx           /* arg char */
    movl    g_asm_atexit_pos(%rip), %eax
    leaq    g_asm_atexit_seq(%rip), %rcx
    movb    %dl, (%rcx, %rax, 1)
    incl    %eax
    movl    %eax, g_asm_atexit_pos(%rip)
    movl    (%rsp), %r11d
    leaq    4(%rsp), %rsp
    jmp     *%r11
.size asm_cdecl_cxa_handler, .-asm_cdecl_cxa_handler

/*
 * cdecl ASM callee for pool trampoline:
 * int asm_cdecl_pool_8i(int a0..a7) -> weighted sum of all 8 stack args
 * [esp+4]..[esp+32]
 */
.globl asm_cdecl_pool_8i
.type asm_cdecl_pool_8i, @function
asm_cdecl_pool_8i:
    movl     4(%rsp), %eax
    addl     8(%rsp), %eax
    addl    12(%rsp), %eax
    addl    16(%rsp), %eax
    addl    20(%rsp), %eax
    addl    24(%rsp), %eax
    addl    28(%rsp), %eax
    movl    32(%rsp), %ecx
    imull   $10, %ecx, %ecx
    addl    %ecx, %eax
    movl    (%rsp), %r11d
    leaq    4(%rsp), %rsp
    jmp     *%r11
.size asm_cdecl_pool_8i, .-asm_cdecl_pool_8i

/*
 * cdecl ASM callee for init_array: void asm_cdecl_init_fn(int argc, char **argv, char **envp)
 */
.globl asm_cdecl_init_fn
.type asm_cdecl_init_fn, @function
asm_cdecl_init_fn:
    movl    4(%rsp), %eax
    movl    8(%rsp), %ecx
    movl    12(%rsp), %edx
    movl    %eax, g_asm_init_argc(%rip)
    movl    %ecx, g_asm_init_argv(%rip)
    movl    %edx, g_asm_init_envp(%rip)
    movl    (%rsp), %r11d
    leaq    4(%rsp), %rsp
    jmp     *%r11
.size asm_cdecl_init_fn, .-asm_cdecl_init_fn

/*
 * Helper: uint64_t asm_test_call_x32_cdecl_i64(void *fn, const nm_fn_sig_t *sig, const uint32_t *args)
 * Calls newminhh_call_x32_cdecl and combines EDX:EAX into RAX.
 */
.globl asm_test_call_x32_cdecl_i64
.type asm_test_call_x32_cdecl_i64, @function
asm_test_call_x32_cdecl_i64:
    call    newminhh_call_x32_cdecl
    movl    %eax, %eax
    shlq    $32, %rdx
    orq     %rdx, %rax
    ret
.size asm_test_call_x32_cdecl_i64, .-asm_test_call_x32_cdecl_i64

/*
 * Helper: double asm_test_call_x32_cdecl_fp(void *fn, const nm_fn_sig_t *sig, const uint32_t *args)
 * Calls newminhh_call_x32_cdecl and moves x87 ST(0) -> XMM0.
 */
.globl asm_test_call_x32_cdecl_fp
.type asm_test_call_x32_cdecl_fp, @function
asm_test_call_x32_cdecl_fp:
    subq    $8, %rsp
    call    newminhh_call_x32_cdecl
    fstpl   (%rsp)
    movsd   (%rsp), %xmm0
    addq    $8, %rsp
    ret
.size asm_test_call_x32_cdecl_fp, .-asm_test_call_x32_cdecl_fp

/*
 * Helper: uint32_t asm_test_invoke_cdecl_out_1ptr(void *tramp, const char *s)
 * Calls an outgoing 4-byte-stride trampoline using i386 cdecl stack frame.
 */
.globl asm_test_invoke_cdecl_out_1ptr
.type asm_test_invoke_cdecl_out_1ptr, @function
asm_test_invoke_cdecl_out_1ptr:
    pushq   %rbx
    movq    %rsp, %rbx
    subq    $8, %rsp
    movl    %esi, 4(%rsp)
    leal    .Lout_ret(%rip), %eax
    movl    %eax, (%rsp)
    jmp     *%rdi
.Lout_ret:
    movq    %rbx, %rsp
    popq    %rbx
    ret
.size asm_test_invoke_cdecl_out_1ptr, .-asm_test_invoke_cdecl_out_1ptr

/*
 * Helper: uint64_t asm_test_invoke_cdecl_out_strtoull(void *tramp, const char *nptr, char **endptr, int base)
 * Calls outgoing cdecl trampoline for strtoull using i386 4-byte stack stride,
 * and recombines EDX:EAX into RAX.
 */
.globl asm_test_invoke_cdecl_out_strtoull
.type asm_test_invoke_cdecl_out_strtoull, @function
asm_test_invoke_cdecl_out_strtoull:
    pushq   %rbx
    movq    %rsp, %rbx
    andq    $-16, %rsp
    subq    $20, %rsp
    movl    %esi, 4(%rsp)
    movl    %edx, 8(%rsp)
    movl    %ecx, 12(%rsp)
    leal    .Lout_u64_ret(%rip), %eax
    movl    %eax, (%rsp)
    jmp     *%rdi
.Lout_u64_ret:
    movl    %eax, %eax
    shlq    $32, %rdx
    orq     %rdx, %rax
    movq    %rbx, %rsp
    popq    %rbx
    ret
.size asm_test_invoke_cdecl_out_strtoull, .-asm_test_invoke_cdecl_out_strtoull

/*
 * Helper: double asm_test_invoke_cdecl_out_1f64(void *tramp, double x)
 * Calls outgoing cdecl trampoline for double(double) using i386 4-byte stack stride,
 * and reads x87 ST(0) -> XMM0.
 */
.globl asm_test_invoke_cdecl_out_1f64
.type asm_test_invoke_cdecl_out_1f64, @function
asm_test_invoke_cdecl_out_1f64:
    pushq   %rbx
    movq    %rsp, %rbx
    andq    $-16, %rsp
    subq    $20, %rsp
    movsd   %xmm0, 4(%rsp)
    leal    .Lout_f64_ret(%rip), %eax
    movl    %eax, (%rsp)
    jmp     *%rdi
.Lout_f64_ret:
    fstpl   (%rsp)
    movsd   (%rsp), %xmm0
    movq    %rbx, %rsp
    popq    %rbx
    ret
.size asm_test_invoke_cdecl_out_1f64, .-asm_test_invoke_cdecl_out_1f64

/*
 * Helper: float asm_test_invoke_cdecl_out_1f32(void *tramp, float x)
 * Calls outgoing cdecl trampoline for float(float) using i386 4-byte stack stride,
 * and reads x87 ST(0) -> XMM0.
 */
.globl asm_test_invoke_cdecl_out_1f32
.type asm_test_invoke_cdecl_out_1f32, @function
asm_test_invoke_cdecl_out_1f32:
    pushq   %rbx
    movq    %rsp, %rbx
    andq    $-16, %rsp
    subq    $20, %rsp
    movss   %xmm0, 4(%rsp)
    leal    .Lout_f32_ret(%rip), %eax
    movl    %eax, (%rsp)
    jmp     *%rdi
.Lout_f32_ret:
    fstps   (%rsp)
    movss   (%rsp), %xmm0
    movq    %rbx, %rsp
    popq    %rbx
    ret
.size asm_test_invoke_cdecl_out_1f32, .-asm_test_invoke_cdecl_out_1f32

/*
 * Helper: int asm_test_invoke_cdecl_out_snprintf(void *tramp, char *buf, uint32_t sz,
 *                                                const char *fmt, int32_t i, double d, const char *s)
 */
.globl asm_test_invoke_cdecl_out_snprintf
.type asm_test_invoke_cdecl_out_snprintf, @function
asm_test_invoke_cdecl_out_snprintf:
    pushq   %rbx
    movq    %rsp, %rbx
    andq    $-16, %rsp
    subq    $36, %rsp
    movl    %esi, 4(%rsp)
    movl    %edx, 8(%rsp)
    movl    %ecx, 12(%rsp)
    movl    %r8d, 16(%rsp)
    movsd   %xmm0, 20(%rsp)
    movl    %r9d, 28(%rsp)
    leal    .Lout_snp_ret(%rip), %eax
    movl    %eax, (%rsp)
    jmp     *%rdi
.Lout_snp_ret:
    movq    %rbx, %rsp
    popq    %rbx
    ret
.size asm_test_invoke_cdecl_out_snprintf, .-asm_test_invoke_cdecl_out_snprintf

/*
 * Helper: int asm_test_invoke_cdecl_out_vsnprintf(void *tramp, char *buf, uint32_t sz,
 *                                                 const char *fmt, const uint32_t *va_words)
 */
.globl asm_test_invoke_cdecl_out_vsnprintf
.type asm_test_invoke_cdecl_out_vsnprintf, @function
asm_test_invoke_cdecl_out_vsnprintf:
    pushq   %rbx
    movq    %rsp, %rbx
    andq    $-16, %rsp
    subq    $20, %rsp
    movl    %esi, 4(%rsp)
    movl    %edx, 8(%rsp)
    movl    %ecx, 12(%rsp)
    movl    %r8d, 16(%rsp)
    leal    .Lout_vsnp_ret(%rip), %eax
    movl    %eax, (%rsp)
    jmp     *%rdi
.Lout_vsnp_ret:
    movq    %rbx, %rsp
    popq    %rbx
    ret
.size asm_test_invoke_cdecl_out_vsnprintf, .-asm_test_invoke_cdecl_out_vsnprintf

/*
 * cdecl ASM callee checking %esp % 16 == 12 at function entry
 * (making [esp+4] 16-byte aligned per i386 SysV ABI).
 */
.globl asm_cdecl_check_align
.type asm_cdecl_check_align, @function
asm_cdecl_check_align:
    movl    %esp, %eax
    andl    $15, %eax
    movl    %eax, g_asm_entry_esp_mod16(%rip)
    movl    (%rsp), %r11d
    leaq    4(%rsp), %rsp
    jmp     *%r11
.size asm_cdecl_check_align, .-asm_cdecl_check_align

/*
 * Helper: uint32_t asm_read_gs_u32(uint32_t offset)
 */
.globl asm_read_gs_u32
.type asm_read_gs_u32, @function
asm_read_gs_u32:
    movl    %edi, %eax
    movl    %gs:(%rax), %eax
    ret
.size asm_read_gs_u32, .-asm_read_gs_u32

/*
 * cdecl ASM signal handler: void asm_cdecl_sig_handler(int signum)
 */
.globl asm_cdecl_sig_handler
.type asm_cdecl_sig_handler, @function
asm_cdecl_sig_handler:
    movl    4(%rsp), %eax
    movl    %eax, g_asm_sig_got(%rip)
    movl    (%rsp), %r11d
    leaq    4(%rsp), %rsp
    jmp     *%r11
.size asm_cdecl_sig_handler, .-asm_cdecl_sig_handler

.section .rodata
.p2align 3
.Lconst_2_5:
    .double 2.5

.bss
.p2align 4
.globl g_asm_atexit_seq
g_asm_atexit_seq:
    .space 32
.globl g_asm_atexit_pos
g_asm_atexit_pos:
    .space 4
.globl g_asm_init_argc
g_asm_init_argc:
    .space 4
.globl g_asm_init_argv
g_asm_init_argv:
    .space 4
.globl g_asm_init_envp
g_asm_init_envp:
    .space 4
.globl g_asm_sig_got
g_asm_sig_got:
    .space 4
.globl g_asm_entry_esp_mod16
g_asm_entry_esp_mod16:
    .space 4

.section .note.GNU-stack,"",@progbits
