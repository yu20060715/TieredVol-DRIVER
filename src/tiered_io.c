#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <time.h>
#include <liburing.h>
#include "tiered_types.h"
#include "version.h"

#define ALIGNMENT 4096
#define BS_DEFAULT (16 * 1024 * 1024)
#define BS_MIN     (4096)
#define BS_MAX     (64 * 1024 * 1024)

static volatile sig_atomic_t g_stop = 0;

static void sig_handler(int sig) { (void)sig; g_stop = 1; }

static uint64_t parse_size(const char *s) {
    char *end;
    uint64_t val = strtoull(s, &end, 10);
    if (*end == 'G' || *end == 'g') val *= 1024ULL * 1024 * 1024;
    else if (*end == 'M' || *end == 'm') val *= 1024ULL * 1024;
    else if (*end == 'K' || *end == 'k') val *= 1024ULL;
    return val;
}

static double elapsed_sec(struct timespec *a, struct timespec *b) {
    return (b->tv_sec - a->tv_sec) + (b->tv_nsec - a->tv_nsec) / 1e9;
}

static uint64_t get_device_size(int fd) {
    uint64_t bytes = 0;
    if (ioctl(fd, BLKGETSIZE64, &bytes) == 0) return bytes;
    struct stat st;
    if (fstat(fd, &st) == 0) return st.st_size;
    return 0;
}

static int bench_write(const char *path, uint64_t size, int use_direct, uint64_t bs) {
    int flags = O_WRONLY | O_CREAT | O_TRUNC;
    if (use_direct) flags |= O_DIRECT;

    int fd = open(path, flags, 0644);
    if (fd < 0) {
        fprintf(stderr, "Error: cannot open %s: %s\n", path, strerror(errno));
        return TV_ERR;
    }

    uint8_t *buf;
    if (posix_memalign((void **)&buf, ALIGNMENT, bs) != 0) {
        fprintf(stderr, "Error: alloc failed\n");
        close(fd);
        return TV_ERR;
    }
    memset(buf, 0xAB, bs);

    uint64_t written = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    while (written < size && !g_stop) {
        ssize_t n = pwrite(fd, buf, bs, (off_t)written);
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "Error: pwrite failed at %lu: %s\n",
                    (unsigned long)written, strerror(errno));
            break;
        }
        written += n;
    }

    fsync(fd);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    close(fd);
    free(buf);

    double sec = elapsed_sec(&t0, &t1);
    double mb = (double)written / (1024.0 * 1024.0);
    double mbps = mb / sec;

    printf("Throughput: %.1f MB/s  (%.1f GB in %.2f sec)\n", mbps, mb / 1024.0, sec);
    return 0;
}

static int bench_write_async(const char *path, uint64_t size, int use_direct, uint64_t bs) {
    int flags = O_WRONLY | O_CREAT | O_TRUNC;
    if (use_direct) flags |= O_DIRECT;

    int fd = open(path, flags, 0644);
    if (fd < 0) {
        fprintf(stderr, "Error: cannot open %s: %s\n", path, strerror(errno));
        return TV_ERR;
    }

    uint64_t count = size / bs;
    int queue_depth = 4;
    if (queue_depth > (int)count) queue_depth = (int)count;

    uint8_t **bufs = calloc(count, sizeof(uint8_t *));
    for (uint64_t i = 0; i < count; i++) {
        if (posix_memalign((void **)&bufs[i], ALIGNMENT, bs) != 0) {
            fprintf(stderr, "Error: alloc failed for buf %lu\n", (unsigned long)i);
            for (uint64_t j = 0; j < i; j++) free(bufs[j]);
            free(bufs);
            close(fd);
            return TV_ERR;
        }
        memset(bufs[i], 0xAB, bs);
    }

    struct io_uring ring;
    int ret = io_uring_queue_init(queue_depth, &ring, 0);
    if (ret < 0) {
        fprintf(stderr, "Error: io_uring_queue_init: %s\n", strerror(-ret));
        for (uint64_t i = 0; i < count; i++) free(bufs[i]);
        free(bufs);
        close(fd);
        return TV_ERR;
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    uint64_t submitted = 0, completed = 0;

    while (completed < count && !g_stop) {
        while (submitted < count && (int)(submitted - completed) < queue_depth) {
            struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
            if (!sqe) break;
            io_uring_prep_write(sqe, fd, bufs[submitted], bs, submitted * bs);
            io_uring_sqe_set_data(sqe, (void *)(uintptr_t)submitted);
            submitted++;
        }

        io_uring_submit(&ring);

        struct io_uring_cqe *cqe;
        int ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0) {
            fprintf(stderr, "Error: io_uring_wait_cqe: %s\n", strerror(-ret));
            break;
        }
        int res = cqe->res;
        io_uring_cqe_seen(&ring, cqe);
        completed++;

        if (res < 0) {
            fprintf(stderr, "Error: io_uring write failed at entry %lu: %s\n",
                    (unsigned long)completed - 1, strerror(-res));
            break;
        }
    }

    fsync(fd);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    io_uring_queue_exit(&ring);

    uint64_t written = completed * bs;
    for (uint64_t i = 0; i < count; i++) free(bufs[i]);
    free(bufs);
    close(fd);

    double sec = elapsed_sec(&t0, &t1);
    double mb = (double)written / (1024.0 * 1024.0);
    double mbps = mb / sec;

    printf("Throughput: %.1f MB/s  (%.1f GB in %.2f sec)\n", mbps, mb / 1024.0, sec);
    return 0;
}

