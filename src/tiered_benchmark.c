#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include "tiered_sched.h"

#define BENCH_SIZE      (1 * 1024 * 1024)
#define BENCH_RUNS      3
#define BENCH_BLOCK     512

int tv_benchmark(const char *disk_path, uint64_t *speed_out) {
    if (!disk_path || !speed_out) return -1;
    *speed_out = 0;

    int fd = open(disk_path, O_RDWR | O_DIRECT);
    if (fd < 0) {
        fprintf(stderr, "benchmark: cannot open '%s': %s\n", disk_path, strerror(errno));
        return -1;
    }

    void *buf = NULL;
    if (posix_memalign(&buf, BENCH_BLOCK, BENCH_SIZE) != 0) {
        close(fd);
        return -1;
    }

    memset(buf, 0xAB, BENCH_SIZE);

    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    for (int run = 0; run < BENCH_RUNS; run++) {
        uint64_t written = 0;
        while (written < BENCH_SIZE) {
            ssize_t n = pwrite(fd, (char *)buf + written, BENCH_SIZE - written, written);
            if (n < 0) {
                if (errno == EINTR) continue;
                fprintf(stderr, "benchmark: write error on '%s': %s\n", disk_path, strerror(errno));
                free(buf);
                close(fd);
                return -1;
            }
            written += n;
        }
        fsync(fd);
    }

    clock_gettime(CLOCK_MONOTONIC, &ts_end);

    free(buf);
    close(fd);

    double elapsed = (ts_end.tv_sec - ts_start.tv_sec) +
                     (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;
    if (elapsed <= 0) elapsed = 0.001;

    uint64_t total = (uint64_t)BENCH_SIZE * BENCH_RUNS;
    *speed_out = (uint64_t)((double)total / elapsed / (1024.0 * 1024.0));

    return 0;
}
