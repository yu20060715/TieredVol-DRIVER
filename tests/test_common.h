#ifndef TEST_COMMON_H
#define TEST_COMMON_H

#include <stdio.h>

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

#endif
