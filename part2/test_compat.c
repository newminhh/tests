#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdbit.h>
#include <stdckdint.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "../../src/common/newminhh_abi.h"
#include "../../src/common/newminhh_log.h"

/* Assembly callees & helpers in test_compat_asm.S */
extern void asm_cdecl_add2(void);
extern void asm_cdecl_f64_mul_add(void);
extern void asm_cdecl_i64_combine(void);
extern void asm_cdecl_qsort_cmp_i32(void);
extern void asm_cdecl_pthread_worker(void);
extern void asm_cdecl_atexit_1(void);
extern void asm_cdecl_atexit_2(void);
extern void asm_cdecl_cxa_handler(void);
extern void asm_cdecl_pool_8i(void);
extern void asm_cdecl_init_fn(void);
extern void asm_cdecl_sig_handler(void);
extern void asm_cdecl_check_align(void);

extern uint64_t asm_test_call_x32_cdecl_i64(void *fn, const nm_fn_sig_t *sig, const uint32_t *args);
extern double asm_test_call_x32_cdecl_fp(void *fn, const nm_fn_sig_t *sig, const uint32_t *args);
extern uint32_t asm_test_invoke_cdecl_out_1ptr(void *tramp, const char *s);
extern uint64_t asm_test_invoke_cdecl_out_strtoull(void *tramp, const char *nptr, char **endptr, int base);
extern double asm_test_invoke_cdecl_out_1f64(void *tramp, double x);
extern float asm_test_invoke_cdecl_out_1f32(void *tramp, float x);
extern int asm_test_invoke_cdecl_out_snprintf(void *tramp, char *buf, uint32_t sz,
                                              const char *fmt, int32_t i, double d, const char *s);
extern int asm_test_invoke_cdecl_out_vsnprintf(void *tramp, char *buf, uint32_t sz,
                                               const char *fmt, const uint32_t *va_words);
extern uint32_t asm_read_gs_u32(uint32_t offset);

alignas(16) extern char g_asm_atexit_seq[32];
extern int g_asm_atexit_pos;
extern uint32_t g_asm_init_argc;
extern uint32_t g_asm_init_argv;
extern uint32_t g_asm_init_envp;
extern volatile int g_asm_sig_got;
extern volatile uint32_t g_asm_entry_esp_mod16;

/* Native x32 target functions for outgoing marshaling tests */
static int32_t target_i32_sum3(int32_t a, int32_t b, const char *s) {
    return a * 10 + b + (int32_t)strlen(s);
}

static uint64_t target_u64_mix(uint64_t a, int32_t b, uint64_t c) {
    return (a ^ c) + (uint64_t)(int64_t)b;
}

static float target_f32_calc(float a, double b, int32_t c) {
    return a * 2.0f + (float)b + (float)c;
}

static double target_f64_10args(int a0, int a1, int a2, int a3, int a4, int a5,
                                int a6_spill, int64_t a7_spill,
                                double d0, double d1) {
    return (double)(a0 + a1 + a2 + a3 + a4 + a5 + a6_spill) +
           (double)a7_spill + d0 * 3.0 + d1 * 5.0;
}

static double target_10xmms(double x0, double x1, double x2, double x3,
                            double x4, double x5, double x6, double x7,
                            double x8_spill, float x9_spill) {
    return x0 + x1 + x2 + x3 + x4 + x5 + x6 + x7 + x8_spill * 2.0 + (double)x9_spill * 4.0;
}

/* Mock DBT invoker & DL backend for Part 3/4 integration checks */
static uint32_t g_mock_dbt_last_va = 0;
static uint32_t g_mock_dbt_last_w0 = 0;
static uint32_t g_mock_dbt_last_w1 = 0;
static int g_mock_dbt_calls = 0;

static int mock_guest_invoker(uint32_t guest_fn_va, const nm_fn_sig_t *sig,
                              const uint32_t *cdecl_words, int nwords,
                              uint64_t *out_gpr, double *out_fp) {
    (void)sig;
    g_mock_dbt_calls++;
    g_mock_dbt_last_va = guest_fn_va;
    g_mock_dbt_last_w0 = (nwords > 0) ? cdecl_words[0] : 0;
    g_mock_dbt_last_w1 = (nwords > 1) ? cdecl_words[1] : 0;
    if (out_gpr) *out_gpr = g_mock_dbt_last_w0 * 5u + g_mock_dbt_last_w1 * 11u;
    if (out_fp) *out_fp = 0.0;
    return NM_OK;
}

static int g_mock_dl_calls = 0;
static void *mock_dbt_dlopen(const char *path, int flags) {
    (void)path; (void)flags;
    g_mock_dl_calls++;
    return (void *)(uintptr_t)0xCAFEBABEu;
}
static void *mock_dbt_dlsym(void *handle, const char *sym) {
    if ((uintptr_t)handle == 0xCAFEBABEu && strcmp(sym, "guest_sym") == 0) {
        return (void *)(uintptr_t)0xDEADBEEFu;
    }
    return nullptr;
}
static int mock_dbt_dlclose(void *handle) {
    return ((uintptr_t)handle == 0xCAFEBABEu) ? 0 : -1;
}
static const char *mock_dbt_dlerror(void) {
    return "mock dbt dlerror";
}

static uint32_t mock_brk_hook(uint32_t req) {
    static uint32_t cur = 0x09000000u;
    if (req >= 0x09000000u && req < 0x0A000000u) cur = req;
    return cur;
}

/* Helper to push two uint32_t slots from a double/int64 */
static void push_u64(uint32_t *buf, int *pos, uint64_t v) {
    buf[(*pos)++] = (uint32_t)(v & 0xFFFFFFFFULL);
    buf[(*pos)++] = (uint32_t)(v >> 32);
}

static void push_f64(uint32_t *buf, int *pos, double d) {
    uint64_t v = 0;
    memcpy(&v, &d, sizeof(v));
    push_u64(buf, pos, v);
}

static void push_f32(uint32_t *buf, int *pos, float f) {
    uint32_t v = 0;
    memcpy(&v, &f, sizeof(v));
    buf[(*pos)++] = v;
}

