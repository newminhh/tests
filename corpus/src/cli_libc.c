#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint32_t g_table[8] = {3, 1, 4, 1, 5, 9, 2, 6};

__attribute__((noinline))
static uint32_t mix_op(uint32_t acc, uint32_t x) {
    return (acc * 33u) ^ (x + 0x9e3779b9u);
}

typedef uint32_t (*op_fn_t)(uint32_t, uint32_t);
static op_fn_t g_op_fn = mix_op;

int main(void) {
    const char *env_val = getenv("NEWMINHH_TEST_ENV");
    if (!env_val) {
        env_val = "default_ok";
    }

    size_t n = 16;
    uint32_t *buf = (uint32_t *)calloc(n, sizeof(uint32_t));
    if (!buf) {
        fprintf(stderr, "calloc failed\n");
        return 1;
    }
    for (size_t i = 0; i < n; i++) {
        buf[i] = g_table[i % 8] + (uint32_t)i;
    }

    uint32_t *grown = (uint32_t *)realloc(buf, 24 * sizeof(uint32_t));
    if (!grown) {
        free(buf);
        fprintf(stderr, "realloc failed\n");
        return 1;
    }
    buf = grown;
    for (size_t i = 16; i < 24; i++) {
        buf[i] = (uint32_t)(i * 3u);
    }

    uint32_t *copy = (uint32_t *)malloc(24 * sizeof(uint32_t));
    if (!copy) {
        free(buf);
        return 1;
    }
    memcpy(copy, buf, 24 * sizeof(uint32_t));

    uint32_t sum = 0;
    for (size_t i = 0; i < 24; i++) {
        sum = g_op_fn(sum, copy[i]);
    }
    free(copy);
    free(buf);

    char msg[128];
    int msg_len = snprintf(msg, sizeof(msg), "newminhh-io-%u", sum & 0xffffu);
    if (msg_len <= 0 || strcmp(msg, "") == 0) {
        return 1;
    }

    char path[128];
    snprintf(path, sizeof(path), "/tmp/newminhh_c01_%ld.tmp", (long)getpid());
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0) {
        perror("open");
        return 1;
    }
    ssize_t wr = write(fd, msg, (size_t)msg_len);
    if (wr != msg_len) {
        close(fd);
        unlink(path);
        return 1;
    }
    if (lseek(fd, 0, SEEK_SET) != 0) {
        close(fd);
        unlink(path);
        return 1;
    }
    char rbuf[128];
    memset(rbuf, 0, sizeof(rbuf));
    ssize_t rd = read(fd, rbuf, sizeof(rbuf) - 1);
    close(fd);
    unlink(path);

    if (rd != msg_len || strcmp(rbuf, msg) != 0) {
        fprintf(stderr, "io mismatch\n");
        return 1;
    }

    printf("LIBC_OK sum=0x%08x env=%s io=%ld\n", sum, env_val, (long)rd);
    return 0;
}
