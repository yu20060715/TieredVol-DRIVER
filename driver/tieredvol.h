#ifndef TIEREDVOL_H
#define TIEREDVOL_H

#include <linux/types.h>
#include <linux/device-mapper.h>
#include <linux/atomic.h>
#include <linux/rcupdate.h>
#include <linux/ktime.h>
#include <linux/timer.h>
#include <linux/bio.h>
#include <linux/completion.h>
#include <linux/workqueue.h>
#include <linux/kref.h>
#include "tieredvol_meta_format.h"

/* Aliases: canonical constants live in tieredvol_meta_format.h */
#define TV_MAX_DISKS    TV_META_MAX_DISKS
#define TV_MAX_SEGS     TV_META_MAX_SEGS
#define TV_SECTOR_SHIFT 9
/* Per-CPU ring entries. Kept at 512 because alloc_percpu() rejects a single
 * allocation > PCPU_MIN_UNIT_SIZE (32KB); 1024 read entries + head/count
 * would be 32776B > 32768B. 512 doubles the headroom over the measured
 * 256-in-flight saturation point (iodepth=32 x 8 stripe sub-writes).
 */
#define TV_PENDING_RING_SIZE 512

	struct tieredvol_segment {
	u64 logical_begin;
	u64 logical_end;
	u32 disk_count;
	u32 disk_index[TV_MAX_DISKS];
	u32 weight[TV_MAX_DISKS];
	u64 stripe_size;
	bool mirror_enabled;
	u32 mirror_disk;
	int policy; /* -1 = inherit from ctx, 0 = static, 2 = random */
};

struct tieredvol_metadata {
	u32 version;
	u32 chunk_size;
	u32 segment_count;
	u32 disk_count;
	char disk_names[TV_MAX_DISKS][64];
	struct tieredvol_segment segments[TV_MAX_SEGS];
	/* Runtime defaults persisted in [runtime] section */
	int runtime_policy;
	int runtime_borrow_enable; /* -1 = unset, 0 = off, 1 = on */
	u32 runtime_borrow_watermark_kb;
	u32 runtime_borrow_area_mb[TV_MAX_DISKS];
	char badmap_ranges[TV_MAX_DISKS][256];
};

struct tieredvol_map {
	int disk;
	int seg_idx;
	u64 offset;
	u64 length;
};

enum tv_policy {
	TV_POLICY_STATIC = 0,
	TV_POLICY_RANDOM = 2,
};

/* Phase 2: Sub-structs for tieredvol_ctx */

struct tv_io_stats {
	atomic_t in_flight_bytes[TV_MAX_DISKS];
	atomic64_t total_write_bytes[TV_MAX_DISKS];
	atomic64_t total_read_bytes[TV_MAX_DISKS];
	atomic64_t total_write_ops[TV_MAX_DISKS];
	atomic64_t total_read_ops[TV_MAX_DISKS];
};

/* ---- Weight-borrowing state ----
 * Deterministic static striping stays authoritative for placement; a slow
 * disk's (borrower) chunk is temporarily redirected to the least-loaded
 * disk's over-provisioned borrow area. Every redirected chunk is recorded
 * in a persistent per-block table so reads and later writes resolve the
 * same destination (unlike the removed dynamic policy, which had no
 * record and silently misplaced data).
 */
struct tv_borrow_entry {
	u8  valid;
	u8  dst_disk;
	u64 block;      /* 全域 borrow-block index = (logical - seg->begin)/block_size */
	u64 dst_sector; /* borrow 區內 sector 位址 */
};

struct tv_borrow_state {
	bool enabled;
	u32 block_size;                 /* borrow 粒度（byte）= chunk_size/8 */
	u32 watermark_bytes;            /* 觸發借出：src in_flight > 此值 */
	u32 area_blocks[TV_MAX_DISKS];  /* 每碟借用區 block 數 */
	u32 used_blocks[TV_MAX_DISKS];  /* 每碟已用借用 slot */
	u64 area_base_sector[TV_MAX_DISKS]; /* 借用區起點 sector */
	struct tv_borrow_entry *entries;    /* n_blocks 陣列（index = borrow block） */
	u64 n_blocks;
	u64 n_borrowed;
	atomic64_t borrow_write_bytes[TV_MAX_DISKS];
	spinlock_t lock;
};

struct tv_mirror_stats {
	atomic64_t mirror_write_bytes;
	atomic64_t mirror_write_ops;
	atomic64_t mirror_read_ops;
	atomic64_t mirror_errors;
};

struct tv_rebuild_state {
	struct task_struct *thread;
	int seg_idx;
	u64 offset;
	u64 total;
	atomic_t running;
	struct completion done_r;
	struct completion done_w;
};

struct tv_degradation {
	atomic_t *error_count;
	u32 error_threshold;
	bool degraded[TV_MAX_DISKS];
};

