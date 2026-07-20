#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include "tiered_sched.h"

static void sigterm_handler(int sig) {
    (void)sig;
    g_shutdown_requested = 1;
}

static void usage(void) {
    fprintf(stderr,
        "Usage:\n"
        "  tiered_io --name <vol> [options]    # Scheduler mode\n"
        "  tiered_io --path <dir> [options]    # Direct path mode (for LVM/filesystem)\n"
        "\n"
        "Scheduler mode options:\n"
        "  --info                    Show metadata and segment info\n"
        "  --read  --offset <N> --len <N>   Read bytes (output to stdout)\n"
        "  --write --offset <N> --len <N>   Write bytes (input from stdin)\n"
        "\n"
        "Benchmark options (both modes):\n"
        "  --bench --size <N>MB      Write benchmark (default: 64MB)\n"
        "  --bench --size <N>GB      Write benchmark in GB\n"
        "  --bench-all               Run full benchmark suite (512MB/5120MB/10240MB)\n"
        "  --direct                  Use O_DIRECT (default for --bench)\n"
        "  --no-direct               Disable O_DIRECT for benchmark\n"
        "  --warmup                  SLC cache warm-up (sustained speed)\n"
        "\n"
        "Examples:\n"
        "  # Scheduler (weighted striping)\n"
        "  tiered_io --name fastpool --bench --size 128MB\n"
        "  tiered_io --name fastpool --bench-all\n"
        "\n"
        "  # Direct path (LVM/filesystem, fair comparison)\n"
        "  tiered_io --path /mnt/test --bench --size 128MB\n"
        "  tiered_io --path /mnt/test --bench-all\n"
    );
}

static uint64_t parse_size(const char *s) {
    char *end;
    uint64_t val = strtoull(s, &end, 10);
    if (*end == 'G' || *end == 'g') val *= 1024ULL * 1024 * 1024;
    else if (*end == 'M' || *end == 'm') val *= 1024ULL * 1024;
    else if (*end == 'K' || *end == 'k') val *= 1024ULL;
    return val;
}

static int open_disks(TV_METADATA *meta, TV_DISK *disks, int use_direct) {
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
            return -1;
        }
        fprintf(stderr, "Opened %s (fd=%d)%s\n", devpath, disks[i].fd,
                use_direct ? " [O_DIRECT]" : "");
    }
    return 0;
}

static void close_disks(TV_DISK *disks, int ndisks) {
    for (int i = 0; i < ndisks; i++) {
        if (disks[i].fd >= 0) close(disks[i].fd);
    }
}

#include <sys/ioctl.h>
#include <linux/fs.h>

static int discard_disks(TV_DISK *disks, int ndisks) {
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

static int cmd_info(TV_METADATA *meta) {
    printf("Metadata: v%u, chunk=%uKB, %u disks, %u segments\n",
           meta->version, meta->chunk_size / 1024,
           meta->disk_count, meta->segment_count);

    for (uint32_t i = 0; i < meta->disk_count; i++) {
        printf("  Disk[%u] %s\n", i, meta->disk_names[i]);
    }

    for (uint32_t i = 0; i < meta->segment_count; i++) {
        TV_SEGMENT *seg = &meta->segments[i];
        printf("  Segment[%u]: %lu - %lu (%u disks, stripe=%luKB)\n",
               i,
               (unsigned long)seg->logical_begin,
               (unsigned long)seg->logical_end,
               seg->disk_count,
               (unsigned long)(seg->stripe_size / 1024));
        for (uint32_t j = 0; j < seg->disk_count; j++) {
            printf("    disk[%u] weight=%u chunk=%luKB\n",
                   seg->disk_index[j], seg->weight[j],
                   (unsigned long)(seg->weight[j] * TV_CHUNK_SIZE / 1024));
        }
    }
    return 0;
}

static int cmd_read(TV_SCHED *sched, uint64_t offset, uint64_t len) {
    uint8_t *buf = NULL;
    if (posix_memalign((void **)&buf, 512, (size_t)len) != 0) {
        fprintf(stderr, "Error: cannot allocate %lu bytes\n", (unsigned long)len);
        return -1;
    }

    fprintf(stderr, "Reading %lu bytes from offset %lu...\n",
            (unsigned long)len, (unsigned long)offset);

    int ret = tv_read(sched, buf, len, offset);
    if (ret < 0) {
        fprintf(stderr, "Error: tv_read failed\n");
        free(buf);
        return -1;
    }

    /* Write to stdout */
    uint64_t written = 0;
    while (written < len) {
        ssize_t n = write(STDOUT_FILENO, buf + written, (size_t)(len - written));
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "Error: write to stdout failed: %s\n", strerror(errno));
            free(buf);
            return -1;
        }
        written += n;
    }

    fprintf(stderr, "Read complete (%lu bytes)\n", (unsigned long)len);
    free(buf);
    return 0;
}