static void test_outgoing_marshaling(void) {
    /* 1. i32 + ptr -> i32 */
    nm_fn_sig_t s1 = {
        .ret_kind = NM_RET_I32,
        .nargs = 3,
        .arg_kinds = {NM_ARG_I32, NM_ARG_I32, NM_ARG_PTR}
    };
    const char *msg = "hello";
    uint32_t a1[3] = {(uint32_t)-4, 50, (uint32_t)(uintptr_t)msg};
    uint64_t r1 = newminhh_call_x32_sig(NM_FN_TO_VOIDP(target_i32_sum3), &s1, a1, nullptr);
    assert((int32_t)r1 == (-4 * 10 + 50 + 5));

    /* 2. u64 + i32 + u64 -> u64 (via EDX:EAX cdecl bridge) */
    nm_fn_sig_t s2 = {
        .ret_kind = NM_RET_U64,
        .nargs = 3,
        .arg_kinds = {NM_ARG_U64, NM_ARG_I32, NM_ARG_U64}
    };
    uint32_t a2[8];
    int p2 = 0;
    uint64_t u_a = 0x1122334455667788ULL;
    uint64_t u_c = 0x0102030405060708ULL;
    push_u64(a2, &p2, u_a);
    a2[p2++] = (uint32_t)-5;
    push_u64(a2, &p2, u_c);
    uint64_t r2 = asm_test_call_x32_cdecl_i64(NM_FN_TO_VOIDP(target_u64_mix), &s2, a2);
    assert(r2 == ((u_a ^ u_c) + (uint64_t)(int64_t)-5));

    /* 3. f32 + f64 + i32 -> f32 (via XMM0 <-> x87 ST(0)) */
    nm_fn_sig_t s3 = {
        .ret_kind = NM_RET_F32,
        .nargs = 3,
        .arg_kinds = {NM_ARG_F32, NM_ARG_F64, NM_ARG_I32}
    };
    uint32_t a3[4];
    int p3 = 0;
    push_f32(a3, &p3, 3.25f);
    push_f64(a3, &p3, 4.5);
    a3[p3++] = 10;
    double r3 = asm_test_call_x32_cdecl_fp(NM_FN_TO_VOIDP(target_f32_calc), &s3, a3);
    assert(fabs(r3 - 21.0) < 1e-5);

    /* 4. Stack spill > 6 GPR args + mixed doubles -> f64 (x87 ST(0)) */
    nm_fn_sig_t s4 = {
        .ret_kind = NM_RET_F64,
        .nargs = 10,
        .arg_kinds = {
            NM_ARG_I32, NM_ARG_I32, NM_ARG_I32, NM_ARG_I32,
            NM_ARG_I32, NM_ARG_I32, NM_ARG_I32, NM_ARG_I64,
            NM_ARG_F64, NM_ARG_F64
        }
    };
    uint32_t a4[16];
    int p4 = 0;
    for (int i = 1; i <= 7; i++) a4[p4++] = (uint32_t)i;
    push_u64(a4, &p4, 1000LL);
    push_f64(a4, &p4, 2.5);
    push_f64(a4, &p4, 4.0);
    double r4 = asm_test_call_x32_cdecl_fp(NM_FN_TO_VOIDP(target_f64_10args), &s4, a4);
    assert(fabs(r4 - (28.0 + 1000.0 + 7.5 + 20.0)) < 1e-9);

    /* 5. Stack spill > 8 XMM args */
    nm_fn_sig_t s5 = {
        .ret_kind = NM_RET_F64,
        .nargs = 10,
        .arg_kinds = {
            NM_ARG_F64, NM_ARG_F64, NM_ARG_F64, NM_ARG_F64,
            NM_ARG_F64, NM_ARG_F64, NM_ARG_F64, NM_ARG_F64,
            NM_ARG_F64, NM_ARG_F32
        }
    };
    uint32_t a5[24];
    int p5 = 0;
    for (int i = 1; i <= 9; i++) push_f64(a5, &p5, (double)i);
    push_f32(a5, &p5, 2.5f);
    double r5 = asm_test_call_x32_cdecl_fp(NM_FN_TO_VOIDP(target_10xmms), &s5, a5);
    /* 1+2+3+4+5+6+7+8 + 9*2 + 2.5*4 = 36 + 18 + 10 = 64 */
    assert(fabs(r5 - 64.0) < 1e-9);

    /* 6. Outgoing 16B trampoline slot invoked with 4B cdecl stride (U32, U64, F64, F32) */
    nm_sym_entry_t strlen_ent;
    assert(newminhh_compat_lookup_wrapper("strlen", &strlen_ent) == NM_OK);
    void *out_tramp = newminhh_alloc_out_trampoline(strlen_ent.wrapper_fn, &strlen_ent.sig);
    assert(out_tramp != nullptr);
    uint32_t len = asm_test_invoke_cdecl_out_1ptr(out_tramp, "newminhh_x32");
    assert(len == 12);

    nm_sym_entry_t strtoull_ent;
    assert(newminhh_compat_lookup_wrapper("strtoull", &strtoull_ent) == NM_OK);
    void *out_strtoull = newminhh_alloc_out_trampoline(strtoull_ent.wrapper_fn, &strtoull_ent.sig);
    assert(out_strtoull != nullptr);
    char *u64_end = nullptr;
    uint64_t u64_val = asm_test_invoke_cdecl_out_strtoull(out_strtoull, "0x123456789ABCDEF0", &u64_end, 16);
    assert(u64_val == 0x123456789ABCDEF0ULL && u64_end != nullptr && *u64_end == '\0');

    nm_sym_entry_t sqrt_ent;
    assert(newminhh_compat_lookup_wrapper("sqrt", &sqrt_ent) == NM_OK);
    void *out_sqrt = newminhh_alloc_out_trampoline(sqrt_ent.wrapper_fn, &sqrt_ent.sig);
    assert(out_sqrt != nullptr);
    double sqrt_val = asm_test_invoke_cdecl_out_1f64(out_sqrt, 2.0);
    assert(fabs(sqrt_val - sqrt(2.0)) < 1e-12);

    nm_sym_entry_t sqrtf_ent;
    assert(newminhh_compat_lookup_wrapper("sqrtf", &sqrtf_ent) == NM_OK);
    void *out_sqrtf = newminhh_alloc_out_trampoline(sqrtf_ent.wrapper_fn, &sqrtf_ent.sig);
    assert(out_sqrtf != nullptr);
    float sqrtf_val = asm_test_invoke_cdecl_out_1f32(out_sqrtf, 9.0f);
    assert(fabsf(sqrtf_val - 3.0f) < 1e-6f);

    printf("PASS test_outgoing_marshaling\n");
}

