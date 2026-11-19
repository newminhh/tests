#define _GNU_SOURCE
#include "../../src/dbt/dbt_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *g_i386_dir = "/root/newminhh/tests/corpus/build/i386";

/* --- 1. SIG_IGN preservation test (must run before dbt_init_once in child) --- */
static int test_sigsegv_sigign_in_child(void) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        signal(SIGSEGV, SIG_IGN);
        dbt_ensure_initialized();

        char so_path[512];
        snprintf(so_path, sizeof(so_path), "%s/libbasic_pic.so", g_i386_dir);
        uint32_t gh = dbt_guest_dlopen(so_path, 0);
        if (!gh) {
            fprintf(stderr, "child: dbt_guest_dlopen failed\n");
            _exit(2);
        }
        uint32_t fn_va = dbt_guest_dlsym(gh, "basic_add_mul");
        if (!fn_va) {
            fprintf(stderr, "child: dbt_guest_dlsym failed\n");
            _exit(3);
        }
        typedef int (*add_mul_t)(int, int, int);
        int res = ((add_mul_t)(uintptr_t)fn_va)(3, 4, 2);
        if (res == 0) {
            fprintf(stderr, "child: unexpected 0 from basic_add_mul\n");
            _exit(4);
        }
        dbt_guest_dlclose(gh);

        /* Non-guest SIGSEGV must honor SIG_IGN and NOT reset to SIG_DFL */
        raise(SIGSEGV);
        raise(SIGSEGV);
        _exit(0);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return 1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "test_sigsegv_sigign_in_child failed: status=0x%x\n", status);
        return 1;
    }
    printf("PASS test_sigsegv_sigign\n");
    return 0;
}

/* --- 2. 64K g_dbt_fast_cache collision + concurrent invalidation stress --- */
#define FC_NTHREADS 8
#define FC_ITERS    20000
#define FC_NFNS     8

typedef struct {
    int tid;
    uint32_t dispatch_va;
    uint32_t fn_vas[FC_NFNS];
    int failed;
} fc_worker_arg_t;

static void *fc_worker(void *raw) {
    fc_worker_arg_t *arg = (fc_worker_arg_t *)raw;

    uint8_t *stk = (uint8_t *)mmap(NULL, 65536, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stk == MAP_FAILED) {
        arg->failed = 1;
        return NULL;
    }
    uint32_t sp_base = (uint32_t)(uintptr_t)(stk + 65536 - 64);

    for (int iter = 0; iter < FC_ITERS; iter++) {
        int k = (arg->tid + iter) % FC_NFNS;
        if (arg->tid == FC_NTHREADS - 1 && (iter % 50) == 0) {
            dbt_invalidate_cache_range(arg->fn_vas[k], 16);
        }
        uint32_t *sp = (uint32_t *)(uintptr_t)sp_base;
        sp[0] = DBT_MAGIC_RET_TO_HOST;
        sp[1] = arg->fn_vas[k];

        newminhh_cpu_t cpu;
        memset(&cpu, 0, sizeof(cpu));
        cpu.eflags = 0x202u;
        cpu.gpr[NM_REG_ESP] = sp_base;

        int rc = newminhh_dbt_enter_at(arg->dispatch_va, &cpu);
        uint32_t expected = 0xC0DE0000u + (uint32_t)k;
        if (rc != NM_OK || cpu.gpr[NM_REG_EAX] != expected) {
            fprintf(stderr, "fast_cache race tid=%d iter=%d k=%d: rc=%d eax=0x%08x expected=0x%08x\n",
                    arg->tid, iter, k, rc, cpu.gpr[NM_REG_EAX], expected);
            arg->failed = 1;
            break;
        }
    }

    munmap(stk, 65536);
    return NULL;
}

