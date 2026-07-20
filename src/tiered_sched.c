#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "tiered_sched.h"

volatile sig_atomic_t g_shutdown_requested = 0;

/* Forward declarations */
static int flush_submit_io(TV_SCHED *sched, uint64_t logical, uint8_t *data, uint64_t data_len, int buf_idx);

TV_SCHED *tv_sched_init(TV_DISK *disks, int ndisks, TV_METADATA *meta) {
    if (!disks || ndisks <= 0 || !meta) return NULL;
    if (meta->segment_count == 0) return NULL;

    TV_SCHED *sched = calloc(1, sizeof(TV_SCHED));
    if (!sched) return NULL;

    sched->disks = disks;
    sched->ndisks = ndisks;
    sched->meta = meta;
    sched->stripe_size = meta->segments[0].stripe_size;

    if (tv_uring_init(&sched->ring, 256) < 0) {
        free(sched);
        return NULL;
    }

    for (int i = 0; i < TV_BUF_COUNT; i++) {
        sched->sbuf[i].data = aligned_alloc(512, (size_t)sched->stripe_size);
        if (!sched->sbuf[i].data) {
            for (int j = 0; j < i; j++) free(sched->sbuf[j].data);
            tv_uring_destroy(&sched->ring);
            free(sched);
            return NULL;
        }
        sched->sbuf[i].in_flight = 0;
        sched->sbuf[i].cqes_pending = 0;
    }
    sched->sbuf_head = 0;
    sched->sbuf_used = 0;
    sched->sbuf_logical = 0;
    sched->inflight = 0;

    return sched;
}

/* Reap completed CQEs using user_data to identify the correct buffer */
static void reap_completed(TV_SCHED *sched) {
    struct io_uring_cqe *cqe;
    while (sched->inflight > 0) {
        int ret = io_uring_peek_cqe(&sched->ring, &cqe);
        if (ret < 0 || !cqe) break;
        if (cqe->res < 0)
            fprintf(stderr, "tv_sched: I/O error res=%d\n", cqe->res);
        int buf_idx = (int)(intptr_t)io_uring_cqe_get_data(cqe);
        io_uring_cqe_seen(&sched->ring, cqe);
        if (buf_idx >= 0 && buf_idx < TV_BUF_COUNT && sched->sbuf[buf_idx].in_flight) {
            sched->sbuf[buf_idx].cqes_pending--;
            if (sched->sbuf[buf_idx].cqes_pending <= 0) {
                sched->sbuf[buf_idx].in_flight = 0;
                sched->inflight--;
            }
        }
    }
}

int tv_write(TV_SCHED *sched, const void *buf, uint64_t len) {
    if (!sched || !buf || len == 0) return -1;

    const uint8_t *src = (const uint8_t *)buf;
    uint64_t pos = 0;

    while (pos < len) {
        if (g_shutdown_requested) return -1;
        uint64_t space = sched->stripe_size - sched->sbuf_used;

        if (space == 0) {
            int head = sched->sbuf_head;
            TV_STRIPE_BUF *cur = &sched->sbuf[head];
            int nsub = flush_submit_io(sched, sched->sbuf_logical, cur->data, sched->sbuf_used, head);
            if (nsub < 0) return -1;
            cur->in_flight = 1;
            cur->cqes_pending = nsub;
            sched->inflight++;

            sched->sbuf_head = (sched->sbuf_head + 1) % TV_BUF_COUNT;
            sched->sbuf_logical += sched->stripe_size;
            sched->sbuf_used = 0;
            space = sched->stripe_size;

            /* All buffers occupied — wait for one to fully complete */
            if (sched->inflight >= TV_BUF_COUNT) {
                while (sched->inflight >= TV_BUF_COUNT) {
                    if (g_shutdown_requested) return -1;
                    reap_completed(sched);
                    if (sched->inflight >= TV_BUF_COUNT) {
                        struct io_uring_cqe *cqe = NULL;
        struct __kernel_timespec ts = { .tv_sec = 30, .tv_nsec = 0 };
        int r = io_uring_wait_cqe_timeout(&sched->ring, &cqe, &ts);
                        if (r == -ETIME) {
                            fprintf(stderr, "tv_write: CQE wait timed out (30s)\n");
                            return -1;
                        }
                        if (r == -EINTR) {
                            if (g_shutdown_requested) return -1;
                            continue;
                        }
                        if (r < 0) {
                            fprintf(stderr, "tv_write: wait_cqe failed: %s\n", strerror(-r));
                            return -1;
                        }
                        int buf_idx = (int)(intptr_t)io_uring_cqe_get_data(cqe);
                        if (cqe->res < 0)
                            fprintf(stderr, "tv_write: I/O error res=%d\n", cqe->res);
                        io_uring_cqe_seen(&sched->ring, cqe);
                        if (buf_idx >= 0 && buf_idx < TV_BUF_COUNT && sched->sbuf[buf_idx].in_flight) {
                            sched->sbuf[buf_idx].cqes_pending--;
                            if (sched->sbuf[buf_idx].cqes_pending <= 0) {
                                sched->sbuf[buf_idx].in_flight = 0;
                                sched->inflight--;
                            }
                        }
                    }
                }
            }
        }

        uint64_t chunk = (len - pos < space) ? (len - pos) : space;
        TV_STRIPE_BUF *cur = &sched->sbuf[sched->sbuf_head];
        memcpy(cur->data + sched->sbuf_used, src + pos, (size_t)chunk);
        sched->sbuf_used += chunk;
        pos += chunk;
    }

    return 0;
}

