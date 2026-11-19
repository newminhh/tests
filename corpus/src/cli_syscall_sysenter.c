#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#if defined(__i386__)

#define NM_SYS_write      4
#define NM_SYS_getpid     20
#define NM_SYS_exit_group 252

extern int32_t nm_sysenter_call(uint32_t nr, uint32_t a1, uint32_t a2, uint32_t a3);

int main(void) {
    int32_t pid = nm_sysenter_call(NM_SYS_getpid, 0, 0, 0);
    if (pid <= 0) {
        nm_sysenter_call(NM_SYS_exit_group, 1, 0, 0);
    }

    char out[96];
    int len = snprintf(out, sizeof(out), "SYSENTER_OK pid_ok=1\n");
    if (len > 0) {
        nm_sysenter_call(NM_SYS_write, 1, (uint32_t)(uintptr_t)out, (uint32_t)len);
    }
    nm_sysenter_call(NM_SYS_exit_group, 0, 0, 0);
    return 0;
}

#else /* Native x32 baseline */

int main(void) {
    pid_t pid = getpid();
    if (pid <= 0) {
        _exit(1);
    }

    char out[96];
    int len = snprintf(out, sizeof(out), "SYSENTER_OK pid_ok=1\n");
    if (len > 0) {
        ssize_t wr = write(1, out, (size_t)len);
        (void)wr;
    }
    _exit(0);
    return 0;
}

#endif
