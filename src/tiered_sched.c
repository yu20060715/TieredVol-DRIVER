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

    sched->flush_data = malloc((size_t)stripe_size);
    if (!sched->flush_data) {
        tv_buf_destroy(&sched->buf);
        tv_uring_destroy(&sched->ring);
        free(sched);
        return NULL;
    }
    sched->flush_pending = 0;
    sched->flush_submitted = 0;

    return sched;
}

int tv_write(TV_SCHED *sched, const void *buf, uint64_t len) {
    if (!sched || !buf || len == 0) return -1;

    const uint8_t *src = (const uint8_t *)buf;
    uint64_t pos = 0;

    while (pos < len) {
        uint64_t space = sched->buf.capacity - sched->buf.used;
        if (space == 0) {
            /* Buffer full — flush before writing more */
            if (sched->flush_pending) {
                if (tv_flush_wait(sched) < 0) return -1;
            }
            if (tv_flush_submit(sched) < 0) return -1;
            space = sched->buf.capacity - sched->buf.used;
            if (space == 0) {
                fprintf(stderr, "tv_write: buffer still full after flush\n");
                return -1;
            }
        }
        uint64_t chunk = (len - pos < space) ? (len - pos) : space;

        tv_buf_write(&sched->buf, src + pos, chunk);
        pos += chunk;
    }

    return 0;
}

static int flush_submit_io(TV_SCHED *sched, uint64_t logical, uint8_t *data, uint64_t data_len) {
    TV_SEGMENT *seg = NULL;
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
    uint64_t remaining = data_len;
    int submitted = 0;

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

        if (tv_uring_write(&sched->ring, fd, data + buf_pos,
                           (size_t)write_bytes, (off_t)disk_off) < 0) {
            fprintf(stderr, "tv_flush: SQE allocation failed for disk %u\n",
                    seg->disk_index[i]);
            return -1;
        }
        sched->flush_expected[submitted] = write_bytes;
        buf_pos += write_bytes;
        remaining -= write_bytes;
        submitted++;
    }

    if (tv_uring_submit(&sched->ring) < 0) {
        fprintf(stderr, "tv_flush: submit failed\n");
        return -1;
    }
    return submitted;
}

/* Submit flush without waiting — I/O runs in background */
static int tv_flush_submit(TV_SCHED *sched) {
    if (!sched || sched->buf.used == 0) return 0;

    memcpy(sched->flush_data, sched->buf.data, (size_t)sched->buf.used);
    uint64_t logical = sched->buf.logical_begin;
    uint64_t data_len = sched->buf.used;

    tv_buf_reset(&sched->buf);

    int submitted = flush_submit_io(sched, logical, sched->flush_data, data_len);
    if (submitted < 0) return -1;

    sched->flush_pending = 1;
    sched->flush_submitted = submitted;
    return 0;
}

/* Wait for a pending async flush to complete */
static int tv_flush_wait(TV_SCHED *sched) {
    if (!sched->flush_pending) return 0;

    int had_error = 0;
    for (int i = 0; i < sched->flush_submitted; i++) {
        int res = tv_uring_wait(&sched->ring);
        if (res < 0) {
            fprintf(stderr, "tv_flush: I/O error on pending stripe\n");
            had_error = 1;
        } else if ((uint64_t)res < sched->flush_expected[i]) {
            fprintf(stderr, "tv_flush: short write: %d/%lu bytes\n",
                    res, (unsigned long)sched->flush_expected[i]);
            had_error = 1;
        }
    }
    sched->flush_pending = 0;
    sched->flush_submitted = 0;
    return had_error ? -1 : 0;
}

/* Explicit flush: wait for pending, then flush any remaining buffer data */
int tv_flush(TV_SCHED *sched) {
    if (!sched) return -1;

    if (sched->flush_pending) {
        if (tv_flush_wait(sched) < 0) return -1;
    }

    if (sched->buf.used == 0) return 0;

    int submitted = flush_submit_io(sched, sched->buf.logical_begin,
                                    sched->buf.data, sched->buf.used);
    if (submitted < 0) return -1;

    int had_error = 0;
    for (int i = 0; i < submitted; i++) {
        int res = tv_uring_wait(&sched->ring);
        if (res < 0) {
            fprintf(stderr, "tv_flush: I/O error on stripe at %lu\n",
                    (unsigned long)sched->buf.logical_begin);
            had_error = 1;
        } else if ((uint64_t)res < sched->flush_expected[i]) {
            fprintf(stderr, "tv_flush: short write: %d/%lu bytes\n",
                    res, (unsigned long)sched->flush_expected[i]);
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

    while (pos < len) {
        TV_MAP map = tv_map_logical(offset + pos, sched->meta);
        if (map.disk < 0 || map.disk >= sched->ndisks) {
            fprintf(stderr, "tv_read: invalid disk index %d\n", map.disk);
            return -1;
        }
        uint64_t chunk = len - pos;
        if (chunk > map.length) chunk = map.length;

        /* Submit single I/O and wait — simple and correct for sequential reads */
        int fd = sched->disks[map.disk].fd;
        if (tv_uring_read(&sched->ring, fd, dst + pos, (size_t)chunk, (off_t)map.offset) < 0) {
            fprintf(stderr, "tv_read: cannot allocate SQE for disk %d\n", map.disk);
            return -1;
        }
        if (tv_uring_submit(&sched->ring) < 0) {
            fprintf(stderr, "tv_read: submit failed\n");
            return -1;
        }
        int res = tv_uring_wait(&sched->ring);
        if (res < 0) {
            fprintf(stderr, "tv_read: I/O error on disk %d at offset %lu\n",
                    map.disk, (unsigned long)map.offset);
            return -1;
        }
        if ((uint64_t)res < chunk) {
            fprintf(stderr, "tv_read: short read on disk %d: %d/%lu bytes\n",
                    map.disk, res, (unsigned long)chunk);
            return -1;
        }
        pos += chunk;
    }

    return 0;
}

void tv_sched_destroy(TV_SCHED *sched) {
    if (!sched) return;
    if (sched->flush_pending) {
        fprintf(stderr, "WARNING: flushing pending async data\n");
        tv_flush_wait(sched);
    }
    if (sched->buf.used > 0) {
        fprintf(stderr, "WARNING: flushing %lu bytes of unflushed data\n",
                (unsigned long)sched->buf.used);
        tv_flush(sched);
    }
    free(sched->flush_data);
    tv_uring_destroy(&sched->ring);
    tv_buf_destroy(&sched->buf);
    free(sched);
}
