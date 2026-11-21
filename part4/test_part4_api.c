#define _GNU_SOURCE
#include "../../src/common/newminhh_cstd.h"
#include "../../src/dbt/dbt_internal.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int test_enter_at(void) {
    /*
     * Allocate a PROT_READ (NO PROT_EXEC!) page containing raw i386 code:
     *   Snippet 1 (offset 0):
     *     40          inc eax        (i386 1-byte inc, invalid as opcode in Long Mode!)
     *     83 c0 0a    add eax, 10
     *     c3          ret
     *   Snippet 2 (offset 64):
     *     b8 2a 00 00 00  mov eax, 42
     *     48              dec eax    (i386 1-byte dec!)
     *     c3              ret
     */
    uint8_t *page = (uint8_t *)mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    const uint8_t snip1[] = {0x40, 0x83, 0xC0, 0x0A, 0xC3};
    const uint8_t snip2[] = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0x48, 0xC3};
    memcpy(page, snip1, sizeof(snip1));
    memcpy(page + 64, snip2, sizeof(snip2));
    if (mprotect(page, 4096, PROT_READ) != 0) {
        perror("mprotect");
        munmap(page, 4096);
        return 1;
    }

    newminhh_cpu_t cpu = {};
    cpu.gpr[NM_REG_EAX] = 30u;
    int rc = newminhh_dbt_enter_at((uint32_t)(uintptr_t)page, &cpu);
    if (rc != NM_OK || cpu.gpr[NM_REG_EAX] != 41u) {
        fprintf(stderr, "enter_at(&cpu) failed: rc=%d eax=%u (expected 41)\n",
                rc, cpu.gpr[NM_REG_EAX]);
        munmap(page, 4096);
        return 1;
    }

    int ret2 = newminhh_dbt_enter_at((uint32_t)(uintptr_t)(page + 64), nullptr);
    if (ret2 != 41) {
        fprintf(stderr, "enter_at(nullptr) failed: got %d (expected 41)\n", ret2);
        munmap(page, 4096);
        return 1;
    }

    munmap(page, 4096);
    printf("PASS test_enter_at\n");
    return 0;
}

static int test_exec_embedded(const char *elf_path) {
    const uint8_t bad[16] = {'B', 'A', 'D', 0};
    if (newminhh_dbt_exec_embedded(bad, sizeof(bad), 0, 0, nullptr, nullptr) >= 0) {
        fprintf(stderr, "exec_embedded unexpectedly succeeded on invalid blob\n");
        return 1;
    }

    int fd = open(elf_path, O_RDONLY);
    if (fd < 0) {
        perror("open elf_path");
        return 1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return 1;
    }
    size_t sz = (size_t)st.st_size;
    uint8_t *blob = (uint8_t *)malloc(sz);
    if (!blob) {
        close(fd);
        return 1;
    }
    if (read(fd, blob, sz) != (ssize_t)sz) {
        free(blob);
        close(fd);
        return 1;
    }
    close(fd);

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        free(blob);
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        free(blob);
        return 1;
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        char arg_name[] = "embedded_switch";
        char *argv_em[] = {arg_name, nullptr};
        int rc = newminhh_dbt_exec_embedded(blob, sz, 0, 1, argv_em, nullptr);
        _exit(rc < 0 ? 99 : rc);
    }
    close(pipefd[1]);
    free(blob);

    char out[256] = {};
    ssize_t n = read(pipefd[0], out, sizeof(out) - 1);
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || n <= 0 ||
        strstr(out, "SWITCH_OK csum=0x67f2271e") == nullptr) {
        fprintf(stderr, "exec_embedded failed: status=0x%x out='%s'\n", status, out);
        return 1;
    }

    printf("PASS test_exec_embedded\n");
    return 0;
}

int main(int argc, char **argv) {
    const char *elf_path = (argc > 1 && argv != nullptr) ? argv[1] :
        "/root/newminhh/tests/corpus/build/i386/cli_switch_jumptable_pie";

    if (test_enter_at() != 0) return 1;
    if (test_exec_embedded(elf_path) != 0) return 1;
    printf("ALL_PART4_API_SMOKE_TESTS_PASSED\n");
    return 0;
}
