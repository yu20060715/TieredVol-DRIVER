#ifndef TIERED_TYPES_H
#define TIERED_TYPES_H

#include <stdint.h>
#include <signal.h>

extern volatile sig_atomic_t g_shutdown_requested;

#define TV_MAX_DISKS    16
#define TV_MAX_SEGS     16
#define TV_MAX_WEIGHT   16
#define TV_CHUNK_SIZE (1 * 1024 * 1024)
#define TV_OK       0
#define TV_ERR      (-1)

#define TV_CONFIG_DIR           "/etc/tieredvol/"

typedef struct {
    int      id;
    uint64_t free_size;
    uint64_t speed;
    uint32_t weight;
    char     name[64];
} TV_DISK;

typedef struct {
    uint64_t logical_begin;
    uint64_t logical_end;
    uint32_t disk_count;
    uint32_t disk_index[TV_MAX_DISKS];
    uint32_t weight[TV_MAX_DISKS];
    uint64_t stripe_size;
} TV_SEGMENT;

typedef struct {
    uint32_t version;
    uint32_t chunk_size;
    uint32_t segment_count;
    uint32_t disk_count;
    char     disk_names[TV_MAX_DISKS][64];
    TV_SEGMENT segments[TV_MAX_SEGS];
} TV_METADATA;

uint32_t tv_compute_weight(uint64_t speed, uint64_t slowest);
int      tv_build_segments(TV_DISK *disks, int ndisks, TV_SEGMENT *segs, int *nsegs);
uint64_t tv_compute_stripe_size(uint32_t *weights, int nweights, uint32_t chunk_size);

int  tv_metadata_save(TV_METADATA *meta, const char *path);
int  tv_metadata_load(TV_METADATA *meta, const char *path);

int  tv_benchmark(const char *disk_path, uint64_t *speed_out, int warmup);

#endif