struct tieredvol_badmap {
	unsigned long *bitmap;
	u64 n_chunks;
};

struct tv_bench_stats {
	ktime_t start_time;
};

struct tv_pending_read_cpu;
struct tv_pending_write_cpu;

struct tieredvol_ctx {
	struct dm_target *ti;
	struct tieredvol_metadata meta;
	struct dm_dev **devs;
	sector_t *disk_sectors;
	char config_path[256];
	int ndisks;
	sector_t min_chunk_sectors;
	sector_t stripe_sectors;
	struct tv_io_stats io;
	struct tv_degradation deg;
	struct tieredvol_badmap badmaps[TV_MAX_DISKS];
	struct tv_borrow_state borrow;
	int policy;
	struct tv_mirror_stats mirror;
	struct tv_rebuild_state rebuild;
	struct {
		spinlock_t lock;
		struct list_head entries;
		sector_t stripe_start;
		int seg_idx;
		u64 accumulated;
		struct delayed_work flush_work;
	} wc;
	bool mirror_enabled_any;
	struct work_struct trigger_event;
	mempool_t *mirror_pw_pool;
	mempool_t *retry_ctx_pool;
	struct tv_pending_read_cpu __percpu *pcpu_reads;
	struct tv_pending_write_cpu __percpu *pcpu_writes;
	struct tv_bench_stats bench[TV_MAX_DISKS];
};

/* ---- tieredvol_map.c exports ---- */
struct tieredvol_map tv_map_logical(u64 logical,
				    struct tieredvol_metadata *meta,
				    u32 chunk_size);
struct tieredvol_map tv_map_logical_random(u64 logical,
					  struct tieredvol_metadata *meta,
					  u32 chunk_size);

/* ---- tieredvol_borrow.c exports ---- */
int  tv_borrow_init(struct tieredvol_ctx *ctx);
void tv_borrow_destroy(struct tieredvol_ctx *ctx);
bool tv_borrow_lookup(struct tieredvol_ctx *ctx, u64 logical,
		      int *disk, u64 *sector);
bool tv_borrow_redirect(struct tieredvol_ctx *ctx, int src_disk,
			u64 logical, u64 length,
			int *disk, u64 *sector);
int tv_borrow_save(struct tieredvol_ctx *ctx);
int tv_borrow_load(struct tieredvol_ctx *ctx, const char *path,
		   u64 n_blocks);

/* ---- tieredvol_meta.c exports ---- */
int tv_metadata_load_kernel(struct tieredvol_metadata *meta,
			    const char *path);
int tv_metadata_save_kernel(struct tieredvol_ctx *ctx);

/* ---- tieredvol_log.c exports ---- */
void tv_log(u8 level, u8 disk_idx, u8 event_type, const char *fmt, ...);

#define TV_LOG_SIZE 512

struct tv_log_entry {
	u64  timestamp_ns;
	u8   level;
	u8   disk_idx;
	u8   event_type;
	char msg[48];
};

enum tv_log_level {
	TV_LOG_OFF  = 0,
	TV_LOG_ERR  = 1,
	TV_LOG_WARN = 2,
	TV_LOG_INFO = 3,
};

enum tv_log_event {
	TV_LOG_IO      = 0,
	TV_LOG_STALE   = 1,
	TV_LOG_RECOVER = 2,
	TV_LOG_MIRROR  = 3,
	TV_LOG_CONFIG  = 4,
};

extern struct kfifo tv_log_fifo;
extern raw_spinlock_t tv_log_lock;

extern u8 tv_log_level;
extern unsigned int log_size;

/* ---- Pending-read tracking (per-CPU, lockless) ---- */
struct tv_pending_read_entry {
	struct block_device *bdev;
	sector_t sector;
	sector_t mirror_sector;
	unsigned int size;
	int mirror_disk;
};

struct tv_pending_read_cpu {
	struct tv_pending_read_entry entries[TV_PENDING_RING_SIZE];
	unsigned int head;
	unsigned int count;
};

/* ---- Pending-write tracking (per-CPU, spinlock-protected) ----
 * The write ring is written by tv_pw_add (submit path, this_cpu) and scanned
 * by tv_pw_remove (completion softirq) / tv_pw_is_pending (read-retry work);
 * those can run on the same CPU concurrently, so every access takes the
 * global tv_pending_lock.  Unlocked per-CPU access races and leaves stale
 * entries, which makes read-retry see a phantom pending write forever -> EIO.
 * The pending-read ring (tv_pending_add / tv_pending_find_and_remove) shares
 * the same lock for the same reason.
 */
struct tv_pending_write_entry {
	struct block_device *bdev;
	sector_t sector;
	unsigned int size;
};

struct tv_pending_write_cpu {
	struct tv_pending_write_entry entries[TV_PENDING_RING_SIZE];
	unsigned int head;
	unsigned int count;
};

