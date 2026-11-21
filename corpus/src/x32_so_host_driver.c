#define _GNU_SOURCE
#include "newminhh_cstd.h"
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef int (*basic_add_mul_t)(int, int, int);
typedef int (*basic_format_msg_t)(char *, int, int);
typedef int (*basic_get_counter_t)(void);

typedef int (*nm_cb_t)(int, int);
typedef int (*lifecycle_check_init_t)(void);
typedef int (*lifecycle_get_pid_int80_t)(void);
typedef int (*lifecycle_stat_probe_t)(const char *, uint64_t *, uint64_t *);
typedef int (*lifecycle_timespec_rlimit_probe_t)(int64_t *, int64_t *, uint64_t *, uint64_t *);
typedef void (*lifecycle_register_cb_t)(nm_cb_t);
typedef int (*lifecycle_trigger_cb_t)(int, int);
typedef nm_cb_t (*lifecycle_get_guest_cb_t)(void);

typedef int (*plugin_compute_t)(int, int);
typedef const char *(*plugin_name_t)(void);

typedef void *(*dlopen_fn_t)(const char *, int);
typedef void *(*dlsym_fn_t)(void *, const char *);
typedef int (*dlclose_fn_t)(void *);
typedef const char *(*dlerror_fn_t)(void);

static int host_callback(int x, int y) {
    return (x * 3) + (y * 2);
}

