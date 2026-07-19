#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include "tiered_sched.h"

static void usage(void) {
    fprintf(stderr,
        "Usage: tiered_io --name <vol> [options]\n"
        "\n"
        "Options:\n"
        "  --info                    Show metadata and segment info\n"
        "  --read  --offset <N> --len <N>   Read bytes (output to stdout)\n"
        "  --write --offset <N> --len <N>   Write bytes (input from stdin)\n"
        "  --bench --size <N>MB      Write benchmark (default: 64MB)\n"
        "  --bench --size <N>GB      Write benchmark in GB\n"
        "  --direct                  Use O_DIRECT (bypass page cache, default for --bench)\n"
        "  --no-direct               Disable O_DIRECT for benchmark\n"
        "  --warmup                  SLC cache warm-up (sustained speed)\n"
        "\n"
        "Examples:\n"
        "  tiered_io --name fastpool --info\n"
        "  tiered_io --name fastpool --bench --size 128MB\n"
        "  tiered_io --name fastpool --bench --size 128MB --warmup\n"
        "  tiered_io --name fastpool --bench --size 1GB --direct --warmup\n"
        "  dd if=/dev/zero bs=1M count=10 | tiered_io --name fastpool --write --offset 0 --len 10485760\n"
        "  tiered_io --name fastpool --read --offset 0 --len 1024 | xxd\n"
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

static int cmd_bench(TV_SCHED *sched, uint64_t size) {
    uint64_t vol_size = sched->meta->segments[sched->meta->segment_count - 1].logical_end;
    uint64_t remaining = vol_size - sched->sbuf_logical;
    if (size > remaining) {
        fprintf(stderr, "Warning: bench size (%lu MB) exceeds remaining volume (%lu MB), capping\n",
                (unsigned long)(size / 1048576), (unsigned long)(remaining / 1048576));
        size = remaining;
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
        uint64_t chunk = sched->stripe_size;
        if (written + chunk > size) chunk = size - written;

        int ret = tv_write(sched, buf, chunk);
        if (ret < 0) {
            fprintf(stderr, "Error: tv_write failed at %lu bytes\n",
                    (unsigned long)written);
            free(buf);
            return -1;
        }

        written += chunk;
    }

    /* Flush remaining partial stripe and wait for all in-flight I/O */
    {
        int ret = tv_flush(sched);
        if (ret < 0) {
            fprintf(stderr, "Error: final tv_flush failed\n");
            free(buf);
            return -1;
        }
    }

    /* Sync all disks to get true physical throughput */
    for (int i = 0; i < sched->ndisks; i++) {
        if (sched->disks[i].fd >= 0)
            fsync(sched->disks[i].fd);
    }

    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    double elapsed = (ts_end.tv_sec - ts_start.tv_sec) +
                     (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;

    double mb = (double)written / (1024.0 * 1024.0);
    double mbps = mb / elapsed;
    int stripe_count = (int)(written / sched->stripe_size);
    if (written % sched->stripe_size) stripe_count++;

    printf("Benchmark: %lu bytes (%.1f MB) in %.3f seconds\n",
           (unsigned long)written, mb, elapsed);
    printf("Throughput: %.1f MB/s\n", mbps);
    printf("Stripes flushed: %d\n", stripe_count);

    free(buf);
    return 0;
}

int main(int argc, char *argv[]) {
    const char *name = NULL;
    int do_info = 0, do_read = 0, do_write = 0, do_bench = 0, use_direct = -1, do_warmup = 0;
    uint64_t offset = 0, len = 0, bench_size = 64 * 1024 * 1024;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            name = argv[++i];
        } else if (strcmp(argv[i], "--info") == 0) {
            do_info = 1;
        } else if (strcmp(argv[i], "--read") == 0) {
            do_read = 1;
        } else if (strcmp(argv[i], "--write") == 0) {
            do_write = 1;
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

    if (!name) {
        usage();
        return 1;
    }

    if (!do_info && !do_read && !do_write && !do_bench) {
        fprintf(stderr, "Error: specify --info, --read, --write, or --bench\n");
        usage();
        return 1;
    }

    /* --bench defaults to O_DIRECT unless --no-direct is specified */
    if (use_direct < 0) use_direct = do_bench ? 1 : 0;

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

    /* SLC cache warm-up: write through scheduler to exhaust SLC cache */
    if (do_warmup && do_bench) {
        uint64_t vol_size = meta.segments[0].logical_end - meta.segments[0].logical_begin;
        uint64_t warmup_size = vol_size * 2 / 10;  /* 20% of volume */
        if (warmup_size > 4ULL * 1024 * 1024 * 1024)
            warmup_size = 4ULL * 1024 * 1024 * 1024;  /* cap at 4GB */
        uint8_t *wbuf = malloc((size_t)sched->stripe_size);
        if (wbuf) {
            memset(wbuf, 0xAB, (size_t)sched->stripe_size);
            fprintf(stderr, "Warming up SLC cache (%luMB, 20%% of volume)...\n",
                    (unsigned long)(warmup_size / (1024 * 1024)));
            uint64_t warmup_written = 0;
            int warmup_ok = 1;
            while (warmup_written < warmup_size) {
                uint64_t chunk = sched->stripe_size;
                if (warmup_written + chunk > warmup_size) chunk = warmup_size - warmup_written;
                if (tv_write(sched, wbuf, chunk) < 0) { warmup_ok = 0; break; }
                warmup_written += chunk;
            }
            if (warmup_ok && sched->sbuf_used > 0) {
                if (tv_flush(sched) < 0) warmup_ok = 0;
            }
            /* Sync to ensure warmup data is on physical media */
            if (warmup_ok) {
                for (int i = 0; i < sched->ndisks; i++) {
                    if (sched->disks[i].fd >= 0) fsync(sched->disks[i].fd);
                }
            }
            free(wbuf);
            if (warmup_ok)
                fprintf(stderr, "Warm-up complete, starting benchmark...\n");
            else
                fprintf(stderr, "WARNING: warm-up failed, benchmark results may be inaccurate\n");
        }
    }

    int ret = 0;
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
        ret = cmd_bench(sched, bench_size);
    }

    /* Cleanup */
    tv_sched_destroy(sched);
    close_disks(disks, (int)meta.disk_count);

    return ret;
}
