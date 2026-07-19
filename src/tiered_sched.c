#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tiered_sched.h"

TV_SCHED *tv_sched_init(TV_DISK *disks, int ndisks, TV_METADATA *meta) {
    if (!disks || ndisks <= 0 || !meta) return NULL;
    if (meta->segment_count == 0) return NULL;

    TV_SCHED *sched = calloc(1, sizeof(TV_SCHED));
    if (!sched) return NULL;

    sched->disks = disks;
    sched->ndisks = ndisks;
    sched->meta = meta;

    if (tv_uring_init(&sched->ring, 256) < 0) {
        free(sched);
        return NULL;
    }

    uint64_t stripe_size = meta->segments[0].stripe_size;
    if (tv_buf_init(&sched->buf, stripe_size) < 0) {
        tv_uring_destroy(&sched->ring);
        free(sched);
        return NULL;
    }

    return sched;
}

int tv_write(TV_SCHED *sched, const void *buf, uint64_t len) {
    if (!sched || !buf || len == 0) return -1;

    const uint8_t *src = (const uint8_t *)buf;
    uint64_t pos = 0;

    while (pos < len) {
        uint64_t space = sched->buf.capacity - sched->buf.used;
        uint64_t chunk = (len - pos < space) ? (len - pos) : space;

        tv_buf_write(&sched->buf, src + pos, chunk);
        pos += chunk;

        if (sched->buf.used == sched->buf.capacity) {
            int ret = tv_flush(sched);
            if (ret < 0) return -1;
        }
    }

    return 0;
}

int tv_flush(TV_SCHED *sched) {
    if (!sched || sched->buf.used == 0) return 0;

    uint64_t logical = sched->buf.logical_begin;
    TV_SEGMENT *seg = NULL;

    /* Find the segment that contains this logical offset */
    for (int i = 0; i < (int)sched->meta->segment_count; i++) {
        if (logical >= sched->meta->segments[i].logical_begin &&
            logical <  sched->meta->segments[i].logical_end) {
            seg = &sched->meta->segments[i];
            break;
        }
    }

    if (!seg) {
        fprintf(stderr, "tv_flush: logical offset %lu not in any segment\n",
                (unsigned long)logical);
        return -1;
    }

    uint64_t stripe_no = (logical - seg->logical_begin) / seg->stripe_size;

    uint64_t buf_pos = 0;
    uint64_t remaining = sched->buf.used;
    int submitted = 0;
    int had_error = 0;

    for (int i = 0; i < (int)seg->disk_count && remaining > 0; i++) {
        uint64_t disk_bytes = (uint64_t)seg->weight[i] * TV_CHUNK_SIZE;
        if (disk_bytes == 0) continue;

        if (seg->disk_index[i] >= (uint32_t)sched->ndisks) {
            fprintf(stderr, "tv_flush: invalid disk index %u\n", seg->disk_index[i]);
            return -1;
        }

        uint64_t write_bytes = (disk_bytes < remaining) ? disk_bytes : remaining;
        uint64_t disk_off = stripe_no * disk_bytes;
        int fd = sched->disks[seg->disk_index[i]].fd;

        if (tv_uring_write(&sched->ring, fd,
                           sched->buf.data + buf_pos,
                           (size_t)write_bytes, (off_t)disk_off) < 0) {
            fprintf(stderr, "tv_flush: SQE allocation failed for disk %u\n",
                    seg->disk_index[i]);
            had_error = 1;
            break;
        }

        buf_pos += write_bytes;
        remaining -= write_bytes;
        submitted++;
    }

    if (tv_uring_submit(&sched->ring) < 0) {
        fprintf(stderr, "tv_flush: submit failed\n");
        had_error = 1;
    }

    for (int i = 0; i < submitted; i++) {
        int res = tv_uring_wait(&sched->ring);
        if (res < 0) {
            fprintf(stderr, "tv_flush: I/O error on stripe at %lu\n",
                    (unsigned long)logical);
            had_error = 1;
        }
    }

    tv_buf_reset(&sched->buf);
    return had_error ? -1 : 0;
}

int tv_read(TV_SCHED *sched, void *buf, uint64_t len, uint64_t offset) {
    if (!sched || !buf || len == 0) return -1;

    uint8_t *dst = (uint8_t *)buf;
    uint64_t pos = 0;
    int pending = 0;
    int had_error = 0;

    while (pos < len) {
        TV_MAP map = tv_map_logical(offset + pos, sched->meta);
        if (map.disk < 0 || map.disk >= sched->ndisks) {
            fprintf(stderr, "tv_read: invalid disk index %d\n", map.disk);
            return -1;
        }
        uint64_t chunk = len - pos;
        if (chunk > map.length) chunk = map.length;

        int fd = sched->disks[map.disk].fd;
        if (tv_uring_read(&sched->ring, fd, dst + pos, (size_t)chunk, (off_t)map.offset) < 0) {
            /* SQE full — submit what we have and wait */
            if (pending > 0) {
                if (tv_uring_submit(&sched->ring) < 0) {
                    had_error = 1;
                }
                for (int i = 0; i < pending; i++) {
                    int res = tv_uring_wait(&sched->ring);
                    if (res < 0) {
                        fprintf(stderr, "tv_read: I/O error on request %d\n", i);
                        had_error = 1;
                    }
                }
                pending = 0;
            }
            /* Retry this chunk */
            if (tv_uring_read(&sched->ring, fd, dst + pos, (size_t)chunk, (off_t)map.offset) < 0) {
                fprintf(stderr, "tv_read: cannot allocate SQE for disk %d\n", map.disk);
                return -1;
            }
        }
        pos += chunk;
        pending++;
    }

    if (tv_uring_submit(&sched->ring) < 0) {
        fprintf(stderr, "tv_read: submit failed\n");
        had_error = 1;
    }

    for (int i = 0; i < pending; i++) {
        int res = tv_uring_wait(&sched->ring);
        if (res < 0) {
            fprintf(stderr, "tv_read: I/O error on request %d\n", i);
            had_error = 1;
        }
    }

    return had_error ? -1 : 0;
}

void tv_sched_destroy(TV_SCHED *sched) {
    if (!sched) return;
    tv_uring_destroy(&sched->ring);
    tv_buf_destroy(&sched->buf);
    free(sched);
}
