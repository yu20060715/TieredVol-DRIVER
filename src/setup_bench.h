#ifndef SETUP_BENCH_H
#define SETUP_BENCH_H

#include "setup_discover.h"

#include <stddef.h>
#include <signal.h>

typedef void (*bench_interrupt_fn)(void *ctx);

int cmd_bench(int argc, char *argv[]);
int run_parallel_bench(disk_t *disks, int ndisks, int warmup,
                       bench_interrupt_fn on_interrupt, void *interrupt_ctx);
int cmp_speed(const void *a, const void *b);

#endif
