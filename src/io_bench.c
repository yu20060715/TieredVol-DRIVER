#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include "tiered_sched.h"
#include "io_bench.h"
#include "warmup.h"

int open_disks(TV_METADATA *meta, TV_DISK *disks, int use_direct) {
    int flags = O_RDWR | (use_direct ? O_DIRECT : 0);
    for (uint32_t i = 0; i < meta->disk_count; i++) {
        memset(&disks[i], 0, sizeof(TV_DISK));
        disks[i].id = (int)i;
        strncpy(disks[i].name, meta->disk_names[i], 63);

        char devpath[128];
        snprintf(devpath, sizeof(devpath), "/dev/mapper/tv_%s_carve",
                 meta->disk_names[i]);
        disks[i].fd = open(devpath, flags);
        if (disks[i].fd < 0) {
            fprintf(stderr, "Error: cannot open %s: %s\n",
                    devpath, strerror(errno));
            for (int j = 0; j < (int)i; j++) close(disks[j].fd);
            return TV_ERR;
        }
        fprintf(stderr, "Opened %s (fd=%d)%s\n", devpath, disks[i].fd,
                use_direct ? " [O_DIRECT]" : "");
    }
    return 0;
}

void close_disks(TV_DISK *disks, int ndisks) {
    for (int i = 0; i < ndisks; i++) {
        if (disks[i].fd >= 0) close(disks[i].fd);
    }
}

int discard_disks(TV_DISK *disks, int ndisks) {
    for (int i = 0; i < ndisks; i++) {
        if (disks[i].fd < 0) continue;
        off_t sz = lseek(disks[i].fd, 0, SEEK_END);
        if (sz <= 0) continue;
        uint64_t range[2] = { 0, (uint64_t)sz };
        if (ioctl(disks[i].fd, BLKDISCARD, range) < 0) {
            fprintf(stderr, "Warning: BLKDISCARD on %s failed: %s\n",
                    disks[i].name, strerror(errno));
        }
    }
    return 0;
}

int do_warmup_exec(TV_SCHED *sched, TV_METADATA *meta) {
    uint64_t vol_size = meta->segments[0].logical_end - meta->segments[0].logical_begin;
    uint64_t warmup_size = vol_size * 2 / 10;
    if (warmup_size > 4ULL * 1024 * 1024 * 1024)
        warmup_size = 4ULL * 1024 * 1024 * 1024;
    uint8_t *wbuf = malloc((size_t)sched->stripe_size);
    if (!wbuf) return TV_ERR;
    memset(wbuf, 0xAB, (size_t)sched->stripe_size);
    fprintf(stderr, "Warming up SLC cache (%luMB, 20%% of volume)...\n",
            (unsigned long)(warmup_size / (1024 * 1024)));
    uint64_t warmup_written = 0;
    int warmup_ok = 1;
    while (warmup_written < warmup_size) {
        if (g_shutdown_requested) { warmup_ok = 0; break; }
        uint64_t chunk = sched->stripe_size;
        if (warmup_written + chunk > warmup_size) chunk = warmup_size - warmup_written;
        if (tv_write(sched, wbuf, chunk) < 0) { warmup_ok = 0; break; }
        warmup_written += chunk;
    }
    if (warmup_ok && sched->sbuf_used > 0) {
        if (tv_flush(sched) < 0) warmup_ok = 0;
    }
    if (warmup_ok) {
        for (int i = 0; i < sched->ndisks; i++) {
            if (sched->disks[i].fd >= 0) fsync(sched->disks[i].fd);
        }
    }
    free(wbuf);
    if (warmup_ok)
        fprintf(stderr, "Warm-up complete.\n");
    else
        fprintf(stderr, "WARNING: warm-up failed\n");
    return warmup_ok ? 0 : -1;
}