extern spinlock_t tv_pending_lock;

/* ---- tieredvol_mirror.c exports ---- */
int tv_mirror_init_ctx(struct tieredvol_ctx *ctx);
void tv_mirror_destroy_ctx(struct tieredvol_ctx *ctx);
struct tv_mirror_pw_ctx {
	struct tieredvol_ctx *ctx;
	struct block_device *bdev;
	sector_t sector;
	unsigned int size;
};
void tv_pw_add(struct tieredvol_ctx *ctx, struct block_device *bdev,
	       sector_t sector, unsigned int size);
void tv_pending_add(struct tieredvol_ctx *ctx, struct block_device *bdev,
		    sector_t sector, unsigned int size, int mirror_disk,
		    sector_t mirror_sector);
int tv_pending_find_and_remove(struct tieredvol_ctx *ctx,
			       struct block_device *bdev, sector_t sector,
			       unsigned int size,
			       sector_t *mirror_sector_out);
void tv_mirror_handle(struct tieredvol_ctx *ctx, struct bio *bio,
		       struct tieredvol_map cur, u64 logical);
void tv_mirror_end_io(struct bio *bio);
void tv_read_retry_work(struct work_struct *work);
int tieredvol_end_io(struct dm_target *ti, struct bio *bio,
		     blk_status_t *error);

struct tv_retry_ctx {
	struct delayed_work dwork;
	struct tieredvol_ctx *ctx;
	struct bio *orig_bio;
	sector_t sector;
	unsigned int size;
	int mirror_disk;
	int retries;
};

int tv_rebuild_thread(void *data);

/* ---- Stripe-split helpers (shared by B path + C path) ---- */
struct tv_stripe_ctx {
	int fi, li;
	int n_seg;
	u64 disk_end[TV_MAX_DISKS];
	u64 s_sz;
	u64 s_off;
	u64 b_end;
};
void tv_stripe_calc_boundaries(struct tieredvol_segment *seg,
			       u32 chunk_size,
			       u64 logical, u64 b_sz,
			       struct tv_stripe_ctx *sc);
int tv_stripe_compute_ranges(struct tv_stripe_ctx *sc,
			     struct tieredvol_segment *seg,
			     u64 logical, u32 chunk_size,
			     u64 *d_start, u64 *d_sz, int *d_id);

/* ---- B: Parallel multi-disk write support ---- */
#define TV_PARALLEL_TIMEOUT (30 * HZ)
struct tv_parallel_block;
struct tv_parallel_sub {
	struct tv_parallel_block *block;
	int disk_id;
	unsigned int size;
};
struct tv_parallel_block {
	atomic_t pending;
	struct bio *orig_bio;
	struct tieredvol_ctx *ctx;
	int n_sub;
	atomic_t completed;
	atomic_t err_status;
	struct kref kref;
	struct timer_list timer;
	struct tv_parallel_sub subs[];
};
void tv_parallel_end_io(struct bio *bio);
void tv_parallel_timeout(struct timer_list *t);
int tv_parallel_submit(struct tieredvol_ctx *ctx, struct bio *bio,
		       int n_sub, u64 *d_start, u64 *d_sz, int *d_id);

/* ---- C: Write coalescing support ---- */
extern bool wc_enabled;
struct tv_wc_entry {
    struct list_head list;
    struct bio *bio;
    struct tieredvol_map map;
    u64 logical;
};
int tv_wc_init_ctx(struct tieredvol_ctx *ctx);
void tv_wc_destroy_ctx(struct tieredvol_ctx *ctx);
void tv_wc_flush(struct tieredvol_ctx *ctx);
int tv_wc_try_buffer(struct tieredvol_ctx *ctx, struct bio *bio,
		     u64 logical, struct tieredvol_map cur);


/* ---- tieredvol_sysfs.c exports ---- */
void tv_sysfs_init(void);
void tv_sysfs_exit(void);

/* ---- tieredvol_badmap.c exports ---- */
void tv_badmap_init(struct tieredvol_ctx *ctx);
void tv_badmap_destroy(struct tieredvol_ctx *ctx);
bool tv_badmap_test(struct tieredvol_ctx *ctx, int disk, u64 chunk_no);
void tv_badmap_set(struct tieredvol_ctx *ctx, int disk, u64 chunk_no);
void tv_badmap_clear(struct tieredvol_ctx *ctx, int disk, u64 chunk_no);

/* ---- Badmap rebuild ---- */
void tv_badmap_rebuild(struct tieredvol_ctx *ctx);

/* ---- tieredvol_message.c exports ---- */
int tieredvol_message(struct dm_target *ti, unsigned int argc,
		      char **argv, char *result, unsigned int maxlen);

/* ---- Global active context (RCU-protected) ---- */
extern struct tieredvol_ctx __rcu *tv_active_ctx;

#endif