static int test_fast_cache_collision_mt(void) {
    size_t region_sz = 2u * 1024u * 1024u;
    uint8_t *raw = (uint8_t *)mmap(NULL, region_sz, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) {
        perror("mmap 2MiB");
        return 1;
    }
    uintptr_t aligned = ((uintptr_t)raw + 0xFFFFFu) & ~(uintptr_t)0xFFFFFu;
    uint8_t *base = (uint8_t *)aligned;

    uint32_t fn_vas[FC_NFNS];
    for (int k = 0; k < FC_NFNS; k++) {
        /* All fn_vas[k] have identical (va ^ (va >> 12)) & 0xFFFF */
        uint32_t off = ((uint32_t)k << 16) + ((uint32_t)k << 4);
        uint8_t *p = base + off;
        uint32_t val = 0xC0DE0000u + (uint32_t)k;
        p[0] = 0xB8; /* mov eax, imm32 */
        memcpy(p + 1, &val, 4);
        p[5] = 0xC3; /* ret */
        fn_vas[k] = (uint32_t)(uintptr_t)p;
    }

    /* Indirect dispatcher at base + 0x80000: mov eax, [esp+4]; call eax; ret */
    uint8_t *disp = base + 0x80000u;
    const uint8_t disp_code[] = {0x8B, 0x44, 0x24, 0x04, 0xFF, 0xD0, 0xC3};
    memcpy(disp, disp_code, sizeof(disp_code));
    uint32_t disp_va = (uint32_t)(uintptr_t)disp;

    if (mprotect(raw, region_sz, PROT_READ) != 0) {
        perror("mprotect PROT_READ");
        munmap(raw, region_sz);
        return 1;
    }

    pthread_t th[FC_NTHREADS];
    fc_worker_arg_t args[FC_NTHREADS];
    for (int i = 0; i < FC_NTHREADS; i++) {
        args[i].tid = i;
        args[i].dispatch_va = disp_va;
        memcpy(args[i].fn_vas, fn_vas, sizeof(fn_vas));
        args[i].failed = 0;
        if (pthread_create(&th[i], NULL, fc_worker, &args[i]) != 0) {
            perror("pthread_create");
            munmap(raw, region_sz);
            return 1;
        }
    }

    int failed = 0;
    for (int i = 0; i < FC_NTHREADS; i++) {
        pthread_join(th[i], NULL);
        if (args[i].failed) failed = 1;
    }

    munmap(raw, region_sz);
    if (failed) return 1;
    printf("PASS test_fast_cache_collision_mt (%d threads x %d iters)\n", FC_NTHREADS, FC_ITERS);
    return 0;
}