static void test_incoming_bridge(void) {
    /* 1. Direct newminhh_in_bridge -> asm_cdecl_add2 (EAX) */
    nm_fn_sig_t s_add = {
        .ret_kind = NM_RET_I32,
        .nargs = 2,
        .arg_kinds = {NM_ARG_I32, NM_ARG_I32}
    };
    uint64_t gprs[6] = {10, 5, 0, 0, 0, 0};
    uint64_t r_add = newminhh_in_bridge((uint32_t)(uintptr_t)asm_cdecl_add2,
                                        &s_add, gprs, nullptr, nullptr,
                                        NM_CB_MODE_NATIVE_CDECL, nullptr);
    assert((int32_t)r_add == (10 * 3 + 5 * 7));

    /* 2. Direct newminhh_in_bridge -> asm_cdecl_f64_mul_add (x87 ST(0)) */
    nm_fn_sig_t s_fp = {
        .ret_kind = NM_RET_F64,
        .nargs = 2,
        .arg_kinds = {NM_ARG_F64, NM_ARG_F32}
    };
    double da = 6.0;
    float fb = 1.5f;
    uint64_t xmms[8] = {0};
    memcpy(&xmms[0], &da, sizeof(da));
    memcpy(&xmms[1], &fb, sizeof(fb));
    double fp_out = 0.0;
    newminhh_in_bridge((uint32_t)(uintptr_t)asm_cdecl_f64_mul_add,
                       &s_fp, nullptr, xmms, nullptr,
                       NM_CB_MODE_NATIVE_CDECL, &fp_out);
    assert(fabs(fp_out - (6.0 * 2.5 + 1.5)) < 1e-9);

    /* 3. Direct newminhh_in_bridge -> asm_cdecl_i64_combine (EDX:EAX) */
    nm_fn_sig_t s_i64 = {
        .ret_kind = NM_RET_I64,
        .nargs = 2,
        .arg_kinds = {NM_ARG_I32, NM_ARG_U32}
    };
    uint64_t g64[6] = {0x12345678u, 0x9ABCDEF0u, 0, 0, 0, 0};
    uint64_t r_i64 = newminhh_in_bridge((uint32_t)(uintptr_t)asm_cdecl_i64_combine,
                                        &s_i64, g64, nullptr, nullptr,
                                        NM_CB_MODE_NATIVE_CDECL, nullptr);
    assert(r_i64 == 0x123456789ABCDEF0ULL);

    /* 4. .so export trampoline + .init_array / .fini_array helpers */
    typedef int (*add2_fn_t)(int, int);
    add2_fn_t exp_add = NM_VOIDP_TO_FN(add2_fn_t, newminhh_wrap_so_export(
        (uint32_t)(uintptr_t)asm_cdecl_add2, &s_add, NM_CB_MODE_NATIVE_CDECL));
    assert(exp_add != nullptr);
    assert(exp_add(4, 6) == (4 * 3 + 6 * 7));

    typedef double (*fp_fn_t)(double, float);
    fp_fn_t exp_fp = NM_VOIDP_TO_FN(fp_fn_t, newminhh_wrap_so_export(
        (uint32_t)(uintptr_t)asm_cdecl_f64_mul_add, &s_fp, NM_CB_MODE_NATIVE_CDECL));
    assert(exp_fp != nullptr);
    assert(fabs(exp_fp(4.0, 3.0f) - 13.0) < 1e-9);

    typedef void (*init_fn_t)(int, char **, char **);
    init_fn_t init_w = NM_VOIDP_TO_FN(init_fn_t, newminhh_wrap_init_array_fn(
        (uint32_t)(uintptr_t)asm_cdecl_init_fn, NM_CB_MODE_NATIVE_CDECL));
    assert(init_w != nullptr);
    char *fake_argv[2] = {"prog", nullptr};
    char *fake_envp[1] = {nullptr};
    init_w(1, fake_argv, fake_envp);
    assert(g_asm_init_argc == 1);
    assert(g_asm_init_argv == (uint32_t)(uintptr_t)fake_argv);
    assert(g_asm_init_envp == (uint32_t)(uintptr_t)fake_envp);

    g_asm_atexit_pos = 0;
    memset(g_asm_atexit_seq, 0, sizeof(g_asm_atexit_seq));
    typedef void (*fini_fn_t)(void);
    fini_fn_t fini_w = NM_VOIDP_TO_FN(fini_fn_t, newminhh_wrap_fini_array_fn(
        (uint32_t)(uintptr_t)asm_cdecl_atexit_1, NM_CB_MODE_NATIVE_CDECL));
    assert(fini_w != nullptr);
    fini_w();
    assert(g_asm_atexit_pos == 1 && g_asm_atexit_seq[0] == '1');

    /* 5. Verify %esp % 16 == 12 at callee entry in newminhh_invoke_cdecl_asm ([esp+4] 16B aligned) */
    const int align_nargs[5] = {0, 1, 2, 3, 5};
    for (int i = 0; i < 5; i++) {
        nm_fn_sig_t s_align = {.ret_kind = NM_RET_U32, .nargs = (uint8_t)align_nargs[i]};
        for (int j = 0; j < align_nargs[i]; j++) {
            s_align.arg_kinds[j] = NM_ARG_U32;
        }
        g_asm_entry_esp_mod16 = 0xFFFFFFFFu;
        uint64_t g_al[6] = {1, 2, 3, 4, 5, 6};
        uint64_t mod16 = newminhh_in_bridge((uint32_t)(uintptr_t)asm_cdecl_check_align,
                                            &s_align, g_al, nullptr, nullptr,
                                            NM_CB_MODE_NATIVE_CDECL, nullptr);
        assert(mod16 == 12u);
        assert(g_asm_entry_esp_mod16 == 12u);
    }

    printf("PASS test_incoming_bridge\n");
}

