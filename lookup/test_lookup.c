#include "newminhh_lookup.h"
#include "newminhh_cstd.h"
#include "newminhh_abi.h"
#include "newminhh_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "[FAIL] %s:%d: assertion failed: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

static void test_sig_parse_and_format(void)
{
    printf("--- [test_sig_parse_and_format] ---\n");

    nm_fn_sig_t sig;
    uint32_t flags = 0;
    char buf[64];

    /* 1. v: */
    TEST_ASSERT(nm_sig_parse("v:", &sig, &flags) == NM_OK);
    TEST_ASSERT(sig.ret_kind == NM_RET_VOID);
    TEST_ASSERT(sig.nargs == 0);
    TEST_ASSERT(sig.is_variadic == 0);
    TEST_ASSERT(sig.abi_ext == 0);
    TEST_ASSERT(flags == 0);
    TEST_ASSERT(nm_sig_format(&sig, flags, buf, sizeof(buf)) == NM_OK);
    TEST_ASSERT(strcmp(buf, "v:") == 0);

    /* 2. i:p */
    TEST_ASSERT(nm_sig_parse("i:p", &sig, &flags) == NM_OK);
    TEST_ASSERT(sig.ret_kind == NM_RET_I32);
    TEST_ASSERT(sig.nargs == 1);
    TEST_ASSERT(sig.arg_kinds[0] == NM_ARG_PTR);
    TEST_ASSERT(nm_sig_format(&sig, flags, buf, sizeof(buf)) == NM_OK);
    TEST_ASSERT(strcmp(buf, "i:p") == 0);

    /* 3. u:uuq */
    TEST_ASSERT(nm_sig_parse("u:uuq", &sig, &flags) == NM_OK);
    TEST_ASSERT(sig.ret_kind == NM_RET_U32);
    TEST_ASSERT(sig.nargs == 3);
    TEST_ASSERT(sig.arg_kinds[0] == NM_ARG_U32);
    TEST_ASSERT(sig.arg_kinds[1] == NM_ARG_U32);
    TEST_ASSERT(sig.arg_kinds[2] == NM_ARG_I64);
    TEST_ASSERT(nm_sig_format(&sig, flags, buf, sizeof(buf)) == NM_OK);
    TEST_ASSERT(strcmp(buf, "u:uuq") == 0);

    /* 4. q:Q */
    TEST_ASSERT(nm_sig_parse("q:Q", &sig, &flags) == NM_OK);
    TEST_ASSERT(sig.ret_kind == NM_RET_I64);
    TEST_ASSERT(sig.nargs == 1);
    TEST_ASSERT(sig.arg_kinds[0] == NM_ARG_U64);
    TEST_ASSERT(nm_sig_format(&sig, flags, buf, sizeof(buf)) == NM_OK);
    TEST_ASSERT(strcmp(buf, "q:Q") == 0);

    /* 5. f:f */
    TEST_ASSERT(nm_sig_parse("f:f", &sig, &flags) == NM_OK);
    TEST_ASSERT(sig.ret_kind == NM_RET_F32);
    TEST_ASSERT(sig.nargs == 1);
    TEST_ASSERT(sig.arg_kinds[0] == NM_ARG_F32);
    TEST_ASSERT(nm_sig_format(&sig, flags, buf, sizeof(buf)) == NM_OK);
    TEST_ASSERT(strcmp(buf, "f:f") == 0);

    /* 6. d:dd */
    TEST_ASSERT(nm_sig_parse("d:dd", &sig, &flags) == NM_OK);
    TEST_ASSERT(sig.ret_kind == NM_RET_F64);
    TEST_ASSERT(sig.nargs == 2);
    TEST_ASSERT(sig.arg_kinds[0] == NM_ARG_F64);
    TEST_ASSERT(sig.arg_kinds[1] == NM_ARG_F64);
    TEST_ASSERT(nm_sig_format(&sig, flags, buf, sizeof(buf)) == NM_OK);
    TEST_ASSERT(strcmp(buf, "d:dd") == 0);

    /* 7. S8:ii (sret) */
    TEST_ASSERT(nm_sig_parse("S8:ii", &sig, &flags) == NM_OK);
    TEST_ASSERT(sig.ret_kind == NM_RET_PTR);
    TEST_ASSERT(sig.nargs == 2);
    TEST_ASSERT(sig.arg_kinds[0] == NM_ARG_I32);
    TEST_ASSERT(sig.arg_kinds[1] == NM_ARG_I32);
    TEST_ASSERT(NM_SIG_EXT_SRET_SIZE(sig.abi_ext) == 8);
    TEST_ASSERT(sig.abi_ext & NM_SIG_EXT_SRET);
    TEST_ASSERT(sig.abi_ext & NM_SIG_EXT_CALLEE_POP4);
    TEST_ASSERT(nm_sig_format(&sig, flags, buf, sizeof(buf)) == NM_OK);
    TEST_ASSERT(strcmp(buf, "S8:ii") == 0);

    /* 8. S16:p */
    TEST_ASSERT(nm_sig_parse("S16:p", &sig, &flags) == NM_OK);
    TEST_ASSERT(NM_SIG_EXT_SRET_SIZE(sig.abi_ext) == 16);
    TEST_ASSERT(nm_sig_format(&sig, flags, buf, sizeof(buf)) == NM_OK);
    TEST_ASSERT(strcmp(buf, "S16:p") == 0);

    /* 9. D4 (data) */
    TEST_ASSERT(nm_sig_parse("D4", &sig, &flags) == NM_OK);
    TEST_ASSERT(sig.ret_kind == NM_RET_PTR);
    TEST_ASSERT(sig.nargs == 0);
    TEST_ASSERT(flags & NM_SYM_FLAG_DATA);
    TEST_ASSERT(sig.abi_ext == 4);
    TEST_ASSERT(nm_sig_format(&sig, flags, buf, sizeof(buf)) == NM_OK);
    TEST_ASSERT(strcmp(buf, "D4") == 0);

    /* 10. Variadic tail i:p... */
    TEST_ASSERT(nm_sig_parse("i:p...", &sig, &flags) == NM_OK);
    TEST_ASSERT(sig.ret_kind == NM_RET_I32);
    TEST_ASSERT(sig.nargs == 1);
    TEST_ASSERT(sig.arg_kinds[0] == NM_ARG_PTR);
    TEST_ASSERT(sig.is_variadic == 1);
    TEST_ASSERT(flags & NM_SYM_FLAG_VARIADIC);
    TEST_ASSERT(nm_sig_format(&sig, flags, buf, sizeof(buf)) == NM_OK);
    TEST_ASSERT(strcmp(buf, "i:p...") == 0);

    /* 11. 16 args max */
    TEST_ASSERT(nm_sig_parse("p:iiiiiiiiiiiiiiii", &sig, &flags) == NM_OK);
    TEST_ASSERT(sig.nargs == 16);
    TEST_ASSERT(nm_sig_format(&sig, flags, buf, sizeof(buf)) == NM_OK);
    TEST_ASSERT(strcmp(buf, "p:iiiiiiiiiiiiiiii") == 0);

    /* 12. Negative tests: strictly reject invalid/forbidden signatures */
    TEST_ASSERT(nm_sig_parse("", &sig, &flags) == NM_ERR_ABI);
    TEST_ASSERT(nm_sig_parse("-", &sig, &flags) == NM_ERR_ABI);
    TEST_ASSERT(nm_sig_parse("e:ii", &sig, &flags) == NM_ERR_ABI); /* long double ret */
    TEST_ASSERT(nm_sig_parse("i:e", &sig, &flags) == NM_ERR_ABI);  /* long double arg */
    TEST_ASSERT(nm_sig_parse("A8:ii", &sig, &flags) == NM_ERR_ABI); /* aggregate ret */
    TEST_ASSERT(nm_sig_parse("i:A8", &sig, &flags) == NM_ERR_ABI);  /* aggregate arg */
    TEST_ASSERT(nm_sig_parse("p:iiiiiiiiiiiiiiiii", &sig, &flags) == NM_ERR_ABI); /* 17 args > 16 */
    TEST_ASSERT(nm_sig_parse("S0:i", &sig, &flags) == NM_ERR_ABI);  /* S0 out of bounds */
    TEST_ASSERT(nm_sig_parse("S17:i", &sig, &flags) == NM_ERR_ABI); /* S17 out of bounds */
    TEST_ASSERT(nm_sig_parse("D", &sig, &flags) == NM_ERR_ABI);
    TEST_ASSERT(nm_sig_parse("xyz", &sig, &flags) == NM_ERR_ABI);
    TEST_ASSERT(nm_sig_parse(":::", &sig, &flags) == NM_ERR_ABI);

    printf("PASS test_sig_parse_and_format\n");
}