static int bench_read(const char *path, uint64_t size, int use_direct, uint64_t bs) {
    int flags = O_RDONLY;
    if (use_direct) flags |= O_DIRECT;

    int fd = open(path, flags);
    if (fd < 0) {
        fprintf(stderr, "Error: cannot open %s: %s\n", path, strerror(errno));
        return TV_ERR;
    }

    uint64_t dev_size = get_device_size(fd);
    if (size > dev_size) size = dev_size;

    uint8_t *buf;
    if (posix_memalign((void **)&buf, ALIGNMENT, bs) != 0) {
        fprintf(stderr, "Error: alloc failed\n");
        close(fd);
        return TV_ERR;
    }

    uint64_t total = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    while (total < size && !g_stop) {
        ssize_t n = pread(fd, buf, bs, (off_t)total);
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "Error: pread failed at %lu: %s\n",
                    (unsigned long)total, strerror(errno));
            break;
        }
        if (n == 0) break;
        total += n;
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    close(fd);
    free(buf);

    double sec = elapsed_sec(&t0, &t1);
    double mb = (double)total / (1024.0 * 1024.0);
    double mbps = mb / sec;

    printf("Throughput: %.1f MB/s  (%.1f GB in %.2f sec)\n", mbps, mb / 1024.0, sec);
    return 0;
}

static int cmd_info(const char *path) {
    TV_METADATA meta;
    char config_path[256];

    snprintf(config_path, sizeof(config_path), TV_CONFIG_DIR "%s.conf", path);
    if (tv_metadata_load(&meta, config_path) == 0) {
        printf("Metadata: v%u, chunk=%uKB, %u disks, %u segments\n",
               meta.version, meta.chunk_size / 1024,
               meta.disk_count, meta.segment_count);
        for (uint32_t i = 0; i < meta.disk_count; i++)
            printf("  Disk[%u] %s\n", i, meta.disk_names[i]);
        for (uint32_t i = 0; i < meta.segment_count; i++) {
            TV_SEGMENT *seg = &meta.segments[i];
            printf("  Segment[%u]: [%lu, %lu) %u disks, stripe=%luKB\n",
                   i, (unsigned long)seg->logical_begin,
                   (unsigned long)seg->logical_end,
                   seg->disk_count,
                   (unsigned long)(seg->stripe_size / 1024));
        }
        return 0;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Error: cannot open %s: %s\n", path, strerror(errno));
        return TV_ERR;
    }
    uint64_t sz = get_device_size(fd);
    close(fd);
    printf("Device: %s\nSize: %.1f GB\n", path, sz / (1024.0 * 1024 * 1024));
    return 0;
}