int cmd_bench_one(TV_SCHED *sched, uint64_t size, int warmup, TV_METADATA *meta) {
    if (warmup) {
        do_warmup_exec(sched, meta);
    }

    uint64_t vol_size = meta->segments[meta->segment_count - 1].logical_end;
    uint64_t remaining = vol_size - sched->sbuf_logical;
    if (size > remaining) {
        fprintf(stderr, "Warning: bench size (%lu MB) exceeds remaining volume (%lu MB), capping\n",
                (unsigned long)(size / 1048576), (unsigned long)(remaining / 1048576));
        size = remaining;
    }
    if (size == 0) {
        fprintf(stderr, "Warning: no space left in volume, skipping bench\n");
        return 0;
    }

    uint8_t *buf = NULL;
    if (posix_memalign((void **)&buf, 4096, (size_t)(sched->stripe_size)) != 0) {
        fprintf(stderr, "Error: cannot allocate buffer\n");
        return TV_ERR;
    }
    memset(buf, 0xAB, (size_t)sched->stripe_size);

    uint64_t written = 0;

    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    while (written < size) {
        if (g_shutdown_requested) {
            fprintf(stderr, "\nShutdown requested, flushing...\n");
            break;
        }
        uint64_t chunk = sched->stripe_size;
        if (written + chunk > size) chunk = size - written;

        int ret = tv_write(sched, buf, chunk);
        if (ret < 0) {
            if (g_shutdown_requested) {
                fprintf(stderr, "\nShutdown requested during write\n");
                break;
            }
            fprintf(stderr, "Error: tv_write failed at %lu bytes\n",
                    (unsigned long)written);
            free(buf);
            return TV_ERR;
        }

        written += chunk;

        if (written % TV_PROGRESS_INTERVAL == 0 || written == size) {
            fprintf(stderr, "\r  progress: %lu / %lu MB",
                    (unsigned long)(written / 1048576),
                    (unsigned long)(size / 1048576));
        }
    }
    fprintf(stderr, "\n");

    clock_gettime(CLOCK_MONOTONIC, &ts_end);

    {
        int ret = tv_flush(sched);
        if (ret < 0) {
            fprintf(stderr, "Warning: final tv_flush failed (some CQEs may be stuck)\n");
        }
    }

    for (int i = 0; i < sched->ndisks; i++) {
        if (sched->disks[i].fd >= 0)
            fsync(sched->disks[i].fd);
    }
    double elapsed = (ts_end.tv_sec - ts_start.tv_sec) +
                     (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;

    double mb = (double)written / (1024.0 * 1024.0);
    double mbps = mb / elapsed;
    int stripe_count = (int)(written / sched->stripe_size);
    if (written % sched->stripe_size) stripe_count++;

    fprintf(stderr, "Benchmark: %lu bytes (%.1f MB) in %.3f seconds\n",
           (unsigned long)written, mb, elapsed);
    fprintf(stderr, "Throughput: %.1f MB/s\n", mbps);
    fprintf(stderr, "Stripes flushed: %d\n", stripe_count);

    free(buf);
    return 0;
}

int cmd_bench_all(TV_METADATA *meta) {
    uint64_t sizes[] = {
        512ULL  * 1024 * 1024,
        5120ULL * 1024 * 1024,
        10240ULL * 1024 * 1024,
    };
    int nsizes = sizeof(sizes) / sizeof(sizes[0]);
    int ret = 0;

    TV_DISK disks[TV_MAX_DISKS];
    if (open_disks(meta, disks, 1) < 0) return TV_ERR;

    for (int phase = 0; phase < 2; phase++) {
        for (int i = 0; i < nsizes; i++) {
            if (g_shutdown_requested) { ret = -1; break; }
            fprintf(stderr, "\n=== Bench %luMB (%s) ===\n",
                    (unsigned long)(sizes[i] / 1048576),
                    phase == 0 ? "no warmup" : "with warmup");

            /* Discard blocks to avoid io_uring CQE loss when
             * overwriting previously io_uring-written dm-linear blocks. */
            discard_disks(disks, (int)meta->disk_count);

            /* Fresh scheduler per bench: avoids kernel I/O hang when
             * io_uring reuses memory buffers for writes to already-
             * written dm-linear blocks (O_DIRECT + dm-linear bug). */
            TV_SCHED *sched = tv_sched_init(disks, (int)meta->disk_count, meta);
            if (!sched) {
                fprintf(stderr, "Error: tv_sched_init failed\n");
                ret = -1;
                break;
            }

            if (cmd_bench_one(sched, sizes[i], phase == 1, meta) < 0) {
                tv_sched_destroy(sched);
                fprintf(stderr, "Warning: bench failed, continuing to next size\n");
                continue;
            }
            tv_sched_destroy(sched);
        }
        if (ret < 0) break;
    }

    close_disks(disks, (int)meta->disk_count);
    return ret;
}

int cmd_bench_read_one(TV_SCHED *sched, uint64_t size, TV_METADATA *meta) {
    uint64_t vol_size = meta->segments[meta->segment_count - 1].logical_end;
    uint64_t remaining = vol_size - sched->sbuf_logical;
    if (size > remaining) {
        fprintf(stderr, "Warning: read bench size (%lu MB) exceeds remaining volume (%lu MB), capping\n",
                (unsigned long)(size / 1048576), (unsigned long)(remaining / 1048576));
        size = remaining;
    }
    if (size == 0) {
        fprintf(stderr, "Warning: no space left in volume, skipping read bench\n");
        return 0;
    }

    uint8_t *buf = NULL;
    if (posix_memalign((void **)&buf, 512, (size_t)sched->stripe_size) != 0) {
        fprintf(stderr, "Error: cannot allocate buffer\n");
        return TV_ERR;
    }
    memset(buf, 0xAB, (size_t)sched->stripe_size);

    /* First, write the data so there is something to read */
    fprintf(stderr, "  Writing %lu MB to prepare for read...\n",
            (unsigned long)(size / 1048576));

    uint64_t written = 0;
    while (written < size) {
        if (g_shutdown_requested) {
            fprintf(stderr, "\nShutdown requested\n");
            free(buf);
            return TV_ERR;
        }
        uint64_t chunk = sched->stripe_size;
        if (written + chunk > size) chunk = size - written;
        if (tv_write(sched, buf, chunk) < 0) {
            fprintf(stderr, "Error: write failed during read bench prep\n");
            free(buf);
            return TV_ERR;
        }
        written += chunk;
    }
    if (tv_flush(sched) < 0) {
        fprintf(stderr, "Warning: flush failed during read prep\n");
    }
    for (int i = 0; i < sched->ndisks; i++) {
        if (sched->disks[i].fd >= 0) fsync(sched->disks[i].fd);
    }

    /* Flush page cache on block devices */
    for (int i = 0; i < sched->ndisks; i++) {
        if (sched->disks[i].fd >= 0) {
            int dcfd = open("/proc/sys/vm/drop_caches", O_WRONLY);
            if (dcfd >= 0) { (void)!write(dcfd, "3", 1); close(dcfd); }
            break;
        }
    }

    /* Seek back to 0 and read */
    if (tv_sched_seek(sched, 0) < 0) {
        fprintf(stderr, "Error: seek failed for read bench\n");
        free(buf);
        return TV_ERR;
    }

    fprintf(stderr, "  Reading %lu MB...\n", (unsigned long)(size / 1048576));

    uint64_t read_total = 0;
    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    while (read_total < size) {
        if (g_shutdown_requested) {
            fprintf(stderr, "\nShutdown requested during read\n");
            break;
        }
        uint64_t chunk = sched->stripe_size;
        if (read_total + chunk > size) chunk = size - read_total;

        int ret = tv_read(sched, buf, chunk, read_total);
        if (ret < 0) {
            fprintf(stderr, "Error: tv_read failed at %lu bytes\n",
                    (unsigned long)read_total);
            free(buf);
            return TV_ERR;
        }
        read_total += chunk;

        if (read_total % TV_PROGRESS_INTERVAL == 0 || read_total == size) {
            fprintf(stderr, "\r  progress: %lu / %lu MB",
                    (unsigned long)(read_total / 1048576),
                    (unsigned long)(size / 1048576));
        }
    }
    fprintf(stderr, "\n");

    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    double elapsed = (ts_end.tv_sec - ts_start.tv_sec) +
                     (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;
    double mb = (double)read_total / (1024.0 * 1024.0);
    double mbps = mb / elapsed;

    fprintf(stderr, "Read benchmark: %lu bytes (%.1f MB) in %.3f seconds\n",
            (unsigned long)read_total, mb, elapsed);
    fprintf(stderr, "Read throughput: %.1f MB/s\n", mbps);

    free(buf);
    return 0;
}

int cmd_bench_read_all(TV_METADATA *meta) {
    uint64_t sizes[] = {
        512ULL  * 1024 * 1024,
        5120ULL * 1024 * 1024,
        10240ULL * 1024 * 1024,
    };
    int nsizes = sizeof(sizes) / sizeof(sizes[0]);
    int ret = 0;

    TV_DISK disks[TV_MAX_DISKS];
    if (open_disks(meta, disks, 1) < 0) return TV_ERR;

    for (int i = 0; i < nsizes; i++) {
        if (g_shutdown_requested) { ret = -1; break; }
        fprintf(stderr, "\n=== Read Bench %luMB ===\n",
                (unsigned long)(sizes[i] / 1048576));

        discard_disks(disks, (int)meta->disk_count);

        TV_SCHED *sched = tv_sched_init(disks, (int)meta->disk_count, meta);
        if (!sched) {
            fprintf(stderr, "Error: tv_sched_init failed\n");
            ret = -1;
            break;
        }

        if (cmd_bench_read_one(sched, sizes[i], meta) < 0) {
            fprintf(stderr, "Warning: read bench failed, continuing\n");
        }
        tv_sched_destroy(sched);
    }

    close_disks(disks, (int)meta->disk_count);
    return ret;
}

int cmd_bench_path(const char *path, uint64_t size, int warmup, int use_direct, int raw) {
    int fd;
    int is_file = !raw;

    if (raw) {
        fd = open(path, O_RDWR | O_DIRECT);
        if (fd < 0) {
            fprintf(stderr, "Error: cannot open %s: %s\n", path, strerror(errno));
            return TV_ERR;
        }
        fprintf(stderr, "Raw device: %s [O_DIRECT]\n", path);
    } else {
        char benchfile[512];
        snprintf(benchfile, sizeof(benchfile), "%s/tieredvol_bench.dat", path);

        int flags = O_WRONLY | O_CREAT | O_TRUNC;
        if (use_direct) flags |= O_DIRECT;

        fd = open(benchfile, flags, 0644);
        if (fd < 0) {
            fprintf(stderr, "Error: cannot open %s: %s\n", benchfile, strerror(errno));
            return TV_ERR;
        }
        fprintf(stderr, "Benchmark file: %s%s\n", benchfile,
                use_direct ? " [O_DIRECT]" : "");
    }

    uint64_t chunk_size = TV_CHUNK_SIZE;

    uint8_t *buf = NULL;
    if (posix_memalign((void **)&buf, 512, (size_t)chunk_size) != 0) {
        fprintf(stderr, "Error: cannot allocate buffer\n");
        close(fd);
        return TV_ERR;
    }
    memset(buf, 0xAB, (size_t)chunk_size);

    if (warmup) {
        uint64_t warmup_size = size * 2 / 10;
        warmup_size = (warmup_size / 512) * 512;
        if (warmup_size > 4ULL * 1024 * 1024 * 1024)
            warmup_size = 4ULL * 1024 * 1024 * 1024;
        fprintf(stderr, "Warming up SLC cache (%luMB)...\n",
                (unsigned long)(warmup_size / (1024 * 1024)));
        tv_warmup_device(path, warmup_size);
        fsync(fd);
        fprintf(stderr, "Warm-up complete.\n");
    }

    uint64_t written = 0;
    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    while (written < size) {
        if (g_shutdown_requested) {
            fprintf(stderr, "\nShutdown requested\n");
            break;
        }
        uint64_t chunk = chunk_size;
        if (written + chunk > size) chunk = size - written;

        ssize_t n = pwrite(fd, buf, (size_t)chunk, (off_t)written);
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "Error: pwrite failed at %lu bytes: %s\n",
                    (unsigned long)written, strerror(errno));
            break;
        }
        written += n;

        if (written % TV_PROGRESS_INTERVAL == 0 || written == size) {
            fprintf(stderr, "\r  progress: %lu / %lu MB",
                    (unsigned long)(written / 1048576),
                    (unsigned long)(size / 1048576));
        }
    }
    fprintf(stderr, "\n");

    clock_gettime(CLOCK_MONOTONIC, &ts_end);

    fsync(fd);
    close(fd);

    if (is_file) {
        char benchfile[512];
        snprintf(benchfile, sizeof(benchfile), "%s/tieredvol_bench.dat", path);
        unlink(benchfile);
    }

    double elapsed = (ts_end.tv_sec - ts_start.tv_sec) +
                     (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;

    double mb = (double)written / (1024.0 * 1024.0);
    double mbps = mb / elapsed;

    fprintf(stderr, "Benchmark: %lu bytes (%.1f MB) in %.3f seconds\n",
           (unsigned long)written, mb, elapsed);
    fprintf(stderr, "Throughput: %.1f MB/s\n", mbps);

    free(buf);
    return 0;
}

int cmd_bench_path_all(const char *path, int use_direct, int raw) {
    uint64_t sizes[] = {
        512ULL  * 1024 * 1024,
        5120ULL * 1024 * 1024,
        10240ULL * 1024 * 1024,
    };
    int nsizes = sizeof(sizes) / sizeof(sizes[0]);

    for (int phase = 0; phase < 2; phase++) {
        for (int i = 0; i < nsizes; i++) {
            if (g_shutdown_requested) break;
            fprintf(stderr, "\n=== Bench %luMB (%s) ===\n",
                    (unsigned long)(sizes[i] / 1048576),
                    phase == 0 ? "no warmup" : "with warmup");
            cmd_bench_path(path, sizes[i], phase == 1, use_direct, raw);
        }
    }
    return 0;
}
