#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include "tiered_sched.h"

int tv_uring_init(struct io_uring *ring, int queue_depth) {
    int ret = io_uring_queue_init(queue_depth, ring, 0);
    if (ret < 0) {
        fprintf(stderr, "io_uring_queue_init failed: %s\n", strerror(-ret));
        return -1;
    }
    return 0;
}

int tv_uring_write(struct io_uring *ring, int fd, const void *buf, size_t len, off_t offset, void *user_data) {
    if (len > UINT_MAX) {
        fprintf(stderr, "tv_uring_write: len %zu exceeds UINT_MAX\n", len);
        return -1;
    }
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) return -1;

    io_uring_prep_write(sqe, fd, buf, (unsigned)len, offset);
    io_uring_sqe_set_data(sqe, user_data);
    return 0;
}

int tv_uring_read(struct io_uring *ring, int fd, void *buf, size_t len, off_t offset) {
    if (len > UINT_MAX) {
        fprintf(stderr, "tv_uring_read: len %zu exceeds UINT_MAX\n", len);
        return -1;
    }
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) return -1;

    io_uring_prep_read(sqe, fd, buf, (unsigned)len, offset);
    io_uring_sqe_set_data(sqe, (void *)(intptr_t)-1);
    return 0;
}

int tv_uring_submit(struct io_uring *ring) {
    int ret = io_uring_submit(ring);
    if (ret < 0) {
        fprintf(stderr, "io_uring_submit failed: %s\n", strerror(-ret));
        return -1;
    }
    return 0;
}

int tv_uring_wait(struct io_uring *ring) {
    struct io_uring_cqe *cqe = NULL;
    struct __kernel_timespec ts = { .tv_sec = TV_CQE_TIMEOUT_SEC, .tv_nsec = 0 };
    int ret = io_uring_wait_cqe_timeout(ring, &cqe, &ts);
    if (ret == -ETIME) {
        fprintf(stderr, "io_uring_wait_cqe timed out (%ds)\n", TV_CQE_TIMEOUT_SEC);
        return -ETIME;
    }
    if (ret < 0) {
        fprintf(stderr, "io_uring_wait_cqe failed: %s\n", strerror(-ret));
        return -1;
    }
    int res = cqe->res;
    io_uring_cqe_seen(ring, cqe);
    return res;
}

void tv_uring_destroy(struct io_uring *ring) {
    io_uring_queue_exit(ring);
}
