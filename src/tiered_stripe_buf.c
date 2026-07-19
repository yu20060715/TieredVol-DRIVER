#include <stdlib.h>
#include <string.h>
#include "tiered_sched.h"

int tv_buf_init(TV_BUFFER *buf, uint64_t stripe_size) {
    if (!buf || stripe_size == 0) return -1;

    buf->data = aligned_alloc(512, (size_t)stripe_size);
    if (!buf->data) return -1;

    buf->used = 0;
    buf->capacity = stripe_size;
    buf->logical_begin = 0;
    return 0;
}

int tv_buf_write(TV_BUFFER *buf, const void *data, uint64_t len) {
    if (!buf || !buf->data) return -1;

    uint64_t space = buf->capacity - buf->used;
    if (len > space) len = space;

    memcpy(buf->data + buf->used, data, (size_t)len);
    buf->used += len;

    return (buf->used == buf->capacity) ? 1 : 0;
}

void tv_buf_reset(TV_BUFFER *buf) {
    if (!buf) return;
    buf->logical_begin += buf->used;
    buf->used = 0;
}

void tv_buf_destroy(TV_BUFFER *buf) {
    if (!buf) return;
    free(buf->data);
    buf->data = NULL;
    buf->used = 0;
    buf->capacity = 0;
}
