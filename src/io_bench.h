#ifndef IO_BENCH_H
#define IO_BENCH_H

#include "tiered_sched.h"

int open_disks(TV_METADATA *meta, TV_DISK *disks, int use_direct, int use_raw);
void close_disks(TV_DISK *disks, int ndisks);
int discard_disks(TV_DISK *disks, int ndisks);

int cmd_bench_one(TV_SCHED *sched, uint64_t size, int warmup, TV_METADATA *meta);
int cmd_bench_all(TV_METADATA *meta, int use_raw);
int cmd_bench_read_one(TV_SCHED *sched, uint64_t size, TV_METADATA *meta);
int cmd_bench_read_all(TV_METADATA *meta, int use_raw);
int cmd_bench_path(const char *path, uint64_t size, int warmup, int use_direct, int raw);
int cmd_bench_path_all(const char *path, int use_direct, int raw);
int do_warmup_exec(TV_SCHED *sched, TV_METADATA *meta);

#endif
