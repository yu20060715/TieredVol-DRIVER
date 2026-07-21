#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/loop.h>

#include "../src/tiered_sched.h"

static int tests_run = 0;
static int tests_passed = 0;

static void check(int cond, const char *name) {
    tests_run++;
    if (cond) {
        tests_passed++;
        printf("  PASS  %s\n", name);
    } else {
        printf("  FAIL  %s\n", name);
    }
}

static void print_summary(void) {
    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
}

static int open_disk(const char *path) {
    int fd = open(path, O_RDWR | O_DIRECT);
    if (fd < 0) {
        fprintf(stderr, "  WARN: cannot open %s with O_DIRECT, trying non-direct\n", path);
        fd = open(path, O_RDWR);
    }
    return fd;
}

static TV_METADATA make_meta(uint64_t size) {
    TV_METADATA meta;
    memset(&meta, 0, sizeof(meta));
    meta.version = 1;
    meta.chunk_size = TV_CHUNK_SIZE;
    meta.segment_count = 1;
    meta.disk_count = 1;
    strcpy(meta.disk_names[0], "test_dev");
    meta.segments[0].logical_begin = 0;
    meta.segments[0].logical_end = size;
    meta.segments[0].disk_count = 1;
    meta.segments[0].weight[0] = 1;
    meta.segments[0].disk_index[0] = 0;
    meta.segments[0].stripe_size = TV_CHUNK_SIZE;
    return meta;
}

static void test_init_destroy_cycle(void) {
    printf("\n[TEST] tv_sched_init / tv_sched_destroy cycle\n");
    TV_METADATA meta = make_meta(64ULL * 1024 * 1024);
    TV_DISK disks[1];
    memset(disks, 0, sizeof(disks));
    disks[0].id = 0;
    strcpy(disks[0].name, "test_dev");
    disks[0].fd = -1;

    TV_SCHED *sched = tv_sched_init(disks, 1, &meta);
    check(sched != NULL, "init succeeded");
    if (sched) {
        check(sched->stripe_size == meta.segments[0].stripe_size,
              "stripe_size matches");
        check((int)sched->ndisks == (int)meta.disk_count, "ndisks matches");
        check(sched->inflight == 0, "inflight starts at 0");
        tv_sched_destroy(sched);
        check(1, "destroy completed without crash");
    }
}

static void test_write_read_verify(const char *devpath) {
    printf("\n[TEST] write/read/verify data integrity\n");
    int fd = open_disk(devpath);
    if (fd < 0) {
        printf("  SKIP  cannot open device %s\n", devpath);
        return;
    }

    TV_METADATA meta = make_meta(64ULL * 1024 * 1024);
    TV_DISK disks[1];
    memset(disks, 0, sizeof(disks));
    disks[0].id = 0;
    strcpy(disks[0].name, "test_dev");
    disks[0].fd = fd;

    TV_SCHED *sched = tv_sched_init(disks, 1, &meta);
    check(sched != NULL, "init succeeded");
    if (!sched) { close(fd); return; }

    uint64_t len = 4096;
    uint8_t *wbuf, *rbuf;
    int ok = 1;
    ok = ok && (posix_memalign((void **)&wbuf, 512, len) == 0);
    ok = ok && (posix_memalign((void **)&rbuf, 512, len) == 0);
    check(ok, "buffer allocation");
    if (!ok) { free(wbuf); free(rbuf); tv_sched_destroy(sched); close(fd); return; }

    for (uint64_t i = 0; i < len; i++) wbuf[i] = (uint8_t)(i & 0xFF);
    memset(rbuf, 0, len);

    int ret = tv_write(sched, wbuf, len);
    check(ret == 0, "write returned 0");

    ret = tv_flush(sched);
    check(ret == 0, "flush returned 0");

    ret = tv_read(sched, rbuf, len, 0);
    check(ret == 0, "read returned 0");

    check(memcmp(wbuf, rbuf, len) == 0, "readback data matches written data");

    free(wbuf);
    free(rbuf);
    tv_sched_destroy(sched);
    close(fd);
}

int main(int argc, char *argv[]) {
    printf("=== TieredVol Integration Tests ===\n");

    test_init_destroy_cycle();

    if (argc > 1) {
        test_write_read_verify(argv[1]);
    } else {
        printf("\n[TEST] write/read/verify data integrity\n");
        printf("  SKIP  no block device path provided\n");
    }

    print_summary();
    return (tests_passed == tests_run) ? 0 : 1;
}