static void test_lookup_open_and_find(void)
{
    printf("--- [test_lookup_open_and_find] ---\n");

    const char sample_table[] =
        "#!newminhh-lookup v1\n"
        "#@generator\tnewminhh-lookup-gen\t1.0.0\n"
        "#@libc\tglibc\n"
        "#@interp\t/libx32/ld-linux-x32.so.2\n"
        "#@scan\t/tmp\t1234567890\n"
        "#@rules\trules.d\t0xdeadbeef\n"
        "\n"
        "lib\tlibz.so.1\t/usr/lib/libz.so.1\tlibz.so.1\t-\tNATIVE\t-\t-\t-\n"
        "lib\tlibc.so.6\t/usr/lib/libc.so.6\tlibc.so.6\t-\tNATIVE\t-\t-\t-\n"
        "sym\tlibz.so.1\tdeflate\tlibz.so.1\tdeflate\tNATIVE_TYPED\ti:pi\t-\t0\n"
        "sym\tlibz.so.1\tadler32_combine64\tlibz.so.1\tadler32_combine64@ZLIB_1.2.3.3\tNATIVE_TYPED\tu:uuq\t-\t0\n"
        "sym\tlibz.so.1\tadler32_combine\t-\t-\tFORBIDDEN_ABI\t-\tREASON=scalar-size:off_t:i386=4,x32=8\t0\n"
        "sym\tlibc.so.6\tstat_only\t-\t-\tCOMPAT\ti:p\tSTATIC_ONLY\t0\n"
        "sym\tlibc.so.6\trun_only\t-\t-\tCOMPAT\ti:p\tRUNTIME_ONLY\t0\n"
        "sym\t*\twild_sym\t-\t-\tNATIVE_TYPED\tv:\t-\t0\n"
        "sym\t*\twild_ver@VER_1\t-\t-\tNATIVE_TYPED\tv:\t-\t0\n"
        "sym\tlibc.so.6\talias1\tlibc.so.6\talias2\tALIAS\t-\t-\t0\n"
        "sym\tlibc.so.6\talias2\tlibc.so.6\talias3\tALIAS\t-\t-\t0\n"
        "sym\tlibc.so.6\talias3\tlibc.so.6\talias4\tALIAS\t-\t-\t0\n"
        "sym\tlibc.so.6\talias4\tlibz.so.1\tdeflate\tALIAS\t-\t-\t0\n"
        "sym\tlibc.so.6\tloop1\tlibc.so.6\tloop2\tALIAS\t-\t-\t0\n"
        "sym\tlibc.so.6\tloop2\tlibc.so.6\tloop1\tALIAS\t-\t-\t0\n";

    nm_lookup_t *lk = NULL;
    TEST_ASSERT(nm_lookup_open_mem(sample_table, strlen(sample_table), &lk) == NM_OK);
    TEST_ASSERT(lk != NULL);

    /* Metadata check */
    TEST_ASSERT(strcmp(nm_lookup_libc(lk), "glibc") == 0);
    TEST_ASSERT(strcmp(nm_lookup_interp(lk), "/libx32/ld-linux-x32.so.2") == 0);

    /* Find lib */
    nm_lk_entry_t lib_entry;
    const char *lib_path = NULL;
    TEST_ASSERT(nm_lookup_find_lib(lk, "libz.so.1", &lib_entry, &lib_path) == NM_OK);
    TEST_ASSERT(lib_entry.cls == NM_LK_NATIVE_LIB);
    TEST_ASSERT(lib_path != NULL && strcmp(lib_path, "/usr/lib/libz.so.1") == 0);
    TEST_ASSERT(nm_lookup_find_lib(lk, "libnonexistent.so", &lib_entry, &lib_path) == NM_ERR_DLSYM);

    /* Find exact symbol */
    nm_lk_entry_t ent;
    TEST_ASSERT(nm_lookup_find(lk, "libz.so.1", "deflate", NULL, 0, &ent) == NM_OK);
    TEST_ASSERT(ent.cls == NM_LK_NATIVE_TYPED);
    TEST_ASSERT(strcmp(ent.tgt_soname, "libz.so.1") == 0);
    TEST_ASSERT(strcmp(ent.tgt_sym, "deflate") == 0);
    TEST_ASSERT(ent.sig.ret_kind == NM_RET_I32);
    TEST_ASSERT(ent.sig.nargs == 2);

    /* Find versioned symbol */
    TEST_ASSERT(nm_lookup_find(lk, "libz.so.1", "adler32_combine64", "ZLIB_1.2.3.3", 0, &ent) == NM_OK);
    TEST_ASSERT(ent.cls == NM_LK_NATIVE_TYPED);
    TEST_ASSERT(strcmp(ent.tgt_sym, "adler32_combine64") == 0);
    TEST_ASSERT(strcmp(ent.tgt_ver, "ZLIB_1.2.3.3") == 0);

    /* Find forbidden symbol and reason */
    TEST_ASSERT(nm_lookup_find(lk, "libz.so.1", "adler32_combine", NULL, 0, &ent) == NM_OK);
    TEST_ASSERT(ent.cls == NM_LK_FORBIDDEN);
    TEST_ASSERT(ent.reason != NULL && strstr(ent.reason, "off_t") != NULL);

    /* Priority (3) and (4): Wildcard soname '*' */
    TEST_ASSERT(nm_lookup_find(lk, "any_dso.so", "wild_sym", NULL, 0, &ent) == NM_OK);
    TEST_ASSERT(ent.cls == NM_LK_NATIVE_TYPED);

    TEST_ASSERT(nm_lookup_find(lk, "any_dso.so", "wild_ver", "VER_1", 0, &ent) == NM_OK);
    TEST_ASSERT(ent.cls == NM_LK_NATIVE_TYPED);

    /* Filtering: STATIC_ONLY vs RUNTIME_ONLY */
    /* mode_mask = 0 (runtime default) -> skips STATIC_ONLY */
    TEST_ASSERT(nm_lookup_find(lk, "libc.so.6", "stat_only", NULL, 0, &ent) == NM_ERR_DLSYM);
    TEST_ASSERT(nm_lookup_find(lk, "libc.so.6", "run_only", NULL, 0, &ent) == NM_OK);

    /* mode_mask = NM_SYM_FLAG_STATIC_ONLY -> skips RUNTIME_ONLY */
    TEST_ASSERT(nm_lookup_find(lk, "libc.so.6", "stat_only", NULL, NM_SYM_FLAG_STATIC_ONLY, &ent) == NM_OK);
    TEST_ASSERT(nm_lookup_find(lk, "libc.so.6", "run_only", NULL, NM_SYM_FLAG_STATIC_ONLY, &ent) == NM_ERR_DLSYM);

    /* Alias resolution: 4 hops */
    TEST_ASSERT(nm_lookup_find(lk, "libc.so.6", "alias1", NULL, 0, &ent) == NM_OK);
    TEST_ASSERT(ent.cls == NM_LK_NATIVE_TYPED);
    TEST_ASSERT(strcmp(ent.tgt_sym, "deflate") == 0);

    /* Alias cycle detection */
    TEST_ASSERT(nm_lookup_find(lk, "libc.so.6", "loop1", NULL, 0, &ent) == NM_ERR_DLSYM);

    nm_lookup_close(lk);
    printf("PASS test_lookup_open_and_find\n");
}