static int flush_submit_io(TV_SCHED *sched, uint64_t logical, uint8_t *data, uint64_t data_len, int buf_idx) {
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
    void *ud = (void *)(intptr_t)buf_idx;

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
                           (size_t)write_bytes, (off_t)disk_off, ud) < 0) {
            fprintf(stderr, "tv_flush: SQE allocation failed for disk %u\n",
                    seg->disk_index[i]);
            return -1;
        }
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

/* Explicit flush: submit current buffer if non-empty, then wait for ALL in-flight */
int tv_flush(TV_SCHED *sched) {
    if (!sched) return -1;

    /* Submit current buffer if it has data */
    if (sched->sbuf_used > 0) {
        int head = sched->sbuf_head;
        TV_STRIPE_BUF *cur = &sched->sbuf[head];
        int nsub = flush_submit_io(sched, sched->sbuf_logical, cur->data, sched->sbuf_used, head);
        if (nsub < 0) return -1;
        cur->in_flight = 1;
        cur->cqes_pending = nsub;
        sched->inflight++;
        sched->sbuf_head = (sched->sbuf_head + 1) % TV_BUF_COUNT;
        sched->sbuf_logical += sched->sbuf_used;
        sched->sbuf_used = 0;
    }

    /* Wait for ALL in-flight I/Os */
    while (sched->inflight > 0) {
        if (g_shutdown_requested) return -1;
        struct io_uring_cqe *cqe = NULL;
        struct __kernel_timespec ts = { .tv_sec = 30, .tv_nsec = 0 };
        int ret = io_uring_wait_cqe_timeout(&sched->ring, &cqe, &ts);
        if (ret == -ETIME) {
            fprintf(stderr, "tv_flush: CQE wait timed out (30s)\n");
            return -1;
        }
        if (ret == -EINTR) {
            if (g_shutdown_requested) return -1;
            continue;
        }
        if (ret < 0) {
            fprintf(stderr, "tv_flush: wait_cqe failed: %s\n", strerror(-ret));
            return -1;
        }
        int buf_idx = (int)(intptr_t)io_uring_cqe_get_data(cqe);
        int res = cqe->res;
        io_uring_cqe_seen(&sched->ring, cqe);
        if (buf_idx >= 0 && buf_idx < TV_BUF_COUNT && sched->sbuf[buf_idx].in_flight) {
            sched->sbuf[buf_idx].cqes_pending--;
            if (sched->sbuf[buf_idx].cqes_pending <= 0) {
                sched->sbuf[buf_idx].in_flight = 0;
                sched->inflight--;
            }
        } else {
            fprintf(stderr, "tv_flush: CQE for buf[%d] not matched (res=%d)\n", buf_idx, res);
        }
    }

    return 0;
}

int tv_read(TV_SCHED *sched, void *buf, uint64_t len, uint64_t offset) {
    if (!sched || !buf || len == 0) return -1;

    /* Flush any pending writes first */
    if (tv_flush(sched) < 0) return -1;

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
    tv_flush(sched);
    /* Drain any leftover CQEs before exiting the ring */
    {
        struct io_uring_cqe *cqe;
        while (io_uring_peek_cqe(&sched->ring, &cqe) == 0 && cqe) {
            io_uring_cqe_seen(&sched->ring, cqe);
        }
    }
    for (int i = 0; i < sched->ndisks; i++) {
        if (sched->disks[i].fd >= 0)
            fsync(sched->disks[i].fd);
    }
    for (int i = 0; i < TV_BUF_COUNT; i++) {
        free(sched->sbuf[i].data);
    }
    tv_uring_destroy(&sched->ring);
    free(sched);
}
