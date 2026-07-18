#ifndef TIERED_SCHED_H
#define TIERED_SCHED_H

#include <stdint.h>
#include <liburing.h>

#define TV_MAX_DISKS    16
#define TV_MAX_SEGS     16
#define TV_CHUNK_SIZE   (64 * 1024)

typedef struct {
    int      id;
    int      fd;
    uint64_t total_size;
    uint64_t free_size;
    uint64_t speed;
    uint32_t weight;
    uint64_t physical_offset;
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

typedef struct {
    int      disk;
    uint64_t offset;
    uint64_t length;
} TV_MAP;

typedef struct {
    uint8_t *data;
    uint64_t used;
    uint64_t capacity;
    uint64_t logical_begin;
} TV_BUFFER;

typedef struct {
    io_uring ring;
    TV_METADATA *meta;
    TV_DISK     *disks;
    int          ndisks;
    TV_BUFFER    buf;
} TV_SCHED;

uint32_t tv_compute_weight(uint64_t speed, uint64_t slowest);
int      tv_build_segments(TV_DISK *disks, int ndisks, TV_SEGMENT *segs, int *nsegs);
uint64_t tv_compute_stripe_size(uint32_t *weights, int nweights, uint32_t chunk_size);

TV_MAP   tv_map_logical(uint64_t logical, TV_METADATA *meta);
uint64_t tv_map_reverse(int disk_index, uint64_t disk_offset, TV_METADATA *meta);

TV_SCHED *tv_sched_init(TV_DISK *disks, int ndisks, TV_METADATA *meta);
int       tv_write(TV_SCHED *sched, const void *buf, uint64_t len);
int       tv_read(TV_SCHED *sched, void *buf, uint64_t len, uint64_t offset);
int       tv_flush(TV_SCHED *sched);
void      tv_sched_destroy(TV_SCHED *sched);

int  tv_metadata_save(TV_METADATA *meta, const char *path);
int  tv_metadata_load(TV_METADATA *meta, const char *path);

int  tv_benchmark(const char *disk_path, uint64_t *speed_out);

void tv_buf_destroy(TV_BUFFER *buf);

#endif