int main(int argc, char **argv) {
    const char *so_dir = ".";
    int use_dbt = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dbt") == 0) {
            use_dbt = 1;
        } else {
            so_dir = argv[i];
        }
    }
    if (getenv("NEWMINHH_USE_DBT") && strcmp(getenv("NEWMINHH_USE_DBT"), "1") == 0) {
        use_dbt = 1;
    }

    dlopen_fn_t p_dlopen = dlopen;
    dlsym_fn_t p_dlsym = dlsym;
    dlclose_fn_t p_dlclose = dlclose;
    dlerror_fn_t p_dlerror = NM_VOIDP_TO_FN(dlerror_fn_t, dlerror);
    void *dbt_lib = NULL;

    if (use_dbt) {
        dbt_lib = dlopen("libnewminhh_dbt.so", RTLD_NOW);
        if (!dbt_lib) {
            fprintf(stderr, "Failed to load libnewminhh_dbt.so: %s\n", dlerror());
            return 1;
        }
        p_dlopen = NM_VOIDP_TO_FN(dlopen_fn_t, dlsym(dbt_lib, "newminhh_dbt_dlopen"));
        p_dlsym = NM_VOIDP_TO_FN(dlsym_fn_t, dlsym(dbt_lib, "newminhh_dbt_dlsym"));
        p_dlclose = NM_VOIDP_TO_FN(dlclose_fn_t, dlsym(dbt_lib, "newminhh_dbt_dlclose"));
        if (!p_dlopen || !p_dlsym || !p_dlclose) {
            fprintf(stderr, "Missing DBT dl symbols\n");
            return 1;
        }
    }

    char path_s01[256], path_s02[256], path_s03[256];
    snprintf(path_s01, sizeof(path_s01), "%s/libbasic_pic.so", so_dir);
    snprintf(path_s02, sizeof(path_s02), "%s/liblifecycle_abi.so", so_dir);
    snprintf(path_s03, sizeof(path_s03), "%s/libplugin_stripped.so", so_dir);

    /* --- S01: libbasic_pic.so --- */
    void *h1 = p_dlopen(path_s01, RTLD_NOW);
    if (!h1) {
        fprintf(stderr, "dlopen %s failed: %s\n", path_s01, p_dlerror());
        return 1;
    }
    basic_add_mul_t f_add_mul = NM_VOIDP_TO_FN(basic_add_mul_t, p_dlsym(h1, "basic_add_mul"));
    basic_format_msg_t f_fmt = NM_VOIDP_TO_FN(basic_format_msg_t, p_dlsym(h1, "basic_format_msg"));
    basic_get_counter_t f_cnt = NM_VOIDP_TO_FN(basic_get_counter_t, p_dlsym(h1, "basic_get_counter"));
    if (!f_add_mul || !f_fmt || !f_cnt) {
        fprintf(stderr, "S01 dlsym failed\n");
        return 1;
    }
    int basic_val = f_add_mul(7, 3, 2);
    char msg[64];
    int mlen = f_fmt(msg, sizeof(msg), basic_val);
    int cnt = f_cnt();
    if (mlen <= 0 || cnt != 11) {
        fprintf(stderr, "S01 mismatch: mlen=%d cnt=%d msg=%s\n", mlen, cnt, msg);
        return 1;
    }
    p_dlclose(h1);

    /* --- S02: liblifecycle_abi.so --- */
    char marker_path[128];
    snprintf(marker_path, sizeof(marker_path), "/tmp/newminhh_fini_%ld.txt", (long)getpid());
    unlink(marker_path);
    setenv("NEWMINHH_FINI_MARKER", marker_path, 1);

    void *h2 = p_dlopen(path_s02, RTLD_NOW);
    if (!h2) {
        fprintf(stderr, "dlopen %s failed: %s\n", path_s02, p_dlerror());
        return 1;
    }
    lifecycle_check_init_t f_init = NM_VOIDP_TO_FN(lifecycle_check_init_t, p_dlsym(h2, "lifecycle_check_init"));
    lifecycle_get_pid_int80_t f_pid = NM_VOIDP_TO_FN(lifecycle_get_pid_int80_t, p_dlsym(h2, "lifecycle_get_pid_int80"));
    lifecycle_stat_probe_t f_stat = NM_VOIDP_TO_FN(lifecycle_stat_probe_t, p_dlsym(h2, "lifecycle_stat_probe"));
    lifecycle_timespec_rlimit_probe_t f_tsrl =
        NM_VOIDP_TO_FN(lifecycle_timespec_rlimit_probe_t, p_dlsym(h2, "lifecycle_timespec_rlimit_probe"));
    lifecycle_register_cb_t f_reg = NM_VOIDP_TO_FN(lifecycle_register_cb_t, p_dlsym(h2, "lifecycle_register_cb"));
    lifecycle_trigger_cb_t f_trig = NM_VOIDP_TO_FN(lifecycle_trigger_cb_t, p_dlsym(h2, "lifecycle_trigger_cb"));
    lifecycle_get_guest_cb_t f_gcb = NM_VOIDP_TO_FN(lifecycle_get_guest_cb_t, p_dlsym(h2, "lifecycle_get_guest_cb"));
    if (!f_init || !f_pid || !f_stat || !f_tsrl || !f_reg || !f_trig || !f_gcb) {
        fprintf(stderr, "S02 dlsym failed\n");
        return 1;
    }

    if (f_init() != 1) {
        fprintf(stderr, "S02 constructor check failed\n");
        return 1;
    }
    if (f_pid() != (int)getpid()) {
        fprintf(stderr, "S02 pid check failed\n");
        return 1;
    }

    /* Create temp file for stat probe */
    char dat_path[128];
    snprintf(dat_path, sizeof(dat_path), "/tmp/newminhh_s02_%ld.dat", (long)getpid());
    int fd = open(dat_path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0) return 1;
    if (write(fd, "0123456789abcdef", 16) != 16) {
        close(fd);
        unlink(dat_path);
        return 1;
    }
    close(fd);

    uint64_t fsz = 0, fino = 0;
    int sret = f_stat(dat_path, &fsz, &fino);
    unlink(dat_path);
    if (sret != 0 || fsz != 16 || fino == 0) {
        fprintf(stderr, "S02 stat probe failed: ret=%d sz=%llu ino=%llu\n",
                sret, (unsigned long long)fsz, (unsigned long long)fino);
        return 1;
    }

    int64_t sec = 0, nsec = -1;
    uint64_t rcur = 0, rmax = 0;
    if (f_tsrl(&sec, &nsec, &rcur, &rmax) != 0 || nsec < 0 || rcur == 0 || rcur > rmax) {
        fprintf(stderr, "S02 timespec/rlimit probe failed\n");
        return 1;
    }

    f_reg(host_callback);
    int cb_res = f_trig(4, 5);
    /* host_callback(4,5) = 12+10 = 22; + arr[0]=5 => 27 */
    if (cb_res != 27) {
        fprintf(stderr, "S02 host callback failed: got %d expected 27\n", cb_res);
        return 1;
    }

    nm_cb_t guest_cb = f_gcb();
    if (!guest_cb || guest_cb(3, 4) != 41) {
        fprintf(stderr, "S02 guest callback failed\n");
        return 1;
    }

    p_dlclose(h2);

    /* Verify .fini_array marker was written */
    int mfd = open(marker_path, O_RDONLY);
    int fini_ok = 0;
    if (mfd >= 0) {
        char mbuf[32];
        memset(mbuf, 0, sizeof(mbuf));
        ssize_t n = read(mfd, mbuf, sizeof(mbuf) - 1);
        close(mfd);
        if (n > 0 && strncmp(mbuf, "FINI_OK", 7) == 0) {
            fini_ok = 1;
        }
    }
    unlink(marker_path);
    if (!fini_ok) {
        fprintf(stderr, "S02 .fini_array marker missing after dlclose\n");
        return 1;
    }

    /* --- S03: libplugin_stripped.so + N09 missing symbol check --- */
    void *h3 = p_dlopen(path_s03, RTLD_NOW);
    if (!h3) {
        fprintf(stderr, "dlopen %s failed: %s\n", path_s03, p_dlerror());
        return 1;
    }
    plugin_compute_t f_pcomp = NM_VOIDP_TO_FN(plugin_compute_t, p_dlsym(h3, "plugin_compute"));
    plugin_name_t f_pname = NM_VOIDP_TO_FN(plugin_name_t, p_dlsym(h3, "plugin_name"));
    if (!f_pcomp || !f_pname) {
        fprintf(stderr, "S03 dlsym failed\n");
        return 1;
    }
    int plug_val = f_pcomp(7, 5);
    const char *pname = f_pname();
    if (plug_val != 47 || !pname || strcmp(pname, "newminhh_plugin_s03") != 0) {
        fprintf(stderr, "S03 mismatch\n");
        return 1;
    }

    /* N09: missing symbol must return NULL and populate dlerror */
    p_dlerror();
    void *missing = p_dlsym(h3, "newminhh_nonexistent_sym");
    const char *merr = p_dlerror();
    if (missing != NULL || (!use_dbt && merr == NULL)) {
        fprintf(stderr, "N09 missing symbol unexpectedly succeeded\n");
        p_dlclose(h3);
        return 1;
    }
    p_dlclose(h3);

    if (dbt_lib) {
        dlclose(dbt_lib);
    }

    printf("SO_DRIVER_OK basic=%d lifecycle=%d plugin=%d fini=1 n09_null=1\n",
           basic_val, cb_res, plug_val);
    return 0;
}