/* --- 3. Direct edge unchaining test --- */
static int test_direct_edge_unchaining(void) {
    uint8_t *page = (uint8_t *)mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    /* Block A at +0: call +0x40 (rel32 = 0x3B); ret */
    page[0] = 0xE8;
    int32_t rel = 0x40 - 5;
    memcpy(page + 1, &rel, 4);
    page[5] = 0xC3;

    /* Block B at +0x40: mov eax, 111; ret */
    uint32_t v1 = 111u;
    page[0x40] = 0xB8;
    memcpy(page + 0x41, &v1, 4);
    page[0x45] = 0xC3;

    if (mprotect(page, 4096, PROT_READ) != 0) {
        perror("mprotect");
        munmap(page, 4096);
        return 1;
    }

    uint32_t va_a = (uint32_t)(uintptr_t)page;
    uint32_t va_b = (uint32_t)(uintptr_t)(page + 0x40);

    int r1 = newminhh_dbt_enter_at(va_a, NULL);
    int r2 = newminhh_dbt_enter_at(va_a, NULL);
    if (r1 != 111 || r2 != 111) {
        fprintf(stderr, "unchain pre-check failed: r1=%d r2=%d (expected 111)\n", r1, r2);
        munmap(page, 4096);
        return 1;
    }

    /* Mutate Block B and invalidate its range -> Block A's patched direct edge must unchain */
    if (mprotect(page, 4096, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect RW");
        munmap(page, 4096);
        return 1;
    }
    uint32_t v2 = 222u;
    memcpy(page + 0x41, &v2, 4);
    if (mprotect(page, 4096, PROT_READ) != 0) {
        perror("mprotect R");
        munmap(page, 4096);
        return 1;
    }
    dbt_invalidate_cache_range(va_b, 16);

    int r3 = newminhh_dbt_enter_at(va_a, NULL);
    int r4 = newminhh_dbt_enter_at(va_a, NULL);
    munmap(page, 4096);
    if (r3 != 222 || r4 != 222) {
        fprintf(stderr, "direct edge unchaining failed: r3=%d r4=%d (expected 222)\n", r3, r4);
        return 1;
    }
    printf("PASS test_direct_edge_unchaining\n");
    return 0;
}

/* --- 4. Concurrent SIGSEGV NX-guard callbacks + brk + dlopen/dlsym/dlclose stress --- */
#define MT_NTHREADS 8
#define MT_ITERS    200

typedef struct {
    int tid;
    uint32_t raw_fn_va;
    int expected_ret;
    const char *plugin_so;
    int failed;
} mt_stress_arg_t;

static void *mt_stress_worker(void *raw) {
    mt_stress_arg_t *arg = (mt_stress_arg_t *)raw;
    typedef int (*fn3_t)(int, int, int);
    typedef int (*fn2_t)(int, int);
    fn3_t raw_fn = (fn3_t)(uintptr_t)arg->raw_fn_va;

    for (int i = 0; i < MT_ITERS; i++) {
        /* A. Raw PROT_READ guest pointer invocation -> SIGSEGV NX-guard trampoline */
        int r = raw_fn(7, 5, 2);
        if (r != arg->expected_ret) {
            fprintf(stderr, "mt SIGSEGV cb tid=%d iter=%d: got %d expected %d\n",
                    arg->tid, i, r, arg->expected_ret);
            arg->failed = 1;
            break;
        }

        /* B. Concurrent brk(0) / brk(cur + 4096) via i386 syscall 45 */
        int32_t cur = newminhh_compat_syscall(45u, 0, 0, 0, 0, 0, 0);
        if (cur <= 0) {
            fprintf(stderr, "mt brk(0) tid=%d iter=%d failed: %d\n", arg->tid, i, cur);
            arg->failed = 1;
            break;
        }
        if ((i % 10) == 0) {
            int32_t nxt = newminhh_compat_syscall(45u, (uint32_t)cur + 4096u, 0, 0, 0, 0, 0);
            if (nxt < cur) {
                fprintf(stderr, "mt brk went backwards tid=%d: cur=%d nxt=%d\n",
                        arg->tid, cur, nxt);
                arg->failed = 1;
                break;
            }
        }

        /* C. Concurrent dlopen / dlsym / invoke / dlclose */
        if ((i % 5) == 0) {
            void *h = newminhh_dbt_dlopen(arg->plugin_so, 0);
            if (!h) {
                fprintf(stderr, "mt dlopen failed tid=%d iter=%d: %s\n",
                        arg->tid, i, newminhh_dbt_dlerror());
                arg->failed = 1;
                break;
            }
            fn2_t pfn = (fn2_t)newminhh_dbt_dlsym(h, "plugin_compute");
            if (!pfn || pfn(7, 5) != 47) {
                fprintf(stderr, "mt dlsym/invoke failed tid=%d iter=%d\n", arg->tid, i);
                newminhh_dbt_dlclose(h);
                arg->failed = 1;
                break;
            }
            if (newminhh_dbt_dlclose(h) != 0) {
                fprintf(stderr, "mt dlclose failed tid=%d iter=%d\n", arg->tid, i);
                arg->failed = 1;
                break;
            }
        }
    }
    return NULL;
}

static int test_concurrent_brk_dl_sigsegv(void) {
    /* Initialize brk arena by loading an ET_DYN main ELF into memory */
    char pie_path[512], basic_so[512], plugin_so[512];
    snprintf(pie_path, sizeof(pie_path), "%s/cli_switch_jumptable_pie", g_i386_dir);
    snprintf(basic_so, sizeof(basic_so), "%s/libbasic_pic.so", g_i386_dir);
    snprintf(plugin_so, sizeof(plugin_so), "%s/libplugin_stripped.so", g_i386_dir);

    int fd = open(pie_path, O_RDONLY);
    if (fd < 0) {
        perror("open pie");
        return 1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return 1;
    }
    uint8_t *blob = (uint8_t *)malloc((size_t)st.st_size);
    if (!blob || read(fd, blob, (size_t)st.st_size) != st.st_size) {
        free(blob);
        close(fd);
        return 1;
    }
    close(fd);

    dbt_module_t *main_mod = NULL;
    if (dbt_load_main_memory(blob, (size_t)st.st_size, pie_path, &main_mod) != NM_OK) {
        free(blob);
        fprintf(stderr, "dbt_load_main_memory failed\n");
        return 1;
    }
    free(blob);

    uint32_t gh = dbt_guest_dlopen(basic_so, 0);
    if (!gh) {
        fprintf(stderr, "dbt_guest_dlopen(libbasic_pic.so) failed\n");
        return 1;
    }
    uint32_t raw_fn_va = dbt_guest_dlsym(gh, "basic_add_mul");
    if (!raw_fn_va) {
        fprintf(stderr, "dbt_guest_dlsym(basic_add_mul) failed\n");
        return 1;
    }
    typedef int (*fn3_t)(int, int, int);
    int expected_ret = ((fn3_t)(uintptr_t)raw_fn_va)(7, 5, 2);

    pthread_t th[MT_NTHREADS];
    mt_stress_arg_t args[MT_NTHREADS];
    for (int i = 0; i < MT_NTHREADS; i++) {
        args[i].tid = i;
        args[i].raw_fn_va = raw_fn_va;
        args[i].expected_ret = expected_ret;
        args[i].plugin_so = plugin_so;
        args[i].failed = 0;
        if (pthread_create(&th[i], NULL, mt_stress_worker, &args[i]) != 0) {
            perror("pthread_create");
            return 1;
        }
    }

    int failed = 0;
    for (int i = 0; i < MT_NTHREADS; i++) {
        pthread_join(th[i], NULL);
        if (args[i].failed) failed = 1;
    }

    dbt_guest_dlclose(gh);
    if (failed) return 1;
    printf("PASS test_concurrent_brk_dl_sigsegv (%d threads x %d iters)\n", MT_NTHREADS, MT_ITERS);
    return 0;
}

/* --- 5. ET_EXEC EEXIST collision diagnostic test --- */
static int test_exec_embedded_eexist_diag(void) {
    char nopie_path[512];
    snprintf(nopie_path, sizeof(nopie_path), "%s/cli_switch_jumptable_nopie", g_i386_dir);
    int fd = open(nopie_path, O_RDONLY);
    if (fd < 0) {
        perror("open nopie");
        return 1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return 1;
    }
    size_t sz = (size_t)st.st_size;
    uint8_t *blob = (uint8_t *)malloc(sz);
    if (!blob || read(fd, blob, sz) != (ssize_t)sz) {
        free(blob);
        close(fd);
        return 1;
    }
    close(fd);

    pid_t pid = fork();
    if (pid < 0) {
        free(blob);
        return 1;
    }
    if (pid == 0) {
        /* Occupy 0x08048000 to simulate an in-place ET_EXEC host collision */
        void *blk = mmap((void *)0x08048000u, 4096, PROT_READ,
                         MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (blk == MAP_FAILED) _exit(2);
        char *argv_em[] = {(char *)"embedded_nopie", NULL};
        int rc = newminhh_dbt_exec_embedded(blob, sz, 0, 1, argv_em, NULL);
        munmap(blk, 4096);
        _exit(rc < 0 ? 0 : 3);
    }
    free(blob);

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "test_exec_embedded_eexist_diag failed: status=0x%x\n", status);
        return 1;
    }
    printf("PASS test_exec_embedded_eexist_diag\n");
    return 0;
}

