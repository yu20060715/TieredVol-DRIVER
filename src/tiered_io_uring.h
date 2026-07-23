#ifndef TIERED_IO_URING_H
#define TIERED_IO_URING_H

#include <liburing.h>
#include <sys/uio.h>

int  tv_uring_init(struct io_uring *ring, int queue_depth);
int  tv_uring_register_buffers(struct io_uring *ring, struct iovec *iovecs, int count);
int  tv_uring_unregister_buffers(struct io_uring *ring);
int  tv_uring_write(struct io_uring *ring, int fd, const void *buf, size_t len, off_t offset, void *user_data);
int  tv_uring_write_fixed(struct io_uring *ring, int fd, const void *buf, size_t len, off_t offset, int buf_index, void *user_data);
int  tv_uring_read(struct io_uring *ring, int fd, void *buf, size_t len, off_t offset);
int  tv_uring_submit(struct io_uring *ring);
int  tv_uring_wait(struct io_uring *ring);
void tv_uring_destroy(struct io_uring *ring);

#endif
