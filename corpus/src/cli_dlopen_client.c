#include "newminhh_cstd.h"
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

typedef double (*fn_d_d_t)(double);
typedef int (*plugin_compute_t)(int, int);

int main(int argc, char **argv) {
    const char *plugin_path = (argc > 1) ? argv[1] : "./libplugin_stripped.so";

    void *libm = dlopen("libm.so.6", RTLD_NOW);
    if (!libm) {
        fprintf(stderr, "dlopen libm.so.6 failed: %s\n", dlerror());
        return 1;
    }

    dlerror();
    fn_d_d_t p_sqrt = NM_VOIDP_TO_FN(fn_d_d_t, dlsym(libm, "sqrt"));
    const char *err = dlerror();
    if (err || !p_sqrt) {
        fprintf(stderr, "dlsym sqrt failed: %s\n", err ? err : "null");
        dlclose(libm);
        return 1;
    }

    fn_d_d_t p_cos = NM_VOIDP_TO_FN(fn_d_d_t, dlsym(libm, "cos"));
    err = dlerror();
    if (err || !p_cos) {
        fprintf(stderr, "dlsym cos failed: %s\n", err ? err : "null");
        dlclose(libm);
        return 1;
    }

    double sq = p_sqrt(25.0);
    double cs = p_cos(0.0);
    if (fabs(sq - 5.0) > 1e-6 || fabs(cs - 1.0) > 1e-6) {
        fprintf(stderr, "libm math mismatch: sqrt=%f cos=%f\n", sq, cs);
        dlclose(libm);
        return 1;
    }
    printf("DLOPEN_LIBM_OK sqrt=%.4f cos=%.4f\n", sq, cs);
    dlclose(libm);

    void *plug = dlopen(plugin_path, RTLD_NOW);
    if (!plug) {
        plug = dlopen("libplugin_stripped.so", RTLD_NOW);
    }
    if (!plug) {
        fprintf(stderr, "dlopen plugin failed: %s\n", dlerror());
        return 1;
    }

    dlerror();
    plugin_compute_t p_comp = NM_VOIDP_TO_FN(plugin_compute_t, dlsym(plug, "plugin_compute"));
    err = dlerror();
    if (err || !p_comp) {
        fprintf(stderr, "dlsym plugin_compute failed: %s\n", err ? err : "null");
        dlclose(plug);
        return 1;
    }

    int res = p_comp(7, 5);
    if (res != 47) {
        fprintf(stderr, "plugin_compute unexpected: %d\n", res);
        dlclose(plug);
        return 1;
    }
    printf("DLOPEN_PLUGIN_OK res=%d\n", res);
    dlclose(plug);
    return 0;
}