/* --- 6. 0x64 (FS) prefix rejection on 0xA0..0xA3 moffs32 --- */
static int test_fs_moffs_rejected(void) {
    uint8_t *page = (uint8_t *)mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    /* mov eax, fs:[0x12345678]; ret */
    const uint8_t bad_fs[] = {0x64, 0xA1, 0x78, 0x56, 0x34, 0x12, 0xC3};
    memcpy(page, bad_fs, sizeof(bad_fs));
    if (mprotect(page, 4096, PROT_READ) != 0) {
        perror("mprotect");
        munmap(page, 4096);
        return 1;
    }
    newminhh_cpu_t cpu;
    memset(&cpu, 0, sizeof(cpu));
    int rc = newminhh_dbt_enter_at((uint32_t)(uintptr_t)page, &cpu);
    munmap(page, 4096);
    if (rc != NM_ERR_INSN) {
        fprintf(stderr, "test_fs_moffs_rejected: expected NM_ERR_INSN (%d), got %d\n",
                NM_ERR_INSN, rc);
        return 1;
    }
    printf("PASS test_fs_moffs_rejected\n");
    return 0;
}

/* --- 7. Memory & resource leak audit verification (1000 dlopen/dlsym cycles + 64 thread TLS/stack cycles) --- */
static long read_proc_self_vm_pages(void) {
    FILE *fp = fopen("/proc/self/statm", "r");
    if (!fp) return -1;
    long total_pages = -1;
    if (fscanf(fp, "%ld", &total_pages) != 1) {
        total_pages = -1;
    }
    fclose(fp);
    return total_pages;
}