static void test_callbacks_and_trampoline_pool(void) {
    /* 1. qsort + bsearch with real cdecl ASM comparator */
    int32_t arr[7] = {50, -10, 42, 7, 99, 0, 15};
    compat_qsort(arr, 7, sizeof(int32_t), (uint32_t)(uintptr_t)asm_cdecl_qsort_cmp_i32);
    int32_t expected[7] = {-10, 0, 7, 15, 42, 50, 99};
    assert(memcmp(arr, expected, sizeof(arr)) == 0);

    int32_t key = 42;
    int32_t *found = (int32_t *)compat_bsearch(&key, arr, 7, sizeof(int32_t),
                                               (uint32_t)(uintptr_t)asm_cdecl_qsort_cmp_i32);
    assert(found == &arr[4] && *found == 42);

    int32_t missing_key = 1234;
    assert(compat_bsearch(&missing_key, arr, 7, sizeof(int32_t),
                          (uint32_t)(uintptr_t)asm_cdecl_qsort_cmp_i32) == nullptr);

    /* 2. pthread_create with GS base propagation + 24-byte i386 mutex */
    uint32_t fake_tls[4] = {0x11110000u, 0x00002222u, 0, 0};
    newminhh_user_desc32_t udesc = {
        .entry_number = (uint32_t)-1,
        .base_addr = (uint32_t)(uintptr_t)fake_tls,
        .limit = 0xFFFFFu,
        .flags = 0x51u
    };
    assert(newminhh_compat_syscall(243 /* set_thread_area */,
                                   (uint32_t)(uintptr_t)&udesc, 0, 0, 0, 0, 0) == 0);
    assert(udesc.entry_number >= 6 && udesc.entry_number <= 8);

    uint32_t th = 0;
    uint32_t th_out[2] = {0, 0};
    uint8_t i386_mutex[24] = {0}; /* 24B i386 pthread_mutex_t size check */
    assert(compat_pthread_mutex_lock(i386_mutex) == 0);
    assert(compat_pthread_mutex_unlock(i386_mutex) == 0);
    assert(compat_pthread_mutex_destroy(i386_mutex) == 0);

    assert(compat_pthread_create(&th, nullptr,
                                 (uint32_t)(uintptr_t)asm_cdecl_pthread_worker,
                                 (uint32_t)(uintptr_t)th_out) == 0);
    void *th_ret = nullptr;
    assert(compat_pthread_join(th, &th_ret) == 0);
    assert(th_out[0] == 0x11112222u);
    assert((uint32_t)(uintptr_t)th_ret == 0x11112222u + 100u);

    /* 3. LIFO atexit & __cxa_atexit */
    g_asm_atexit_pos = 0;
    memset(g_asm_atexit_seq, 0, sizeof(g_asm_atexit_seq));
    assert(compat_atexit((uint32_t)(uintptr_t)asm_cdecl_atexit_1) == 0);
    assert(compat_atexit((uint32_t)(uintptr_t)asm_cdecl_atexit_2) == 0);
    assert(compat___cxa_atexit((uint32_t)(uintptr_t)asm_cdecl_cxa_handler, 'X', 0xABCDu) == 0);
    compat___cxa_finalize(0xABCDu);
    assert(g_asm_atexit_pos == 1 && g_asm_atexit_seq[0] == 'X');
    newminhh_run_guest_atexits();
    assert(g_asm_atexit_pos == 3);
    assert(g_asm_atexit_seq[1] == '2' && g_asm_atexit_seq[2] == '1');

    /* 4. Pooled generic callback trampoline (8 integer args -> stack spill) */
    nm_fn_sig_t s_pool8 = {
        .ret_kind = NM_RET_I32,
        .nargs = 8,
        .arg_kinds = {
            NM_ARG_I32, NM_ARG_I32, NM_ARG_I32, NM_ARG_I32,
            NM_ARG_I32, NM_ARG_I32, NM_ARG_I32, NM_ARG_I32
        }
    };
    typedef int (*fn8_t)(int, int, int, int, int, int, int, int);
    fn8_t pfn8 = NM_VOIDP_TO_FN(fn8_t, newminhh_alloc_cb_trampoline(
        (uint32_t)(uintptr_t)asm_cdecl_pool_8i, &s_pool8, NM_CB_MODE_NATIVE_CDECL, 0));
    assert(pfn8 != nullptr);
    /* 1+2+3+4+5+6+7 + 8*10 = 28 + 80 = 108 */
    assert(pfn8(1, 2, 3, 4, 5, 6, 7, 8) == 108);

    /* 5. Pooled trampoline delegating to registered DBT guest invoker */
    newminhh_register_guest_invoker(mock_guest_invoker);
    nm_fn_sig_t s_dbt = {
        .ret_kind = NM_RET_I32,
        .nargs = 2,
        .arg_kinds = {NM_ARG_I32, NM_ARG_I32}
    };
    typedef int (*fn2_t)(int, int);
    fn2_t pfn_dbt = NM_VOIDP_TO_FN(fn2_t, newminhh_alloc_cb_trampoline(
        0x08049999u, &s_dbt, NM_CB_MODE_DBT, 0));
    assert(pfn_dbt != nullptr);
    int dbt_res = pfn_dbt(4, 3);
    assert(g_mock_dbt_calls == 1);
    assert(g_mock_dbt_last_va == 0x08049999u);
    assert(dbt_res == (4 * 5 + 3 * 11));
    newminhh_register_guest_invoker(nullptr);

    /* 6. compat_signal dispatching to cdecl ASM handler */
    g_asm_sig_got = 0;
    uint32_t prev_sig = compat_signal(SIGUSR1, (uint32_t)(uintptr_t)asm_cdecl_sig_handler);
    assert(prev_sig != (uint32_t)(uintptr_t)SIG_ERR);
    assert(raise(SIGUSR1) == 0);
    assert(g_asm_sig_got == SIGUSR1);
    assert(compat_signal(SIGUSR1, (uint32_t)(uintptr_t)SIG_DFL) != (uint32_t)(uintptr_t)SIG_ERR);

    printf("PASS test_callbacks_and_trampoline_pool\n");
}

static void test_syscalls(void) {
    /* 1. getpid */
    int32_t pid = newminhh_compat_syscall(20 /* getpid */, 0, 0, 0, 0, 0, 0);
    assert(pid == (int32_t)getpid());

    /* 2. pipe + write + read + close */
    int fds[2] = {-1, -1};
    assert(newminhh_compat_syscall(331 /* pipe2 */, (uint32_t)(uintptr_t)fds, 0, 0, 0, 0, 0) == 0);
    const char *wmsg = "part2-syscall-ok";
    assert(newminhh_compat_syscall(4 /* write */, (uint32_t)fds[1],
                                   (uint32_t)(uintptr_t)wmsg, (uint32_t)strlen(wmsg), 0, 0, 0) == (int32_t)strlen(wmsg));
    char rbuf[32] = {0};
    assert(newminhh_compat_syscall(3 /* read */, (uint32_t)fds[0],
                                   (uint32_t)(uintptr_t)rbuf, sizeof(rbuf) - 1, 0, 0, 0) == (int32_t)strlen(wmsg));
    assert(strcmp(rbuf, wmsg) == 0);
    assert(newminhh_compat_syscall(6 /* close */, (uint32_t)fds[0], 0, 0, 0, 0, 0) == 0);
    assert(newminhh_compat_syscall(6 /* close */, (uint32_t)fds[1], 0, 0, 0, 0, 0) == 0);

    /* 3. mmap2 (pgoffset << 12) + munmap */
    char tmp_path[128];
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/newminhh_p2_mmap_%d.bin", (int)getpid());
    int fd = open(tmp_path, O_CREAT | O_RDWR | O_TRUNC, S_IRUSR | S_IWUSR);
    assert(fd >= 0);
    uint8_t page0[4096], page1[4096];
    memset(page0, 0x11, sizeof(page0));
    memset(page1, 0xA5, sizeof(page1));
    assert(write(fd, page0, sizeof(page0)) == sizeof(page0));
    assert(write(fd, page1, sizeof(page1)) == sizeof(page1));

    int32_t map_ret = newminhh_compat_syscall(192 /* mmap2 */, 0, 4096,
                                              PROT_READ, MAP_PRIVATE,
                                              (uint32_t)fd, 1 /* pgoffset=1 -> byte 4096 */);
    assert((uint32_t)map_ret < 0xfffff000u);
    const uint8_t *mapped = (const uint8_t *)(uintptr_t)(uint32_t)map_ret;
    assert(mapped[0] == 0xA5 && mapped[4095] == 0xA5);
    assert(newminhh_compat_syscall(91 /* munmap */, (uint32_t)map_ret, 4096, 0, 0, 0, 0) == 0);
    close(fd);

    /* 4. uname */
    struct utsname uts;
    memset(&uts, 0, sizeof(uts));
    assert(newminhh_compat_syscall(122 /* uname */, (uint32_t)(uintptr_t)&uts, 0, 0, 0, 0, 0) == 0);
    assert(strcmp(uts.sysname, "Linux") == 0);

    /* 5. stat64 + stat32 + clock_gettime + getrlimit */
    newminhh_stat64_t st64;
    memset(&st64, 0, sizeof(st64));
    assert(newminhh_compat_syscall(195 /* stat64 */, (uint32_t)(uintptr_t)tmp_path,
                                   (uint32_t)(uintptr_t)&st64, 0, 0, 0, 0) == 0);
    assert(st64.st_size == 8192);
    assert(S_ISREG(st64.st_mode));
    unlink(tmp_path);

    newminhh_timespec32_t ts32 = {0, -1};
    assert(newminhh_compat_syscall(265 /* clock_gettime */, CLOCK_MONOTONIC,
                                   (uint32_t)(uintptr_t)&ts32, 0, 0, 0, 0) == 0);
    assert(ts32.tv_nsec >= 0);

    newminhh_timespec64_t32_t ts64 = {0, -1, 0};
    assert(newminhh_compat_syscall(403 /* clock_gettime64 */, CLOCK_MONOTONIC,
                                   (uint32_t)(uintptr_t)&ts64, 0, 0, 0, 0) == 0);
    assert(ts64.tv_nsec >= 0);

    newminhh_rlimit32_t rl32 = {0, 0};
    assert(newminhh_compat_syscall(191 /* ugetrlimit */, RLIMIT_NOFILE,
                                   (uint32_t)(uintptr_t)&rl32, 0, 0, 0, 0) == 0);
    assert(rl32.rlim_cur > 0 && rl32.rlim_cur <= rl32.rlim_max);

    /* 6. set_thread_area -> arch_prctl(ARCH_SET_GS) + %gs:0 read + cpu state */
    uint32_t tls_mem[4] = {0xCAFE0001u, 0xCAFE0002u, 0, 0};
    newminhh_user_desc32_t desc = {
        .entry_number = 6,
        .base_addr = (uint32_t)(uintptr_t)tls_mem,
        .limit = 0xFFFFFu,
        .flags = 0x51u
    };
    newminhh_cpu_t cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.gpr[NM_REG_EAX] = 243; /* set_thread_area */
    cpu.gpr[NM_REG_EBX] = (uint32_t)(uintptr_t)&desc;
    assert(newminhh_compat_syscall_cpu(&cpu) == 0);
    assert(cpu.gs_base == (uint32_t)(uintptr_t)tls_mem);
    assert(cpu.tls_bases[0] == (uint32_t)(uintptr_t)tls_mem);
    assert(asm_read_gs_u32(0) == 0xCAFE0001u);
    assert(asm_read_gs_u32(4) == 0xCAFE0002u);

    /* 7. brk hook isolation */
    newminhh_register_brk_hook(mock_brk_hook);
    assert((uint32_t)newminhh_compat_syscall(45 /* brk */, 0, 0, 0, 0, 0, 0) == 0x09000000u);
    assert((uint32_t)newminhh_compat_syscall(45 /* brk */, 0x09004000u, 0, 0, 0, 0, 0) == 0x09004000u);
    newminhh_register_brk_hook(nullptr);

    /* 8. Unsupported syscall returns -ENOSYS and logs diagnostic */
    int32_t unsup = newminhh_compat_syscall(9999u, 1, 2, 3, 4, 5, 6);
    assert(unsup == -ENOSYS);

    printf("PASS test_syscalls\n");
}

