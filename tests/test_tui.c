#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../src/tiered_ui_helpers.h"

static char bench_buf[16384] = "";
static int bench_buf_len = 0;

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

static void test_parse_parallel_2disk(void) {
    printf("\n[TEST] parse_bench_output — parallel 2 disk\n");
    const char *out =
        "Benchmarking 2 disks in parallel...\n"
        "  Started benchmark for /dev/sdb (pid 100)\n"
        "  Started benchmark for /dev/sdc (pid 101)\n"
        "  /dev/sdc: Write 426 MB/s  Read 441 MB/s\n"
        "  /dev/sdb: Write 263 MB/s  Read 385 MB/s\n";

    ui_disk_t d;
    memset(&d, 0, sizeof(d));
    strncpy(d.disk, "sdb", sizeof(d.disk));
    parse_bench_output(out, &d);
    check(fabs(d.speed_write - 263.0) < 1.0, "sdb write = 263");
    check(fabs(d.speed_read - 385.0) < 1.0, "sdb read = 385");

    memset(&d, 0, sizeof(d));
    strncpy(d.disk, "sdc", sizeof(d.disk));
    parse_bench_output(out, &d);
    check(fabs(d.speed_write - 426.0) < 1.0, "sdc write = 426");
    check(fabs(d.speed_read - 441.0) < 1.0, "sdc read = 441");
}

static void test_parse_parallel_3disk(void) {
    printf("\n[TEST] parse_bench_output — parallel 3 disk, out of order\n");
    const char *out =
        "Benchmarking 3 disks in parallel...\n"
        "  /dev/nvme0n1: Write 935 MB/s  Read 943 MB/s\n"
        "  /dev/sdb: Write 254 MB/s  Read 376 MB/s\n"
        "  /dev/sdc: Write 239 MB/s  Read 372 MB/s\n";

    ui_disk_t d;
    memset(&d, 0, sizeof(d));
    strncpy(d.disk, "nvme0n1", sizeof(d.disk));
    parse_bench_output(out, &d);
    check(fabs(d.speed_write - 935.0) < 1.0, "nvme0n1 write = 935");
    check(fabs(d.speed_read - 943.0) < 1.0, "nvme0n1 read = 943");

    memset(&d, 0, sizeof(d));
    strncpy(d.disk, "sdb", sizeof(d.disk));
    parse_bench_output(out, &d);
    check(fabs(d.speed_write - 254.0) < 1.0, "sdb write = 254");
    check(fabs(d.speed_read - 376.0) < 1.0, "sdb read = 376");

    memset(&d, 0, sizeof(d));
    strncpy(d.disk, "sdc", sizeof(d.disk));
    parse_bench_output(out, &d);
    check(fabs(d.speed_write - 239.0) < 1.0, "sdc write = 239");
    check(fabs(d.speed_read - 372.0) < 1.0, "sdc read = 372");
}

static void test_parse_started_line_not_matched(void) {
    printf("\n[TEST] parse_bench_output — 'Started' line should not match\n");
    const char *out =
        "Started benchmark for /dev/sdb (pid 1234)\n"
        "  /dev/sdc: Write 426 MB/s  Read 441 MB/s\n"
        "  /dev/sdb: Write 263 MB/s  Read 385 MB/s\n";

    ui_disk_t d;
    memset(&d, 0, sizeof(d));
    strncpy(d.disk, "sdb", sizeof(d.disk));
    parse_bench_output(out, &d);
    check(fabs(d.speed_write - 263.0) < 1.0, "sdb write = 263 (not from Started line)");
    check(fabs(d.speed_read - 385.0) < 1.0, "sdb read = 385 (not from Started line)");
}

static void test_parse_disk_not_found(void) {
    printf("\n[TEST] parse_bench_output — disk not in output\n");
    const char *out =
        "  /dev/sdb: Write 263 MB/s  Read 385 MB/s\n";

    ui_disk_t d;
    memset(&d, 0, sizeof(d));
    strncpy(d.disk, "sdc", sizeof(d.disk));
    parse_bench_output(out, &d);
    check(d.speed_write == 0.0, "sdc write = 0");
    check(d.speed_read == 0.0, "sdc read = 0");
}

static void test_bench_done_parallel(void) {
    printf("\n[TEST] bench_disk_done — parallel output, disk completed\n");
    bench_buf_len = 0;
    bench_buf[0] = 0;
    const char *chunk =
        "  /dev/sdb: Write 263 MB/s  Read 385 MB/s\n"
        "  /dev/sdc: Write 426 MB/s  Read 441 MB/s\n";
    strcpy(bench_buf, chunk);
    bench_buf_len = strlen(bench_buf);

    check(bench_disk_done("sdb", bench_buf) == 1, "sdb done");
    check(bench_disk_done("sdc", bench_buf) == 1, "sdc done");
    check(bench_disk_done("nvme0n1", bench_buf) == 0, "nvme0n1 not done");
}

static void test_bench_done_old_format_rejected(void) {
    printf("\n[TEST] bench_disk_done — old sequential format should NOT match\n");
    bench_buf_len = 0;
    bench_buf[0] = 0;
    const char *chunk =
        "Testing /dev/sdb ... Write: 263 MB/s  Read: 385 MB/s\n";
    strcpy(bench_buf, chunk);
    bench_buf_len = strlen(bench_buf);

    check(bench_disk_done("sdb", bench_buf) == 0, "old format 'Testing /dev/sdb ... Write:' not matched");
}

static void test_bench_done_not_started(void) {
    printf("\n[TEST] bench_disk_done — disk not in buffer at all\n");
    bench_buf_len = 0;
    bench_buf[0] = 0;

    check(bench_disk_done("sdb", bench_buf) == 0, "empty buffer = not done");
}

static void test_lvs_command(void) {
    printf("\n[TEST] lvs command construction\n");
    const char *vol_name = "myvolume";
    char lv_cmd[512];
    snprintf(lv_cmd, sizeof(lv_cmd),
        "sudo lvs --noheadings -o lv_name,lv_size,stripes,stripesize tv_vg_%s 2>/dev/null",
        vol_name);

    check(strstr(lv_cmd, "tv_vg_myvolume") != NULL, "contains tv_vg_myvolume");
    check(strstr(lv_cmd, "tv_vg_2>/dev/null") == NULL, "does NOT contain tv_vg_2>/dev/null");
    check(strstr(lv_cmd, "--noheadings") != NULL, "contains --noheadings");
}

int main(void) {
    printf("=== TieredVol TUI Unit Tests ===\n");

    test_parse_parallel_2disk();
    test_parse_parallel_3disk();
    test_parse_started_line_not_matched();
    test_parse_disk_not_found();
    test_bench_done_parallel();
    test_bench_done_old_format_rejected();
    test_bench_done_not_started();
    test_lvs_command();

    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
