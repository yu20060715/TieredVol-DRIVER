#ifndef TIERED_SCHED_H
#define TIERED_SCHED_H

#include "tiered_types.h"
#include <liburing.h>

typedef struct {
    struct io_uring ring;
    TV_METADATA *meta;
    TV_DISK     *disks;
    int          ndisks;
    TV_STRIPE_BUF sbuf[TV_BUF_COUNT];
    uint64_t      stripe_size;
    int           sbuf_head;
    uint64_t      sbuf_used;
    uint64_t      sbuf_logical;
    int           inflight;
    int           buffers_registered;
} TV_SCHED;

TV_SCHED *tv_sched_init(TV_DISK *disks, int ndisks, TV_METADATA *meta);
int       tv_write(TV_SCHED *sched, const void *buf, uint64_t len);
int       tv_read(TV_SCHED *sched, void *buf, uint64_t len, uint64_t offset);
int       tv_flush(TV_SCHED *sched);
int       tv_sched_seek(TV_SCHED *sched, uint64_t offset);
void      tv_sched_destroy(TV_SCHED *sched);

#endif
