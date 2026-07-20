#include <stdio.h>
#include <string.h>
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

static TV_METADATA single_seg_meta(void) {
    TV_METADATA m;
    memset(&m, 0, sizeof(m));
    m.version = 1;
    m.chunk_size = 1024 * 1024;
    m.disk_count = 2;
    strcpy(m.disk_names[0], "nvme0n1");
    strcpy(m.disk_names[1], "sda");

    m.segment_count = 1;
    m.segments[0].logical_begin = 0;
    m.segments[0].logical_end = 10ULL * 1024 * 1024 * 1024;
    m.segments[0].disk_count = 2;
    m.segments[0].disk_index[0] = 0;
    m.segments[0].disk_index[1] = 1;
    m.segments[0].weight[0] = 3;
    m.segments[0].weight[1] = 1;
    m.segments[0].stripe_size = 4ULL * 1024 * 1024;
    return m;
}

static TV_METADATA dual_seg_meta(void) {
    TV_METADATA m;
    memset(&m, 0, sizeof(m));
    m.version = 1;
    m.chunk_size = 1024 * 1024;
    m.disk_count = 2;
    strcpy(m.disk_names[0], "nvme0n1");
    strcpy(m.disk_names[1], "sda");

    m.segment_count = 2;
    m.segments[0].logical_begin = 0;
    m.segments[0].logical_end = 5ULL * 1024 * 1024 * 1024;
    m.segments[0].disk_count = 2;
    m.segments[0].disk_index[0] = 0;
    m.segments[0].disk_index[1] = 1;
    m.segments[0].weight[0] = 3;
    m.segments[0].weight[1] = 1;
    m.segments[0].stripe_size = 4ULL * 1024 * 1024;

    m.segments[1].logical_begin = 5ULL * 1024 * 1024 * 1024;
    m.segments[1].logical_end = 10ULL * 1024 * 1024 * 1024;
    m.segments[1].disk_count = 2;
    m.segments[1].disk_index[0] = 1;
    m.segments[1].disk_index[1] = 0;
    m.segments[1].weight[0] = 2;
    m.segments[1].weight[1] = 2;
    m.segments[1].stripe_size = 4ULL * 1024 * 1024;
    return m;
}

static void test_map_single_seg_begin(void) {
    printf("\n[TEST] tv_map_logical single segment, offset 0\n");
    TV_METADATA m = single_seg_meta();
    TV_MAP map = tv_map_logical(0, &m);
    check(map.disk == 0, "maps to disk 0 (nvme0n1)");
    check(map.offset == 0, "offset 0");
    check(map.length == m.segments[0].weight[0] * TV_CHUNK_SIZE, "length = weight * chunk");
}

static void test_map_single_seg_mid(void) {
    printf("\n[TEST] tv_map_logical single segment, mid offset\n");
    TV_METADATA m = single_seg_meta();
    uint64_t stripe = m.segments[0].stripe_size;
    TV_MAP map = tv_map_logical(stripe * 2 + 1, &m);
    check(map.disk == 0 || map.disk == 1, "maps to a valid disk");
    check(map.offset > 0, "non-zero offset");
    check(map.length > 0, "non-zero length");
}

static void test_map_single_seg_boundary(void) {
    printf("\n[TEST] tv_map_logical single segment, near end\n");
    TV_METADATA m = single_seg_meta();
    uint64_t end = m.segments[0].logical_end;
    TV_MAP map = tv_map_logical(end - 1, &m);
    check(map.disk >= 0, "valid disk index");
    check(map.length > 0, "non-zero length");
}

static void test_map_dual_seg_first(void) {
    printf("\n[TEST] tv_map_logical dual segment, first segment\n");
    TV_METADATA m = dual_seg_meta();
    TV_MAP map = tv_map_logical(0, &m);
    check(map.disk == 0, "segment 0, offset 0 -> disk 0");
}

static void test_map_dual_seg_second(void) {
    printf("\n[TEST] tv_map_logical dual segment, second segment\n");
    TV_METADATA m = dual_seg_meta();
    uint64_t boundary = m.segments[0].logical_end;
    TV_MAP map = tv_map_logical(boundary, &m);
    check(map.disk >= 0, "valid disk index in segment 1");
}

int main(void) {
    printf("=== TieredVol Mapper Unit Tests ===\n");

    test_map_single_seg_begin();
    test_map_single_seg_mid();
    test_map_single_seg_boundary();
    test_map_dual_seg_first();
    test_map_dual_seg_second();

    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