static int cmd_write(TV_SCHED *sched, uint64_t offset, uint64_t len) {
    uint8_t *buf = malloc((size_t)len);
    if (!buf) {
        fprintf(stderr, "Error: cannot allocate %lu bytes\n", (unsigned long)len);
        return -1;
    }

    /* Read from stdin */
    uint64_t total = 0;
    while (total < len) {
        ssize_t n = read(STDIN_FILENO, buf + total, (size_t)(len - total));
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "Error: read from stdin failed: %s\n", strerror(errno));
            free(buf);
            return -1;
        }
        if (n == 0) break; /* EOF */
        total += n;
    }

    fprintf(stderr, "Writing %lu bytes to offset %lu...\n",
            (unsigned long)total, (unsigned long)offset);

    int ret = tv_write(sched, buf, total);
    if (ret < 0) {
        fprintf(stderr, "Error: tv_write failed\n");
        free(buf);
        return -1;
    }

    ret = tv_flush(sched);
    if (ret < 0) {
        fprintf(stderr, "Error: tv_flush failed\n");
        free(buf);
        return -1;
    }

    fprintf(stderr, "Write complete (%lu bytes)\n", (unsigned long)total);
    free(buf);
    return 0;
}

static int do_warmup_exec(TV_SCHED *sched, TV_METADATA *meta) {
    uint64_t vol_size = meta->segments[0].logical_end - meta->segments[0].logical_begin;
    uint64_t warmup_size = vol_size * 2 / 10;
    if (warmup_size > 4ULL * 1024 * 1024 * 1024)
        warmup_size = 4ULL * 1024 * 1024 * 1024;
    uint8_t *wbuf = malloc((size_t)sched->stripe_size);
    if (!wbuf) return -1;
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

static int cmd_bench_one(TV_SCHED *sched, uint64_t size, int warmup, TV_METADATA *meta) {
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

    uint8_t *buf = malloc((size_t)(sched->stripe_size));
    if (!buf) {
        fprintf(stderr, "Error: cannot allocate buffer\n");
        return -1;
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
            return -1;
        }

        written += chunk;

        if (written % (64 * 1024 * 1024) == 0 || written == size) {
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

static int cmd_bench_all(TV_METADATA *meta) {
    uint64_t sizes[] = {
        512ULL  * 1024 * 1024,
        5120ULL * 1024 * 1024,
        10240ULL * 1024 * 1024,
    };
    int nsizes = sizeof(sizes) / sizeof(sizes[0]);
    int ret = 0;

    TV_DISK disks[TV_MAX_DISKS];
    if (open_disks(meta, disks, 1) < 0) return -1;

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

/* Direct path benchmark: write to a file on a filesystem (LVM/ext4/etc.)
 * Uses same block size (256KB) and O_DIRECT as scheduler mode for fair comparison. */
static int cmd_bench_path(const char *path, uint64_t size, int warmup, int use_direct) {
    /* Create benchmark file */
    char benchfile[512];
    snprintf(benchfile, sizeof(benchfile), "%s/tieredvol_bench.dat", path);

    int flags = O_WRONLY | O_CREAT | O_TRUNC;
    if (use_direct) flags |= O_DIRECT;

    int fd = open(benchfile, flags, 0644);
    if (fd < 0) {
        fprintf(stderr, "Error: cannot open %s: %s\n", benchfile, strerror(errno));
        return -1;
    }

    fprintf(stderr, "Benchmark file: %s%s\n", benchfile,
            use_direct ? " [O_DIRECT]" : "");

    /* Use same block size as scheduler (256KB) */
    uint64_t chunk_size = TV_CHUNK_SIZE;

    uint8_t *buf = NULL;
    if (posix_memalign((void **)&buf, 512, (size_t)chunk_size) != 0) {
        fprintf(stderr, "Error: cannot allocate buffer\n");
        close(fd);
        return -1;
    }
    memset(buf, 0xAB, (size_t)chunk_size);

    /* Warmup: fill SLC cache with 20% of size (capped at 4GB) */
    if (warmup) {
        uint64_t warmup_size = size * 2 / 10;
        warmup_size = (warmup_size / 512) * 512;  /* O_DIRECT alignment */
        if (warmup_size > 4ULL * 1024 * 1024 * 1024)
            warmup_size = 4ULL * 1024 * 1024 * 1024;
        fprintf(stderr, "Warming up SLC cache (%luMB)...\n",
                (unsigned long)(warmup_size / (1024 * 1024)));
        uint64_t warmup_written = 0;
        while (warmup_written < warmup_size) {
            if (g_shutdown_requested) break;
            uint64_t chunk = chunk_size;
            if (warmup_written + chunk > warmup_size) chunk = warmup_size - warmup_written;
            ssize_t n = pwrite(fd, buf, (size_t)chunk, (off_t)warmup_written);
            if (n < 0) {
                fprintf(stderr, "Error: warmup pwrite failed: %s\n", strerror(errno));
                break;
            }
            warmup_written += n;
        }
        fsync(fd);
        fprintf(stderr, "Warm-up complete.\n");
    }

    /* Benchmark */
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

        if (written % (64 * 1024 * 1024) == 0 || written == size) {
            fprintf(stderr, "\r  progress: %lu / %lu MB",
                    (unsigned long)(written / 1048576),
                    (unsigned long)(size / 1048576));
        }
    }
    fprintf(stderr, "\n");

    clock_gettime(CLOCK_MONOTONIC, &ts_end);

    fsync(fd);
    close(fd);

    /* Clean up benchmark file */
    unlink(benchfile);

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

static int cmd_bench_path_all(const char *path, int use_direct) {
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
            cmd_bench_path(path, sizes[i], phase == 1, use_direct);
        }
    }
    return 0;
}

int main(int argc, char *argv[]) {
    signal(SIGTERM, sigterm_handler);
    signal(SIGINT, sigterm_handler);

    const char *name = NULL;
    const char *path = NULL;
    int do_info = 0, do_read = 0, do_write = 0, do_bench = 0, do_bench_all = 0;
    int use_direct = -1, do_warmup = 0;
    uint64_t offset = 0, len = 0, bench_size = 64 * 1024 * 1024;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            name = argv[++i];
        } else if (strcmp(argv[i], "--path") == 0 && i + 1 < argc) {
            path = argv[++i];
        } else if (strcmp(argv[i], "--info") == 0) {
            do_info = 1;
        } else if (strcmp(argv[i], "--read") == 0) {
            do_read = 1;
        } else if (strcmp(argv[i], "--write") == 0) {
            do_write = 1;
        } else if (strcmp(argv[i], "--bench-all") == 0) {
            do_bench_all = 1;
        } else if (strcmp(argv[i], "--bench") == 0) {
            do_bench = 1;
        } else if (strcmp(argv[i], "--direct") == 0) {
            use_direct = 1;
        } else if (strcmp(argv[i], "--no-direct") == 0) {
            use_direct = 0;
        } else if (strcmp(argv[i], "--warmup") == 0) {
            do_warmup = 1;
        } else if (strcmp(argv[i], "--offset") == 0 && i + 1 < argc) {
            offset = strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--len") == 0 && i + 1 < argc) {
            len = strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            bench_size = parse_size(argv[++i]);
        } else {
            usage();
            return 1;
        }
    }

    /* --bench defaults to O_DIRECT unless --no-direct is specified */
    if (use_direct < 0) use_direct = do_bench ? 1 : 0;

    /* Direct path mode */
    if (path) {
        if (!do_bench && !do_bench_all) {
            fprintf(stderr, "Error: --path requires --bench or --bench-all\n");
            usage();
            return 1;
        }
        if (do_bench_all) use_direct = 1;  /* --bench-all always uses O_DIRECT */
        if (do_bench_all) {
            return cmd_bench_path_all(path, use_direct);
        }
        return cmd_bench_path(path, bench_size, do_warmup, use_direct);
    }

    /* Scheduler mode */
    if (!name) {
        usage();
        return 1;
    }

    if (!do_info && !do_read && !do_write && !do_bench && !do_bench_all) {
        fprintf(stderr, "Error: specify --info, --read, --write, --bench, or --bench-all\n");
        usage();
        return 1;
    }

    if (do_bench_all) {
        do_bench = 1;
        use_direct = 1;
    }

    /* Load metadata */
    TV_METADATA meta;
    char config_path[256];
    snprintf(config_path, sizeof(config_path), "/etc/tieredvol/%s.scheduler", name);

    fprintf(stderr, "Loading metadata from %s...\n", config_path);
    if (tv_metadata_load(&meta, config_path) < 0) {
        return 1;
    }

    if (do_info) {
        return cmd_info(&meta);
    }

    int ret = 0;
    if (do_bench_all) {
        /* bench-all manages its own scheduler lifecycle */
        ret = cmd_bench_all(&meta);
    } else {
        /* Open disks */
        TV_DISK disks[TV_MAX_DISKS];
        if (open_disks(&meta, disks, use_direct) < 0) {
            return 1;
        }

        /* Init scheduler */
        fprintf(stderr, "Initializing scheduler...\n");
        TV_SCHED *sched = tv_sched_init(disks, (int)meta.disk_count, &meta);
        if (!sched) {
            fprintf(stderr, "Error: tv_sched_init failed\n");
            close_disks(disks, (int)meta.disk_count);
            return 1;
        }

        if (do_read) {
            if (len == 0) {
                fprintf(stderr, "Error: --read requires --len\n");
                ret = 1;
            } else {
                ret = cmd_read(sched, offset, len);
            }
        } else if (do_write) {
            if (len == 0) {
                fprintf(stderr, "Error: --write requires --len\n");
                ret = 1;
            } else {
                ret = cmd_write(sched, offset, len);
            }
        } else if (do_bench) {
            discard_disks(disks, (int)meta.disk_count);
            /* Single bench with optional warmup */
            if (do_warmup) {
                do_warmup_exec(sched, &meta);
            }
            ret = cmd_bench_one(sched, bench_size, 0, &meta);
        }

        tv_sched_destroy(sched);
        close_disks(disks, (int)meta.disk_count);
    }

    return ret;
}