static void usage(void) {
    fprintf(stderr,
        "TieredVol I/O Benchmark Tool v" VERSION "\n\n"
        "Usage:\n"
        "  tiered_io --path <device> [options]     Benchmark a block device\n"
        "  tiered_io --name <vol>  [options]       Show metadata for a volume\n"
        "\nOptions:\n"
        "  --bench --size <N>MB    Write benchmark\n"
        "  --bench-read --size <N>MB  Read benchmark\n"
        "  --bench-all             Write suite: 512MB, 5GB, 10GB\n"
        "  --bench-read-all        Read suite: 512MB, 5GB, 10GB\n"
        "  --blocksize <N>         Block size (default: 16MB, min: 4K, max: 64MB)\n"
        "  --direct                Use O_DIRECT (default)\n"
        "  --no-direct             Disable O_DIRECT\n"
        "  --info                  Show device/volume info\n"
        "\nExamples:\n"
        "  sudo tiered_io --path /dev/mapper/myvol --bench --size 5GB\n"
        "  sudo tiered_io --path /dev/mapper/myvol --bench --blocksize 32M\n"
        "  sudo tiered_io --path /dev/mapper/myvol --bench-all\n"
        "  sudo tiered_io --path /dev/mapper/myvol --bench-read-all\n"
        "  sudo tiered_io --name myvol --info\n"
    );
}

static int run_suite(const char *path, int do_read, int use_direct, uint64_t bs) {
    uint64_t sizes[] = { 512ULL * 1024 * 1024,
                         5ULL * 1024 * 1024 * 1024,
                         10ULL * 1024 * 1024 * 1024 };
    const char *labels[] = { "512MB", "5GB", "10GB" };
    int ret = 0;
    for (int i = 0; i < 3; i++) {
        printf("--- %s %s (bs=%luKB) ---\n", do_read ? "Read" : "Write", labels[i],
               (unsigned long)(bs / 1024));
        if (do_read)
            ret = bench_read(path, sizes[i], use_direct, bs);
        else
            ret = bench_write(path, sizes[i], use_direct, bs);
        if (ret != 0 || g_stop) break;
        sleep(2);
    }
    return ret;
}

int main(int argc, char *argv[]) {
    signal(SIGTERM, sig_handler);
    signal(SIGINT, sig_handler);

    const char *path = NULL;
    const char *name = NULL;
    int do_bench = 0, do_bench_all = 0, do_bench_async = 0;
    int do_bench_read = 0, do_bench_read_all = 0;
    int do_info = 0;
    int use_direct = -1;
    uint64_t bench_size = 64 * 1024 * 1024;
    uint64_t block_size = BS_DEFAULT;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--path") == 0 && i + 1 < argc) path = argv[++i];
        else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) name = argv[++i];
        else if (strcmp(argv[i], "--bench") == 0) do_bench = 1;
        else if (strcmp(argv[i], "--bench-async") == 0) do_bench_async = 1;
        else if (strcmp(argv[i], "--bench-all") == 0) do_bench_all = 1;
        else if (strcmp(argv[i], "--bench-read") == 0) do_bench_read = 1;
        else if (strcmp(argv[i], "--bench-read-all") == 0) do_bench_read_all = 1;
        else if (strcmp(argv[i], "--info") == 0) do_info = 1;
        else if (strcmp(argv[i], "--direct") == 0) use_direct = 1;
        else if (strcmp(argv[i], "--no-direct") == 0) use_direct = 0;
        else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) bench_size = parse_size(argv[++i]);
        else if (strcmp(argv[i], "--blocksize") == 0 && i + 1 < argc) {
            block_size = parse_size(argv[++i]);
            if (block_size < BS_MIN || block_size > BS_MAX) {
                fprintf(stderr, "Error: blocksize must be between 4K and 64M\n");
                return 1;
            }
        }
        else { usage(); return 1; }
    }

    if (use_direct < 0) use_direct = 1;

    if (name) {
        return cmd_info(name);
    }

    if (!path) { usage(); return 1; }

    if (do_info) return cmd_info(path);

    printf("Block size: %lu KB\n", (unsigned long)(block_size / 1024));

    if (do_bench_all) return run_suite(path, 0, use_direct, block_size);
    if (do_bench_read_all) return run_suite(path, 1, use_direct, block_size);
    if (do_bench_read) return bench_read(path, bench_size, use_direct, block_size);
    if (do_bench_async) return bench_write_async(path, bench_size, use_direct, block_size);
    if (do_bench) return bench_write(path, bench_size, use_direct, block_size);

    fprintf(stderr, "Error: specify --bench, --bench-read, --bench-all, --bench-read-all, or --info\n");
    usage();
    return 1;
}
