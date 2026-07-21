#include <stdio.h>
#include <string.h>
#include "../src/tiered_sched.h"
#include "test_common.h"

static void test_init_null(void) {
    printf("\n[TEST] tv_sched_init with NULL args\n");
    TV_SCHED *s = tv_sched_init(NULL, 0, NULL);
    check(s == NULL, "all-NULL returns NULL");
}

static void test_init_no_segments(void) {
    printf("\n[TEST] tv_sched_init with zero segments\n");
    TV_METADATA meta;
    memset(&meta, 0, sizeof(meta));
    meta.version = 1;
    meta.chunk_size = 1024 * 1024;
    meta.segment_count = 0;
    TV_DISK disks[1];
    memset(disks, 0, sizeof(disks));
    TV_SCHED *s = tv_sched_init(disks, 1, &meta);
    check(s == NULL, "zero segments returns NULL");
}

static void test_init_destroy_cycle(void) {
    printf("\n[TEST] tv_sched_init / tv_sched_destroy cycle\n");
    TV_METADATA meta;
    memset(&meta, 0, sizeof(meta));
    meta.version = 1;
    meta.chunk_size = 1024 * 1024;
    meta.segment_count = 1;
    meta.disk_count = 1;
    strcpy(meta.disk_names[0], "test");
    meta.segments[0].logical_begin = 0;
    meta.segments[0].logical_end = 10ULL * 1024ULL * 1024ULL * 1024ULL;
    meta.segments[0].disk_count = 1;
    meta.segments[0].disk_index[0] = 0;
    meta.segments[0].weight[0] = 1;
    meta.segments[0].stripe_size = 1024ULL * 1024ULL;

    TV_DISK disks[1];
    memset(disks, 0, sizeof(disks));
    disks[0].id = 0;
    strcpy(disks[0].name, "test");
    disks[0].fd = -1;

    TV_SCHED *s = tv_sched_init(disks, 1, &meta);
    check(s != NULL, "init succeeded");
    if (!s) return;
    check(s->stripe_size == meta.segments[0].stripe_size, "stripe_size matches");
    check(s->ndisks == 1, "ndisks matches");
    check(s->inflight == 0, "inflight starts at 0");

    tv_sched_destroy(s);
    check(1, "destroy completed without crash");
}

static void test_write_null(void) {
    printf("\n[TEST] tv_write with NULL args\n");
    int ret = tv_write(NULL, "hello", 5);
    check(ret == -1, "NULL sched returns -1");
    TV_METADATA dummy; memset(&dummy, 0, sizeof(dummy));
    TV_DISK d; memset(&d, 0, sizeof(d));
    TV_SCHED s; memset(&s, 0, sizeof(s));
    ret = tv_write(&s, NULL, 5);
    check(ret == -1, "NULL buf returns -1");
    ret = tv_write(&s, "hello", 0);
    check(ret == -1, "zero length returns -1");
}

static void test_read_null(void) {
    printf("\n[TEST] tv_read with NULL args\n");
    int ret = tv_read(NULL, NULL, 0, 0);
    check(ret == -1, "NULL sched returns -1");
    TV_SCHED *s = NULL;
    ret = tv_read(s, "buf", 1, 0);
    check(ret == -1, "NULL sched explicit returns -1");
}

static void test_flush_null(void) {
    printf("\n[TEST] tv_flush with NULL sched\n");
    int ret = tv_flush(NULL);
    check(ret == -1, "NULL returns -1");
}

static void test_seek_null(void) {
    printf("\n[TEST] tv_sched_seek with NULL sched\n");
    int ret = tv_sched_seek(NULL, 0);
    check(ret == -1, "NULL returns -1");
}

static void test_destroy_null(void) {
    printf("\n[TEST] tv_sched_destroy with NULL\n");
    tv_sched_destroy(NULL);
    check(1, "NULL destroy no-op (no crash)");
}

static void test_flush_no_inflight(void) {
    printf("\n[TEST] tv_flush with no in-flight I/O\n");
    TV_METADATA meta;
    memset(&meta, 0, sizeof(meta));
    meta.version = 1;
    meta.chunk_size = 1024 * 1024;
    meta.segment_count = 1;
    meta.disk_count = 1;
    strcpy(meta.disk_names[0], "test");
    meta.segments[0].logical_begin = 0;
    meta.segments[0].logical_end = 10ULL * 1024ULL * 1024ULL * 1024ULL;
    meta.segments[0].disk_count = 1;
    meta.segments[0].disk_index[0] = 0;
    meta.segments[0].weight[0] = 1;
    meta.segments[0].stripe_size = 1024ULL * 1024ULL;

    TV_DISK disks[1];
    memset(disks, 0, sizeof(disks));
    disks[0].id = 0;
    strcpy(disks[0].name, "test");
    disks[0].fd = -1;

    TV_SCHED *s = tv_sched_init(disks, 1, &meta);
    check(s != NULL, "init succeeded");
    if (!s) return;
    int ret = tv_flush(s);
    check(ret == 0, "flush with no I/O returns 0");
    tv_sched_destroy(s);
}

static void test_seek_rejects_unaligned(void) {
    printf("\n[TEST] tv_sched_seek rejects non-aligned offset\n");
    TV_METADATA meta;
    memset(&meta, 0, sizeof(meta));
    meta.version = 1;
    meta.chunk_size = 1024 * 1024;
    meta.segment_count = 1;
    meta.disk_count = 1;
    strcpy(meta.disk_names[0], "test");
    meta.segments[0].logical_begin = 0;
    meta.segments[0].logical_end = 10ULL * 1024ULL * 1024ULL * 1024ULL;
    meta.segments[0].disk_count = 1;
    meta.segments[0].disk_index[0] = 0;
    meta.segments[0].weight[0] = 1;
    meta.segments[0].stripe_size = 1024ULL * 1024ULL;

    TV_DISK disks[1];
    memset(disks, 0, sizeof(disks));
    disks[0].id = 0;
    strcpy(disks[0].name, "test");
    disks[0].fd = -1;

    TV_SCHED *s = tv_sched_init(disks, 1, &meta);
    check(s != NULL, "init succeeded");
    if (!s) return;
    int ret = tv_sched_seek(s, 1);
    check(ret == -1, "offset 1 (not stripe-aligned) returns -1");
    ret = tv_sched_seek(s, meta.segments[0].stripe_size);
    check(ret == 0, "offset = stripe_size (aligned) returns 0");
    tv_sched_destroy(s);
}

int main(void) {
    printf("=== TieredVol Scheduler Unit Tests ===\n");

    test_init_null();
    test_init_no_segments();
    test_init_destroy_cycle();
    test_write_null();
    test_read_null();
    test_flush_null();
    test_seek_null();
    test_destroy_null();
    test_flush_no_inflight();
    test_seek_rejects_unaligned();

    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
