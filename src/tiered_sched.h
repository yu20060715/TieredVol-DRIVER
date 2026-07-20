#ifndef TIERED_SCHED_H
#define TIERED_SCHED_H

#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <liburing.h>

/* Checked by tv_write/tv_flush to allow graceful SIGTERM shutdown.
 * Defined in tiered_io.c, referenced here so the scheduler core
 * can bail out without touching any io_uring primitives. */
extern volatile sig_atomic_t g_shutdown_requested;

#define TV_MAX_DISKS    16
#define TV_MAX_SEGS     16
#define TV_CHUNK_SIZE   (256 * 1024)
#define TV_BUF_COUNT    8   /* stripe buffer pool size for pipelining */

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
    uint8_t *data;       /* 512-byte aligned stripe buffer */
    uint64_t logical;    /* logical offset for this stripe */
    int      in_flight;  /* 1 = I/O submitted, waiting for completion */
    int      cqes_pending; /* SQEs still in flight for this stripe */
} TV_STRIPE_BUF;

typedef struct {
    struct io_uring ring;
    TV_METADATA *meta;
    TV_DISK     *disks;
    int          ndisks;
    /* Stripe buffer pool for pipelining */
    TV_STRIPE_BUF sbuf[TV_BUF_COUNT];
    uint64_t      stripe_size;
    int           sbuf_head;     /* index of buffer being filled */
    uint64_t      sbuf_used;     /* bytes used in current buffer */
    uint64_t      sbuf_logical;  /* logical offset of current buffer */
    int           inflight;      /* number of in-flight I/O submissions */
} TV_SCHED;

uint32_t tv_compute_weight(uint64_t speed, uint64_t slowest);
int      tv_build_segments(TV_DISK *disks, int ndisks, TV_SEGMENT *segs, int *nsegs);
uint64_t tv_compute_stripe_size(uint32_t *weights, int nweights, uint32_t chunk_size);

TV_MAP   tv_map_logical(uint64_t logical, TV_METADATA *meta);

TV_SCHED *tv_sched_init(TV_DISK *disks, int ndisks, TV_METADATA *meta);
int       tv_write(TV_SCHED *sched, const void *buf, uint64_t len);
int       tv_read(TV_SCHED *sched, void *buf, uint64_t len, uint64_t offset);
int       tv_flush(TV_SCHED *sched);
void      tv_sched_reset(TV_SCHED *sched);
void      tv_sched_destroy(TV_SCHED *sched);

int  tv_metadata_save(TV_METADATA *meta, const char *path);
int  tv_metadata_load(TV_METADATA *meta, const char *path);

int  tv_benchmark(const char *disk_path, uint64_t *speed_out, int warmup);

int  tv_uring_init(struct io_uring *ring, int queue_depth);
int  tv_uring_write(struct io_uring *ring, int fd, const void *buf, size_t len, off_t offset, void *user_data);
int  tv_uring_read(struct io_uring *ring, int fd, void *buf, size_t len, off_t offset);
int  tv_uring_submit(struct io_uring *ring);
int  tv_uring_wait(struct io_uring *ring);
void tv_uring_destroy(struct io_uring *ring);

#endif