static void test_lookup_is_stale(void)
{
    printf("--- [test_lookup_is_stale] ---\n");

    char template_dir[] = "/tmp/nm_test_scan_XXXXXX";
    char *dir_path = mkdtemp(template_dir);
    TEST_ASSERT(dir_path != NULL);

    struct stat st;
    TEST_ASSERT(stat(dir_path, &st) == 0);
    int64_t mtime_ns = (int64_t)st.st_mtim.tv_sec * 1000000000LL + (int64_t)st.st_mtim.tv_nsec;

    char table_buf[1024];
    snprintf(table_buf, sizeof(table_buf),
             "#!newminhh-lookup v1\n"
             "#@generator\tnewminhh-lookup-gen\t1.0.0\n"
             "#@scan\t%s\t%lld\n"
             "lib\tlibz.so.1\t-\tlibz.so.1\t-\tNATIVE\t-\t-\t-\n",
             dir_path, (long long)mtime_ns);

    nm_lookup_t *lk = NULL;
    TEST_ASSERT(nm_lookup_open_mem(table_buf, strlen(table_buf), &lk) == NM_OK);

    /* Initially fresh */
    TEST_ASSERT(nm_lookup_is_stale(lk) == 0);

    /* Touch dir mtime using utimensat */
    struct timespec ts[2];
    ts[0].tv_sec = st.st_atim.tv_sec;
    ts[0].tv_nsec = st.st_atim.tv_nsec;
    ts[1].tv_sec = st.st_mtim.tv_sec + 2;
    ts[1].tv_nsec = st.st_mtim.tv_nsec;
    TEST_ASSERT(utimensat(AT_FDCWD, dir_path, ts, 0) == 0);

    /* Should now report stale */
    TEST_ASSERT(nm_lookup_is_stale(lk) == 1);

    /* Remove dir */
    rmdir(dir_path);

    /* Missing directory should also report stale */
    TEST_ASSERT(nm_lookup_is_stale(lk) == 1);

    nm_lookup_close(lk);
    printf("PASS test_lookup_is_stale\n");
}

static void test_negative_open(void)
{
    printf("--- [test_negative_open] ---\n");

    nm_lookup_t *lk = NULL;

    /* 1. Missing header magic */
    const char bad_magic[] = "bad header line\nlib\tx\tx\tx\tx\tx\tx\tx\tx\n";
    TEST_ASSERT(nm_lookup_open_mem(bad_magic, strlen(bad_magic), &lk) == NM_ERR_ELF);

    /* 2. Malformed columns (< 9 columns) */
    const char bad_cols[] = "#!newminhh-lookup v1\nsym\tonly\tfive\tcols\n";
    TEST_ASSERT(nm_lookup_open_mem(bad_cols, strlen(bad_cols), &lk) == NM_ERR_ABI);

    printf("PASS test_negative_open\n");
}

int main(void)
{
    printf("=== Running nm_lookup C Unit Tests ===\n");
    test_sig_parse_and_format();
    test_lookup_open_and_find();
    test_lookup_is_stale();
    test_negative_open();
    printf("ALL_LOOKUP_C_TESTS_PASSED\n");
    return 0;
}
