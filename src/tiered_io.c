#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include "tiered_sched.h"
#include "io_bench.h"

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
        "  --bench-all               Run full write benchmark suite (512MB/5120MB/10240MB)\n"
        "  --bench-read --size <N>MB Read benchmark (writes then reads back)\n"
        "  --bench-read-all           Run full read benchmark suite (512MB/5120MB/10240MB)\n"
        "  --direct                  Use O_DIRECT (default for --bench)\n"
        "  --no-direct               Disable O_DIRECT for benchmark\n"
        "  --warmup                  SLC cache warm-up (sustained speed)\n"
        "  --raw                     Write directly to block device (bypass filesystem)\n"
        "\n"
        "Examples:\n"
        "  # Scheduler (weighted striping)\n"
        "  tiered_io --name fastpool --bench --size 128MB\n"
        "  tiered_io --name fastpool --bench-all\n"
        "  tiered_io --name fastpool --bench-read --size 128MB\n"
        "  tiered_io --name fastpool --bench-read-all\n"
        "\n"
        "  # Direct path (LVM/filesystem, fair comparison)\n"
        "  tiered_io --path /mnt/test --bench --size 128MB\n"
        "  tiered_io --path /mnt/test --bench-all\n"
        "\n"
        "  # Raw device (bypass filesystem, fair comparison with scheduler)\n"
        "  tiered_io --path /dev/mapper/tv_vg_testpool2-tv_lv_testpool2 --bench-all --raw\n"
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

    if (tv_sched_seek(sched, offset) < 0) {
        fprintf(stderr, "Error: tv_sched_seek failed\n");
        free(buf);
        return -1;
    }

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

int main(int argc, char *argv[]) {
    signal(SIGTERM, sigterm_handler);
    signal(SIGINT, sigterm_handler);

    const char *name = NULL;
    const char *path = NULL;
    int do_info = 0, do_read = 0, do_write = 0, do_bench = 0, do_bench_all = 0;
    int do_bench_read = 0, do_bench_read_all = 0;
    int use_direct = -1, do_warmup = 0, do_raw = 0;
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
        } else if (strcmp(argv[i], "--bench-read-all") == 0) {
            do_bench_read_all = 1;
        } else if (strcmp(argv[i], "--bench-read") == 0) {
            do_bench_read = 1;
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
        } else if (strcmp(argv[i], "--raw") == 0) {
            do_raw = 1;
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
    if (use_direct < 0) use_direct = (do_bench || do_bench_read) ? 1 : 0;

    /* Direct path mode */
    if (path) {
        if (!do_bench && !do_bench_all) {
            fprintf(stderr, "Error: --path requires --bench or --bench-all\n");
            usage();
            return 1;
        }
        if (do_bench_all) use_direct = 1;  /* --bench-all always uses O_DIRECT */
        if (do_bench_all) {
            return cmd_bench_path_all(path, use_direct, do_raw);
        }
        return cmd_bench_path(path, bench_size, do_warmup, use_direct, do_raw);
    }

    /* Scheduler mode */
    if (!name) {
        usage();
        return 1;
    }

    if (!do_info && !do_read && !do_write && !do_bench && !do_bench_all
        && !do_bench_read && !do_bench_read_all) {
        fprintf(stderr, "Error: specify --info, --read, --write, --bench, --bench-all, --bench-read, or --bench-read-all\n");
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
    if (do_bench_read_all) {
        ret = cmd_bench_read_all(&meta);
    } else if (do_bench_all) {
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

        if (do_bench_read) {
            discard_disks(disks, (int)meta.disk_count);
            ret = cmd_bench_read_one(sched, bench_size, &meta);
        } else if (do_read) {
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