static void *leak_thread_worker(void *arg) {
    uint32_t code_va = (uint32_t)(uintptr_t)arg;
    /* Passing cpu=NULL forces dbt_ensure_thread_tls_and_stack to allocate 16 KiB TLS + 2 MiB stack */
    int res = newminhh_dbt_enter_at(code_va, NULL);
    return (res == 77) ? (void *)1 : NULL;
}

static int test_leak_audit_cycles(void) {
    char plugin_so[512];
    snprintf(plugin_so, sizeof(plugin_so), "%s/libplugin_stripped.so", g_i386_dir);

    /* A. 1000 cycles of newminhh_dbt_dlopen + dlsym + invoke + dlclose (> 512 CB cap & > 256 cache cap) */
    typedef int (*fn2_t)(int, int);
    for (int i = 0; i < 1000; i++) {
        void *h = newminhh_dbt_dlopen(plugin_so, 0);
        if (!h) {
            fprintf(stderr, "leak_audit: newminhh_dbt_dlopen failed at iter %d: %s\n",
                    i, newminhh_dbt_dlerror());
            return 1;
        }
        fn2_t pfn = (fn2_t)newminhh_dbt_dlsym(h, "plugin_compute");
        if (!pfn || pfn(7, 5) != 47) {
            fprintf(stderr, "leak_audit: newminhh_dbt_dlsym/invoke failed at iter %d\n", i);
            newminhh_dbt_dlclose(h);
            return 1;
        }
        if (newminhh_dbt_dlclose(h) != 0) {
            fprintf(stderr, "leak_audit: newminhh_dbt_dlclose failed at iter %d\n", i);
            return 1;
        }
    }

    /* B. 1000 cycles of compat_dlopen("libc.so.6") + compat_dlsym_cdecl("strlen") + compat_dlclose (> 256 OUT cap) */
    void *first_tramp = NULL;
    for (int i = 0; i < 1000; i++) {
        void *ch = compat_dlopen("libc.so.6", 0);
        if (!ch) {
            fprintf(stderr, "leak_audit: compat_dlopen(libc.so.6) failed at iter %d\n", i);
            return 1;
        }
        void *tr = compat_dlsym_cdecl(ch, "strlen");
        if (!tr) {
            fprintf(stderr, "leak_audit: compat_dlsym_cdecl(strlen) failed at iter %d\n", i);
            compat_dlclose(ch);
            return 1;
        }
        if (i == 0) {
            first_tramp = tr;
        } else if (tr != first_tramp) {
            fprintf(stderr, "leak_audit: outgoing trampoline not deduplicated at iter %d (%p != %p)\n",
                    i, tr, first_tramp);
            compat_dlclose(ch);
            return 1;
        }
        if (compat_dlclose(ch) != 0) {
            fprintf(stderr, "leak_audit: compat_dlclose failed at iter %d\n", i);
            return 1;
        }
    }

    /* C. 64 worker thread create/join cycles verifying 16 KiB TLS + 2 MiB guest stack munmap on exit */
    uint8_t *page = (uint8_t *)mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    /* mov eax, gs:[0]; test eax, eax; mov eax, 77; ret */
    const uint8_t code[] = {0x65, 0xA1, 0x00, 0x00, 0x00, 0x00, 0xB8, 0x4D, 0x00, 0x00, 0x00, 0xC3};
    memcpy(page, code, sizeof(code));
    if (mprotect(page, 4096, PROT_READ) != 0) {
        munmap(page, 4096);
        return 1;
    }

    /* Warm up 1 thread first so glibc pthread stack cache settles */
    pthread_t warm;
    if (pthread_create(&warm, NULL, leak_thread_worker, page) != 0) {
        munmap(page, 4096);
        return 1;
    }
    void *wret = NULL;
    pthread_join(warm, &wret);
    if (wret != (void *)1) {
        munmap(page, 4096);
        return 1;
    }

    long pages_before = read_proc_self_vm_pages();
    for (int i = 0; i < 64; i++) {
        pthread_t th;
        if (pthread_create(&th, NULL, leak_thread_worker, page) != 0) {
            munmap(page, 4096);
            return 1;
        }
        void *tret = NULL;
        pthread_join(th, &tret);
        if (tret != (void *)1) {
            fprintf(stderr, "leak_audit: worker thread failed at iter %d\n", i);
            munmap(page, 4096);
            return 1;
        }
    }
    long pages_after = read_proc_self_vm_pages();
    munmap(page, 4096);

    /* Without TLS cleanup, 64 threads would leak 64 * 4 = 256 pages (1 MiB);
     * without stack cleanup, 64 threads would leak 64 * 512 = 32768 pages (128 MiB).
     * Allow at most 64 pages of slack for allocator metadata. */
    if (pages_before > 0 && pages_after > 0 && (pages_after - pages_before) > 64) {
        fprintf(stderr, "leak_audit: VmSize grew by %ld pages (%ld -> %ld) across 64 threads\n",
                pages_after - pages_before, pages_before, pages_after);
        return 1;
    }

    printf("PASS test_leak_audit_cycles (1000 dbt_dl cycles, 1000 compat_dl cycles, 64 threads delta=%ld pages)\n",
           (pages_before > 0 && pages_after > 0) ? (pages_after - pages_before) : 0L);
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1) {
        g_i386_dir = argv[1];
    }
    if (test_sigsegv_sigign_in_child() != 0) return 1;
    if (test_fast_cache_collision_mt() != 0) return 1;
    if (test_direct_edge_unchaining() != 0) return 1;
    if (test_concurrent_brk_dl_sigsegv() != 0) return 1;
    if (test_exec_embedded_eexist_diag() != 0) return 1;
    if (test_fs_moffs_rejected() != 0) return 1;
    if (test_leak_audit_cycles() != 0) return 1;
    printf("ALL_PART4_MT_STRESS_TESTS_PASSED\n");
    return 0;
}
