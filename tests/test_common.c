#include <stdio.h>
#include <string.h>
#include <math.h>
#include "../src/tiered_common.h"
#include "../src/setup_bench.h"
#include "test_common.h"

static void test_valid_name(void) {
    printf("\n[TEST] tiered_is_valid_name\n");
    check(tiered_is_valid_name("fastpool") == 1, "fastpool ok");
    check(tiered_is_valid_name("pool-v2") == 1, "pool-v2 ok");
    check(tiered_is_valid_name("pool.v2") == 1, "pool.v2 ok");
    check(tiered_is_valid_name("Pool_01") == 1, "Pool_01 ok");
    check(tiered_is_valid_name("a") == 1, "single char ok");
    check(tiered_is_valid_name("") == 0, "empty name rejected");
    check(tiered_is_valid_name(NULL) == 0, "NULL rejected");
    check(tiered_is_valid_name("pool name") == 0, "space rejected");
    check(tiered_is_valid_name("pool;rm -rf /") == 0, "semicolon injection rejected");
    check(tiered_is_valid_name("pool|cat /etc/passwd") == 0, "pipe injection rejected");
    check(tiered_is_valid_name("pool$(whoami)") == 0, "dollar injection rejected");
    check(tiered_is_valid_name("pool`id`") == 0, "backtick injection rejected");
}

static void test_valid_fs(void) {
    printf("\n[TEST] tiered_is_valid_fs\n");
    check(tiered_is_valid_fs("ext4") == 1, "ext4 ok");
    check(tiered_is_valid_fs("ext3") == 1, "ext3 ok");
    check(tiered_is_valid_fs("xfs") == 1, "xfs ok");
    check(tiered_is_valid_fs("btrfs") == 1, "btrfs ok");
    check(tiered_is_valid_fs("none") == 1, "none ok");
    check(tiered_is_valid_fs("ntfs") == 0, "ntfs rejected");
    check(tiered_is_valid_fs("fat32") == 0, "fat32 rejected");
    check(tiered_is_valid_fs("ext4;rm -rf /") == 0, "injection rejected");
    check(tiered_is_valid_fs("") == 0, "empty rejected");
    check(tiered_is_valid_fs(NULL) == 0, "NULL rejected");
}

static void test_valid_mount(void) {
    printf("\n[TEST] tiered_is_valid_mount\n");
    check(tiered_is_valid_mount("/mnt/fast") == 1, "/mnt/fast ok");
    check(tiered_is_valid_mount("/home") == 1, "/home ok");
    check(tiered_is_valid_mount("/") == 1, "/ ok");
    check(tiered_is_valid_mount("relative") == 0, "relative rejected");
    check(tiered_is_valid_mount("") == 0, "empty rejected");
    check(tiered_is_valid_mount(NULL) == 0, "NULL rejected");
    check(tiered_is_valid_mount("/mnt/../etc") == 0, "/mnt/../etc rejected");
    check(tiered_is_valid_mount("/..") == 0, "/.. rejected");
    check(tiered_is_valid_mount("/mnt/data/..") == 0, "/mnt/data/.. rejected");
    check(tiered_is_valid_mount("/a") == 1, "/a short path ok");
    check(tiered_is_valid_mount("/ab") == 1, "/ab short path ok");
    check(tiered_is_valid_mount("/abc") == 1, "/abc ok");
}

static void test_physical_disk(void) {
    printf("\n[TEST] tiered_is_physical_disk\n");
    check(tiered_is_physical_disk("nvme0n1") == 1, "nvme0n1 ok");
    check(tiered_is_physical_disk("sda") == 1, "sda ok");
    check(tiered_is_physical_disk("") == 0, "empty rejected");
    check(tiered_is_physical_disk(NULL) == 0, "NULL rejected");
    check(tiered_is_physical_disk("loop0") == 0, "loop0 rejected");
    check(tiered_is_physical_disk("loop") == 0, "loop rejected");
    check(tiered_is_physical_disk("ram0") == 0, "ram0 rejected");
    check(tiered_is_physical_disk("zram0") == 0, "zram0 rejected");
    check(tiered_is_physical_disk("fd0") == 0, "fd0 rejected");
    check(tiered_is_physical_disk("sr0") == 0, "sr0 rejected");
}

static void test_cmp_speed(void) {
    printf("\n[TEST] cmp_speed\n");
    disk_t a, b;
    a.speed_write = 1000; a.speed_read = 1000;
    b.speed_write = 500;  b.speed_read = 500;
    check(cmp_speed(&a, &b) < 0, "faster disk sorts first");
    check(cmp_speed(&b, &a) > 0, "slower disk sorts later");
    a.speed_write = 2000; a.speed_read = 2000;
    b.speed_write = 2000; b.speed_read = 2000;
    check(cmp_speed(&a, &b) == 0, "equal speeds → 0");
    a.speed_write = 0.0/0.0; a.speed_read = 0.0/0.0;
    b.speed_write = 1000; b.speed_read = 1000;
    check(cmp_speed(&a, &b) > 0, "NaN sorts after valid");
    b.speed_write = 0.0/0.0; b.speed_read = 0.0/0.0;
    check(cmp_speed(&a, &b) == 0, "both NaN → 0");
}

int main(void) {
    printf("=== TieredVol Common Unit Tests ===\n");

    test_valid_name();
    test_valid_fs();
    test_valid_mount();
    test_physical_disk();
    test_cmp_speed();

    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
