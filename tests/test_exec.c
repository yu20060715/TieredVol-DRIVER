#include <stdio.h>
#include <string.h>
#include "../src/exec_helper.h"
#include "test_common.h"

static void test_run_true(void) {
    printf("\n[TEST] tv_exec_run true\n");
    char *argv[] = {"true", NULL};
    check(tv_exec_run("/bin/true", argv) == 0, "/bin/true returns 0");
    check(tv_exec_run("/bin/true", argv) == 0, "/bin/true again");
}

static void test_run_false(void) {
    printf("\n[TEST] tv_exec_run false\n");
    char *argv[] = {"false", NULL};
    check(tv_exec_run("/bin/false", argv) == 1, "/bin/false returns 1");
}

static void test_run_nonexistent(void) {
    printf("\n[TEST] tv_exec_run nonexistent\n");
    char *argv[] = {"no_such_command_v42", NULL};
    check(tv_exec_run("/no/such/path", argv) == 127, "nonexistent path -> 127");
}

static void test_capture_echo(void) {
    printf("\n[TEST] tv_exec_capture echo\n");
    char out[128];
    char *argv[] = {"echo", "hello world", NULL};
    int ret = tv_exec_capture("/bin/echo", argv, out, sizeof(out));
    check(ret == 0, "echo succeeded");
    check(strcmp(out, "hello world\n") == 0, "captured 'hello world\\n'");
}

static void test_capture_truncate(void) {
    printf("\n[TEST] tv_exec_capture truncation\n");
    char tiny[4];
    char *argv[] = {"echo", "abcdefghij", NULL};
    int ret = tv_exec_capture("/bin/echo", argv, tiny, sizeof(tiny));
    check(ret == 0, "echo succeeded even with small buffer");
    check(strlen(tiny) <= 3, "output truncated to fit buffer");
    check(tiny[3] == '\0', "buffer null-terminated");
}

int main(void) {
    printf("=== TieredVol Exec Helper Unit Tests ===\n");

    test_run_true();
    test_run_false();
    test_run_nonexistent();
    test_capture_echo();
    test_capture_truncate();

    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
