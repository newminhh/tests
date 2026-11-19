#include <stdio.h>

static int g_init_seq = 0;

__attribute__((constructor(101)))
static void init_101(void) {
    g_init_seq = g_init_seq * 10 + 1;
    printf("INIT_101\n");
    fflush(stdout);
}

__attribute__((constructor(102)))
static void init_102(void) {
    g_init_seq = g_init_seq * 10 + 2;
    printf("INIT_102\n");
    fflush(stdout);
}

__attribute__((destructor(102)))
static void fini_102(void) {
    printf("FINI_102\n");
    fflush(stdout);
}

__attribute__((destructor(101)))
static void fini_101(void) {
    printf("FINI_101\n");
    fflush(stdout);
}

int main(void) {
    if (g_init_seq != 12) {
        fprintf(stderr, "init order mismatch: %d\n", g_init_seq);
        return 1;
    }
    printf("MAIN\n");
    fflush(stdout);
    return 0;
}
