#define _GNU_SOURCE
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct align_probe {
    int tag;
    uint64_t val;
};

int main(void) {
    char path[128];
    char link_path[128];
    snprintf(path, sizeof(path), "/tmp/newminhh_c05_%ld.dat", (long)getpid());
    snprintf(link_path, sizeof(link_path), "/tmp/newminhh_c05_%ld.lnk", (long)getpid());

    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    char payload[123];
    memset(payload, 'A', sizeof(payload));
    if (write(fd, payload, sizeof(payload)) != (ssize_t)sizeof(payload)) {
        close(fd);
        unlink(path);
        return 1;
    }

    struct stat st_stat, st_fstat, st_lstat;
    memset(&st_stat, 0, sizeof(st_stat));
    memset(&st_fstat, 0, sizeof(st_fstat));
    memset(&st_lstat, 0, sizeof(st_lstat));

    if (stat(path, &st_stat) != 0 || fstat(fd, &st_fstat) != 0) {
        perror("stat/fstat");
        close(fd);
        unlink(path);
        return 1;
    }

    unlink(link_path);
    if (symlink(path, link_path) != 0) {
        perror("symlink");
        close(fd);
        unlink(path);
        return 1;
    }
    if (lstat(link_path, &st_lstat) != 0) {
        perror("lstat");
        close(fd);
        unlink(link_path);
        unlink(path);
        return 1;
    }

    close(fd);
    unlink(link_path);
    unlink(path);

    int stat_ok = (st_stat.st_size == 123) &&
                  (st_fstat.st_size == 123) &&
                  (st_stat.st_ino == st_fstat.st_ino) &&
                  (st_stat.st_ino != 0) &&
                  S_ISREG(st_stat.st_mode) &&
                  S_ISLNK(st_lstat.st_mode);

    struct timespec t0, t1, req;
    if (clock_gettime(CLOCK_MONOTONIC, &t0) != 0) {
        perror("clock_gettime t0");
        return 1;
    }
    req.tv_sec = 0;
    req.tv_nsec = 2000000L; /* 2ms */
    nanosleep(&req, NULL);
    if (clock_gettime(CLOCK_MONOTONIC, &t1) != 0) {
        perror("clock_gettime t1");
        return 1;
    }
    long long elapsed_ns = ((long long)t1.tv_sec - (long long)t0.tv_sec) * 1000000000LL +
                           ((long long)t1.tv_nsec - (long long)t0.tv_nsec);
    int elapsed_ok = (elapsed_ns > 0);

    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) {
        perror("getrlimit");
        return 1;
    }
    struct rlimit rl_new = rl;
    if (rl_new.rlim_cur > 128 && rl_new.rlim_cur <= rl_new.rlim_max) {
        rl_new.rlim_cur -= 1;
        if (setrlimit(RLIMIT_NOFILE, &rl_new) != 0) {
            perror("setrlimit");
            return 1;
        }
        struct rlimit rl_check;
        if (getrlimit(RLIMIT_NOFILE, &rl_check) != 0 || rl_check.rlim_cur != rl_new.rlim_cur) {
            fprintf(stderr, "rlimit verify failed\n");
            return 1;
        }
        /* Restore */
        setrlimit(RLIMIT_NOFILE, &rl);
    }
    int rlim_ok = (rl.rlim_cur > 0 && rl.rlim_cur <= rl.rlim_max);

    struct align_probe probe;
    probe.tag = 42;
    probe.val = 0x1122334455667788ULL;
    size_t off_val = offsetof(struct align_probe, val);
    int align_ok = (probe.tag == 42 && probe.val == 0x1122334455667788ULL &&
                    (off_val == 4 || off_val == 8));

    if (!stat_ok || !elapsed_ok || !rlim_ok || !align_ok) {
        fprintf(stderr, "STRUCTS_FAIL stat=%d elapsed=%d rlim=%d align=%d\n",
                stat_ok, elapsed_ok, rlim_ok, align_ok);
        return 1;
    }

    printf("STRUCTS_OK size=%lld ino_ok=1 elapsed_ok=1 rlim_ok=1 align_ok=1\n",
           (long long)st_stat.st_size);
    return 0;
}
