#include <dlfcn.h>
#include <stdio.h>

int main(int argc, char **argv) {
    const char *so_path = (argc > 1) ? argv[1] : "libc.so.6";
    const char *sym_name = (argc > 2) ? argv[2] : "newminhh_nonexistent_sym";

    void *h = dlopen(so_path, RTLD_NOW);
    if (!h) {
        fprintf(stderr, "N09_DIAG: dlopen(%s) failed: %s\n", so_path, dlerror());
        return 2;
    }
    dlerror();
    void *sym = dlsym(h, sym_name);
    const char *err = dlerror();
    if (!sym) {
        fprintf(stderr, "N09_EXPECTED_FAIL: dlsym(%s) missing: %s\n",
                sym_name, err ? err : "symbol not found");
        dlclose(h);
        return 3;
    }
    fprintf(stderr, "N09_UNEXPECTED_SUCCESS: %s resolved\n", sym_name);
    dlclose(h);
    return 0;
}