static void test_struct_repacking(void) {
    struct stat src;
    memset(&src, 0, sizeof(src));
    src.st_dev = 0x1234;
    src.st_ino = 0x87654321ULL;
    src.st_mode = S_IFREG | (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    src.st_nlink = 7;
    src.st_uid = 1000;
    src.st_gid = 1000;
    src.st_rdev = 0;
    src.st_size = 123456;
    src.st_blksize = 4096;
    src.st_blocks = 248;
    src.st_atim.tv_sec = 1700000001LL;
    src.st_atim.tv_nsec = 111222333L;
    src.st_mtim.tv_sec = 1700000002LL;
    src.st_mtim.tv_nsec = 444555666L;
    src.st_ctim.tv_sec = 1700000003LL;
    src.st_ctim.tv_nsec = 777888999L;

    newminhh_stat32_t s32;
    assert(newminhh_repack_stat_to_stat32(&src, &s32) == NM_OK);
    assert(s32.st_mode == (S_IFREG | (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)));
    assert(s32.st_nlink == 7);
    assert(s32.st_size == 123456);
    assert(s32.st_ino == 0x87654321U);
    assert(s32.st_mtim.tv_sec == 1700000002);

    /* Unaligned buffer check for stat64 (96B) and stat64_t64 (108B) */
    alignas(4) uint8_t unaligned_buf[128];
    newminhh_stat64_t *ps64 = (newminhh_stat64_t *)(unaligned_buf + 4);
    src.st_ino = 0x123456789ABCDEF0ULL;
    src.st_size = 0x100000001LL; /* > 4 GiB */
    assert(newminhh_repack_stat_to_stat64(&src, ps64) == NM_OK);
    assert(ps64->st_ino == 0x123456789ABCDEF0ULL);
    assert(ps64->st_size == 0x100000001LL);
    assert(ps64->st_mode == (S_IFREG | (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)) && ps64->st_nlink == 7);

    /* EOVERFLOW check on stat32 when st_size > INT32_MAX */
    errno = 0;
    assert(newminhh_repack_stat_to_stat32(&src, &s32) == NM_ERR_ABI);
    assert(errno == EOVERFLOW);

    /* t64 stat with > 2038 timestamp */
    src.st_mtim.tv_sec = 3000000000LL;
    newminhh_stat64_t64_t s_t64;
    assert(newminhh_repack_stat_to_stat64_t64(&src, &s_t64) == NM_OK);
    assert(s_t64.st_mtim.tv_sec == 3000000000LL);
    assert(s_t64.st_size == 0x100000001LL);

    /* timespec32 EOVERFLOW check */
    struct timespec xts = { .tv_sec = 3000000000LL, .tv_nsec = 123 };
    newminhh_timespec32_t ts32;
    assert(newminhh_repack_timespec_to_32(&xts, &ts32) == NM_ERR_ABI);

    newminhh_timespec64_t32_t ts64;
    assert(newminhh_repack_timespec_to_64t32(&xts, &ts64) == NM_OK);
    struct timespec xts_back;
    assert(newminhh_repack_timespec_from_64t32(&ts64, &xts_back) == NM_OK);
    assert(xts_back.tv_sec == 3000000000LL && xts_back.tv_nsec == 123L);

    /* rlimit32 RLIM_INFINITY <-> 0xFFFFFFFFU */
    struct rlimit rl_inf = { .rlim_cur = 1024, .rlim_max = RLIM_INFINITY };
    newminhh_rlimit32_t rl32;
    assert(newminhh_repack_rlimit_to_32(&rl_inf, &rl32) == NM_OK);
    assert(rl32.rlim_cur == 1024U && rl32.rlim_max == 0xFFFFFFFFU);
    struct rlimit rl_back;
    assert(newminhh_repack_rlimit_from_32(&rl32, &rl_back) == NM_OK);
    assert(rl_back.rlim_cur == 1024 && rl_back.rlim_max == RLIM_INFINITY);

    /* NULL pointer rejection */
    void *volatile null_p = nullptr;
    assert(newminhh_repack_stat_to_stat32((const struct stat *)null_p, &s32) == NM_ERR_ABI);
    assert(newminhh_repack_timespec_to_32(&xts, (newminhh_timespec32_t *)null_p) == NM_ERR_ABI);
    assert(newminhh_repack_rlimit_to_64t32((const struct rlimit *)null_p, (newminhh_rlimit64_t32_t *)null_p) == NM_ERR_ABI);

    printf("PASS test_struct_repacking\n");
}

static void test_dl_and_wrappers(void) {
    /* 1. Positive libc.so.6 + libm.so.6 dlopen/dlsym */
    void *h_libc = compat_dlopen("libc.so.6", RTLD_NOW);
    assert(h_libc != nullptr);
    typedef uint32_t (*fn_strlen_t)(const char *);
    fn_strlen_t fn_strlen = NM_VOIDP_TO_FN(fn_strlen_t, compat_dlsym(h_libc, "strlen"));
    assert(fn_strlen != nullptr);
    assert(fn_strlen("abcd") == 4);

    FILE **p_stdout = (FILE **)compat_dlsym(h_libc, "stdout");
    assert(p_stdout != nullptr && *p_stdout == stdout);

    void *h_libm = compat_dlopen("libm.so.6", RTLD_NOW);
    assert(h_libm != nullptr);
    typedef double (*fn_math1_t)(double);
    fn_math1_t fn_sqrt = NM_VOIDP_TO_FN(fn_math1_t, compat_dlsym(h_libm, "sqrt"));
    fn_math1_t fn_cos = NM_VOIDP_TO_FN(fn_math1_t, compat_dlsym(h_libm, "cos"));
    assert(fn_sqrt != nullptr && fn_cos != nullptr);
    assert(fabs(fn_sqrt(25.0) - 5.0) < 1e-9);
    assert(fabs(fn_cos(0.0) - 1.0) < 1e-9);

    /* 2. Negative: unsupported native symbol MUST return NULL + descriptive dlerror */
    compat_dlerror(); /* clear */
    void *bad_sym = compat_dlsym(h_libc, "gnu_get_libc_version");
    const char *err = compat_dlerror();
    assert(bad_sym == nullptr && err != nullptr && strstr(err, "unsupported") != nullptr);

    /* 3. Negative: nonexistent .so */
    void *bad_lib = compat_dlopen("/nonexistent/libmissing_newminhh.so", RTLD_NOW);
    err = compat_dlerror();
    assert(bad_lib == nullptr && err != nullptr);

    /* 4. Negative: forged/invalid handle + double dlclose */
    assert(compat_dlsym((void *)(uintptr_t)0x123456u, "strlen") == nullptr);
    assert(compat_dlerror() != nullptr);
    assert(compat_dlclose(h_libm) == 0);
    assert(compat_dlclose(h_libm) != 0);
    assert(compat_dlerror() != nullptr);
    assert(compat_dlclose(h_libc) == 0);

    /* 5. Wrapper lookup API negative tests */
    nm_sym_entry_t ent;
    assert(newminhh_compat_lookup_wrapper("nonexistent_sym_xyz", &ent) == NM_ERR_DLSYM);
    const char *volatile null_sym_name = nullptr;
    assert(newminhh_compat_lookup_wrapper(null_sym_name, &ent) == NM_ERR_ABI);

    /* 6. cdecl_mode dlopen + compat_dlsym_cdecl outgoing trampolines (incl. CDECL_DIRECT, U64, F64) */
    void *h_cdecl = compat_dlopen("libc.so.6", RTLD_NOW | NM_RTLD_CDECL);
    assert(h_cdecl != nullptr);
    void *tramp_strlen = compat_dlsym(h_cdecl, "strlen");
    assert(tramp_strlen != nullptr);
    assert(asm_test_invoke_cdecl_out_1ptr(tramp_strlen, "tramp_ok") == 8);

    void *tramp_snp_mode = compat_dlsym(h_cdecl, "snprintf");
    assert(tramp_snp_mode != nullptr);
    assert(tramp_snp_mode != NM_FN_TO_VOIDP(compat_snprintf_cdecl));
    char snp_buf1[64] = {0};
    int snp_len1 = asm_test_invoke_cdecl_out_snprintf(tramp_snp_mode, snp_buf1, sizeof(snp_buf1),
                                                      "%d:%.2f:%s", -42, 6.25, "cdecl");
    assert(snp_len1 == 14 && strcmp(snp_buf1, "-42:6.25:cdecl") == 0);
    assert(compat_dlclose(h_cdecl) == 0);

    /* Explicit compat_dlsym_cdecl on standard RTLD_NOW handles */
    void *h_libc_d = compat_dlopen("libc.so.6", RTLD_NOW);
    void *h_libm_d = compat_dlopen("libm.so.6", RTLD_NOW);
    assert(h_libc_d != nullptr && h_libm_d != nullptr);

    void *cdecl_snp = compat_dlsym_cdecl(h_libc_d, "snprintf");
    assert(cdecl_snp != nullptr && cdecl_snp != NM_FN_TO_VOIDP(compat_snprintf_cdecl));
    char snp_buf2[64] = {0};
    int snp_len2 = asm_test_invoke_cdecl_out_snprintf(cdecl_snp, snp_buf2, sizeof(snp_buf2),
                                                      "%d:%.2f:%s", 77, 1.5, "ok");
    assert(snp_len2 == 10 && strcmp(snp_buf2, "77:1.50:ok") == 0);

    void *cdecl_strtoull = compat_dlsym_cdecl(h_libc_d, "strtoull");
    assert(cdecl_strtoull != nullptr);
    char *dl_u64_end = nullptr;
    uint64_t dl_u64 = asm_test_invoke_cdecl_out_strtoull(cdecl_strtoull, "0xFEDCBA9876543210", &dl_u64_end, 16);
    assert(dl_u64 == 0xFEDCBA9876543210ULL && dl_u64_end != nullptr && *dl_u64_end == '\0');

    void *cdecl_sqrt = compat_dlsym_cdecl(h_libm_d, "sqrt");
    assert(cdecl_sqrt != nullptr);
    double dl_sqrt = asm_test_invoke_cdecl_out_1f64(cdecl_sqrt, 144.0);
    assert(fabs(dl_sqrt - 12.0) < 1e-12);

    /* Ensure newminhh_compat_lookup_wrapper still reports NM_SYM_FLAG_CDECL_DIRECT for Part 4 DBT */
    assert(newminhh_compat_lookup_wrapper("snprintf", &ent) == NM_OK);
    assert((ent.flags & NM_SYM_FLAG_CDECL_DIRECT) != 0);
    assert(ent.wrapper_fn == NM_FN_TO_VOIDP(compat_snprintf_cdecl));

    /* 7. Minimal i386 ELF detection + DL backend delegation */
    char fake_i386_so[128];
    snprintf(fake_i386_so, sizeof(fake_i386_so), "/tmp/newminhh_fake_i386_%d.so", (int)getpid());
    int fd = open(fake_i386_so, O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR);
    assert(fd >= 0);
    Elf32_Ehdr eh;
    memset(&eh, 0, sizeof(eh));
    memcpy(eh.e_ident, ELFMAG, SELFMAG);
    eh.e_ident[EI_CLASS] = ELFCLASS32;
    eh.e_ident[EI_DATA] = ELFDATA2LSB;
    eh.e_ident[EI_VERSION] = EV_CURRENT;
    eh.e_type = ET_DYN;
    eh.e_machine = EM_386;
    eh.e_version = EV_CURRENT;
    assert(write(fd, &eh, sizeof(eh)) == sizeof(eh));
    close(fd);

    /* Without backend -> fails loudly */
    assert(compat_dlopen(fake_i386_so, RTLD_NOW) == nullptr);
    assert(compat_dlerror() != nullptr);

    /* With backend -> delegates cleanly */
    nm_dl_backend_t backend = {
        .dlopen_fn = mock_dbt_dlopen,
        .dlsym_fn = mock_dbt_dlsym,
        .dlclose_fn = mock_dbt_dlclose,
        .dlerror_fn = mock_dbt_dlerror,
        .is_guest_elf_fn = nullptr
    };
    newminhh_register_dl_backend(&backend);
    void *h_dbt = compat_dlopen(fake_i386_so, RTLD_NOW);
    assert(h_dbt != nullptr && g_mock_dl_calls == 1);
    assert(compat_dlsym(h_dbt, "guest_sym") == (void *)(uintptr_t)0xDEADBEEFu);
    assert(compat_dlsym(h_dbt, "missing_guest_sym") == nullptr);
    assert(compat_dlerror() != nullptr);
    assert(compat_dlclose(h_dbt) == 0);
    newminhh_register_dl_backend(nullptr);
    unlink(fake_i386_so);

    /* 8. compat_snprintf_cdecl, compat_vsnprintf (i386 va_list), & newminhh_get_stdio_streams */
    FILE *f_in = nullptr, *f_out = nullptr, *f_err = nullptr;
    newminhh_get_stdio_streams(&f_in, &f_out, &f_err);
    assert(f_in == stdin && f_out == stdout && f_err == stderr);

    char fmt_out[128] = {0};
    const char *fmt_str = "%d:%lld:%.2f:%s";
    const char *tag = "ok";
    uint32_t cdecl_fmt_args[10];
    int fp_pos = 0;
    cdecl_fmt_args[fp_pos++] = (uint32_t)(uintptr_t)fmt_out;
    cdecl_fmt_args[fp_pos++] = (uint32_t)sizeof(fmt_out);
    cdecl_fmt_args[fp_pos++] = (uint32_t)(uintptr_t)fmt_str;
    cdecl_fmt_args[fp_pos++] = (uint32_t)-7;
    push_u64(cdecl_fmt_args, &fp_pos, 0x100000001LL);
    push_f64(cdecl_fmt_args, &fp_pos, 3.25);
    cdecl_fmt_args[fp_pos++] = (uint32_t)(uintptr_t)tag;
    int flen = compat_snprintf_cdecl(cdecl_fmt_args);
    assert(flen > 0 && strcmp(fmt_out, "-7:4294967297:3.25:ok") == 0);

    /* compat_vsnprintf & compat_vsprintf with uint32_t i386-style va_list buffer */
    char vfmt_out[128] = {0};
    const uint32_t *i386_va_words = &cdecl_fmt_args[3];
    int vlen = compat_vsnprintf(vfmt_out, (uint32_t)sizeof(vfmt_out), fmt_str,
                                (void *)(uintptr_t)i386_va_words);
    assert(vlen == flen && strcmp(vfmt_out, "-7:4294967297:3.25:ok") == 0);

    char vs_out[128] = {0};
    int vslen = compat_vsprintf(vs_out, fmt_str, (void *)(uintptr_t)i386_va_words);
    assert(vslen == flen && strcmp(vs_out, "-7:4294967297:3.25:ok") == 0);

    void *cdecl_vsnp = compat_dlsym_cdecl(h_libc_d, "vsnprintf");
    assert(cdecl_vsnp != nullptr);
    char vsnp_asm_out[128] = {0};
    int vsnp_asm_len = asm_test_invoke_cdecl_out_vsnprintf(cdecl_vsnp, vsnp_asm_out,
                                                           (uint32_t)sizeof(vsnp_asm_out),
                                                           fmt_str, i386_va_words);
    assert(vsnp_asm_len == flen && strcmp(vsnp_asm_out, "-7:4294967297:3.25:ok") == 0);

    assert(compat_dlclose(h_libc_d) == 0);
    assert(compat_dlclose(h_libm_d) == 0);

    printf("PASS test_dl_and_wrappers\n");
}

static void test_cstd_features(void) {
    /* 0. C23 floor static_assert */
    static_assert(__STDC_VERSION__ >= 202311L, "Must be compiled under C23 floor");

    /* 1. <stdckdint.h> checked arithmetic (N2683) */
    uint32_t ckd_res = 0;
    assert(!ckd_add(&ckd_res, 100u, 200u) && ckd_res == 300u);
    assert(ckd_add(&ckd_res, 0xFFFFFFFFu, 1u));
    assert(!ckd_sub(&ckd_res, 500u, 200u) && ckd_res == 300u);
    assert(ckd_sub(&ckd_res, 10u, 20u));
    assert(!ckd_mul(&ckd_res, 10u, 30u) && ckd_res == 300u);
    assert(ckd_mul(&ckd_res, 0x80000000u, 2u));

    /* 2. <stdbit.h> bit operations (N3022) */
    assert(!stdc_has_single_bit(0u));
    assert(stdc_has_single_bit(1u));
    assert(stdc_has_single_bit(2u));
    assert(!stdc_has_single_bit(3u));
    assert(stdc_has_single_bit(4096u));
    assert(stdc_bit_ceil(0u) == 1u);
    assert(stdc_bit_ceil(1u) == 1u);
    assert(stdc_bit_ceil(2u) == 2u);
    assert(stdc_bit_ceil(3u) == 4u);
    assert(stdc_bit_ceil(4095u) == 4096u);
    assert(stdc_bit_ceil(4096u) == 4096u);

    /* 3. NM_COUNTOF decay-safe compile-time check */
    int test_arr[7] = {};
    static_assert(NM_COUNTOF(test_arr) == 7);
    constexpr size_t test_arr_cnt = NM_COUNTOF(test_arr);
    assert(test_arr_cnt == 7);

    /* 4. Structured logging __VA_OPT__(,) */
    NM_LOG_INFO("cstd", "zero extra args");
    NM_LOG_INFO("cstd", "val=%d str=%s", 42, "c23");

    /* 5. C23 binary prefix parsing (N2549/N2626) */
    char *endptr = nullptr;
    int32_t bval1 = compat___isoc23_strtol("0b1010", &endptr, 0);
    assert(bval1 == 10);
    assert(endptr != nullptr && *endptr == '\0');

    uint64_t bval2 = compat___isoc23_strtoull("0B11110000", &endptr, 2);
    assert(bval2 == 240);
    assert(endptr != nullptr && *endptr == '\0');

    /* 6. aligned_alloc and free_aligned_sized */
    void *p_align = compat_aligned_alloc(64, 256);
    assert(p_align != nullptr);
    assert(((uintptr_t)p_align & 63u) == 0);
    memset(p_align, 0xAA, 256);
    compat_free_aligned_sized(p_align, 64, 256);

    /* 7. aligned_alloc non-power-of-2 and overflow rejection */
    errno = 0;
    void *p_bad_align = compat_aligned_alloc(3, 64);
    assert(p_bad_align == nullptr);
    assert(errno == EINVAL);
    errno = 0;
    void *p_bad_align2 = compat_aligned_alloc(2, 64);
    assert(p_bad_align2 == nullptr);
    assert(errno == EINVAL);
    errno = 0;
    void *p_overflow = compat_aligned_alloc(64, 0xFFFFFFFFu);
    assert(p_overflow == nullptr);
    assert(errno == ENOMEM);

    /* 8. free_sized */
    nm_sym_entry_t ent_malloc;
    assert(newminhh_compat_lookup_wrapper("malloc", &ent_malloc) == NM_OK);
    typedef void *(*fn_malloc_t)(uint32_t);
    fn_malloc_t p_malloc = NM_VOIDP_TO_FN(fn_malloc_t, ent_malloc.wrapper_fn);
    void *p_sz = p_malloc(128);
    assert(p_sz != nullptr);
    memset(p_sz, 0x55, 128);
    compat_free_sized(p_sz, 128);

    /* 9. realloc(p, 0) returns nullptr (C17 DR 400 / C23 N2464) */
    nm_sym_entry_t ent_realloc;
    assert(newminhh_compat_lookup_wrapper("realloc", &ent_realloc) == NM_OK);
    typedef void *(*fn_realloc_t)(void *, uint32_t);
    fn_realloc_t p_realloc = NM_VOIDP_TO_FN(fn_realloc_t, ent_realloc.wrapper_fn);
    void *p_re = p_malloc(64);
    assert(p_re != nullptr);
    void *p_re_zero = p_realloc(p_re, 0);
    assert(p_re_zero == nullptr);

    void *volatile null_p = nullptr;
    const char *volatile null_str = nullptr;

    /* 10. memset_explicit */
    char explicit_buf[32];
    memset(explicit_buf, 0xEE, sizeof(explicit_buf));
    void *ret_exp = compat_memset_explicit(explicit_buf, 0, (uint32_t)sizeof(explicit_buf));
    assert(ret_exp == explicit_buf);
    for (size_t i = 0; i < sizeof(explicit_buf); i++) {
        assert(explicit_buf[i] == 0);
    }
    assert(compat_memset_explicit(null_p, 0, 0) == nullptr);
    assert(compat_memset_explicit(explicit_buf, 0, 0) == explicit_buf);

    /* 11. memchr (const-preserving & zero-length safe) */
    const char haystack[] = "abcdefghijklmnopqrstuvwxyz";
    void *chr_found = compat_memchr(haystack, 'm', (uint32_t)sizeof(haystack));
    assert(chr_found == haystack + 12);
    void *chr_not_found = compat_memchr(haystack, 'Z', (uint32_t)sizeof(haystack));
    assert(chr_not_found == nullptr);
    assert(compat_memchr(null_p, 'a', 0) == nullptr);
    assert(compat_memchr(haystack, 'a', 0) == nullptr);

    /* 12. memccpy */
    char dest[32] = {};
    void *ret_cc = compat_memccpy(dest, "hello-world", '-', 16);
    assert(ret_cc == dest + 6);
    assert(dest[5] == '-');
    assert(compat_memccpy(null_p, "a", 'a', 1) == nullptr);
    assert(compat_memccpy(dest, null_p, 'a', 1) == nullptr);
    assert(compat_memccpy(dest, "a", 'a', 0) == nullptr);

    /* 13. strndup */
    char *dup_str = compat_strndup("testing_c23_strndup", 7);
    assert(dup_str != nullptr);
    assert(strcmp(dup_str, "testing") == 0);
    compat_free_sized(dup_str, 8);
    assert(compat_strndup(null_str, 5) == nullptr && errno == EINVAL);

    /* 14. timespec_get */
    newminhh_timespec32_t ts32 = {};
    int base_ret = compat_timespec_get(&ts32, TIME_UTC);
    assert(base_ret == TIME_UTC);
    assert(ts32.tv_sec > 1700000000);
    assert(ts32.tv_nsec >= 0 && ts32.tv_nsec < 1000000000);

    /* 15. C23 fixed-width and binary printf formatting (%b, %B, %w64, %wf64) */
    char c23_fmt_buf[128] = {};
    uint32_t c23_args[16] = {};
    int c23_pos = 0;
    const char *c23_fmt = "%b %B %w8d %w16x %w32u %w64d %wf8d %wf64d %d";
    c23_args[c23_pos++] = (uint32_t)(uintptr_t)c23_fmt_buf;
    c23_args[c23_pos++] = (uint32_t)sizeof(c23_fmt_buf);
    c23_args[c23_pos++] = (uint32_t)(uintptr_t)c23_fmt;
    c23_args[c23_pos++] = 10u;               /* %b -> 1010 */
    c23_args[c23_pos++] = 15u;               /* %B -> 1111 */
    c23_args[c23_pos++] = (uint32_t)-42;     /* %w8d */
    c23_args[c23_pos++] = 0xABCDu;           /* %w16x */
    c23_args[c23_pos++] = 12345678u;         /* %w32u */
    push_u64(c23_args, &c23_pos, 0x123456789ABCDEF0ULL); /* %w64d (2 words) */
    c23_args[c23_pos++] = (uint32_t)100;     /* %wf8d */
    push_u64(c23_args, &c23_pos, 200ULL);    /* %wf64d (2 words) */
    c23_args[c23_pos++] = 999u;              /* trailing %d */
    int c23_flen = compat_snprintf_cdecl(c23_args);
    assert(c23_flen > 0);
    assert(strcmp(c23_fmt_buf, "1010 1111 -42 abcd 12345678 1311768467463790320 100 200 999") == 0);

    /* 16. C23 const-preserving search (N3020) */
    const char c_hay[] = "hello world";
    const char *c_p = strchr(c_hay, 'w');
    assert(c_p == c_hay + 6);
    char m_hay[] = "hello world";
    char *m_p = strchr(m_hay, 'w');
    assert(m_p == m_hay + 6);

    /* 17. Verify all 9 C11/C23 symbols are registered in g_sym_table */
    const char *new_syms[] = {
        "aligned_alloc", "free_sized", "free_aligned_sized", "memset_explicit",
        "memchr", "memccpy", "strndup", "timespec_get", "quick_exit",
        "__isoc23_strtol", "__isoc23_strtoul", "__isoc23_strtoll", "__isoc23_strtoull"
    };
    for (size_t i = 0; i < NM_COUNTOF(new_syms); i++) {
        nm_sym_entry_t lookup_ent;
        assert(newminhh_compat_lookup_wrapper(new_syms[i], &lookup_ent) == NM_OK);
        assert(lookup_ent.wrapper_fn != nullptr);
    }

    printf("PASS test_cstd_features\n");
}

int main(void) {
    test_outgoing_marshaling();
    test_incoming_bridge();
    test_callbacks_and_trampoline_pool();
    test_syscalls();
    test_struct_repacking();
    test_dl_and_wrappers();
    test_cstd_features();
    printf("ALL_PART2_TESTS_PASSED\n");
    return 0;
}
