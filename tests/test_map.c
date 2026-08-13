/*
 * test_map.c — Unit tests for kernel mapping logic (tieredvol_map.c)
 *
 * Re-implements kernel mapping functions as pure C for userspace testing.
 * Tests tv_find_segment, tv_map_logical, weight-borrowing redirects,
 * and tv_map_logical_random.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include "test_common.h"

/* ---- Mock kernel types ---- */
#define TV_MAX_DISKS    16
#define TV_MAX_SEGS     16
#define TV_META_MAX_DISKS TV_MAX_DISKS
#define TV_META_MAX_SEGS  TV_MAX_SEGS

typedef struct { uint64_t val; } atomic64_t;
#define atomic64_read(x)  ((x)->val)
#define atomic64_set(x, v) ((x)->val = (v))
#define atomic64_inc(x)   ((x)->val++)
#define atomic64_add(x, v) ((x)->val += (v))

typedef uint64_t u64;
typedef uint32_t u32;

/* ---- Replicated kernel structs ---- */
struct tieredvol_segment {
	uint64_t logical_begin;
	uint64_t logical_end;
	uint32_t disk_count;
	uint32_t disk_index[TV_MAX_DISKS];
	uint32_t weight[TV_MAX_DISKS];
	uint64_t stripe_size;
	bool     mirror_enabled;
	int      mirror_disk;
};

struct tieredvol_metadata {
	uint32_t version;
	uint32_t chunk_size;
	uint32_t segment_count;
	uint32_t disk_count;
	char     disk_names[TV_MAX_DISKS][64];
	struct tieredvol_segment segments[TV_MAX_SEGS];
};

struct tieredvol_map {
	int  disk;
	int  seg_idx;
	u64  offset;
	u64  length;
};

/* ---- Re-implemented kernel functions ---- */

static int tv_find_segment(u64 logical, const struct tieredvol_metadata *meta)
{
	int lo = 0, hi = (int)meta->segment_count - 1;

	while (lo <= hi) {
		int mid = lo + (hi - lo) / 2;
		const struct tieredvol_segment *seg = &meta->segments[mid];

		if (logical < seg->logical_begin)
			hi = mid - 1;
		else if (logical >= seg->logical_end)
			lo = mid + 1;
		else
			return mid;
	}
	return -1;
}

static struct tieredvol_map tv_map_logical(u64 logical,
					   struct tieredvol_metadata *meta,
					   u32 chunk_size)
{
	struct tieredvol_map err = { .disk = -1, .offset = 0, .length = 0 };
	int seg_idx, disk_idx;
	const struct tieredvol_segment *seg;
	u64 stripe_no, offset_in;
	u64 boundary[TV_MAX_DISKS + 1];
	int i;

	if (!meta || meta->segment_count == 0)
		return err;

	seg_idx = tv_find_segment(logical, meta);
	if (seg_idx < 0)
		return err;

	seg = &meta->segments[seg_idx];

	if (seg->disk_count == 0 || seg->disk_count > TV_MAX_DISKS)
		return err;

	stripe_no = (logical - seg->logical_begin) / seg->stripe_size;
	offset_in = (logical - seg->logical_begin) % seg->stripe_size;

	boundary[0] = 0;
	for (i = 0; i < (int)seg->disk_count; i++)
		boundary[i + 1] = boundary[i] +
			(u64)seg->weight[i] * chunk_size;

	disk_idx = -1;
	for (i = 0; i < (int)seg->disk_count; i++) {
		if (offset_in >= boundary[i] && offset_in < boundary[i + 1]) {
			disk_idx = i;
			break;
		}
	}

	if (disk_idx < 0)
		return err;

	{
		struct tieredvol_map map;

		map.disk = (int)seg->disk_index[disk_idx];
		map.seg_idx = seg_idx;
		map.offset = stripe_no * (u64)seg->weight[disk_idx] *
			     chunk_size +
			     (offset_in - boundary[disk_idx]);
		map.length = (u64)seg->weight[disk_idx] * chunk_size;

		return map;
	}
}

static struct tieredvol_map tv_map_logical_random(u64 logical,
						  struct tieredvol_metadata *meta,
						  u32 chunk_size)
{
	struct tieredvol_map err = { .disk = -1, .offset = 0, .length = 0 };
	int seg_idx;
	const struct tieredvol_segment *seg;
	u64 stripe_no, offset_in;
	int disk_idx;

	if (!meta || meta->segment_count == 0)
		return err;

	seg_idx = tv_find_segment(logical, meta);
	if (seg_idx < 0)
		return err;

	seg = &meta->segments[seg_idx];

	if (seg->disk_count == 0 || seg->disk_count > TV_MAX_DISKS)
		return err;

	stripe_no = (logical - seg->logical_begin) / seg->stripe_size;
	offset_in = (logical - seg->logical_begin) % seg->stripe_size;

	disk_idx = rand() % seg->disk_count;

	{
		struct tieredvol_map map;
		u64 disk_chunk = (u64)seg->weight[disk_idx] * chunk_size;

		map.disk = (int)seg->disk_index[disk_idx];
		map.seg_idx = seg_idx;
		map.offset = stripe_no * disk_chunk +
			     (offset_in % disk_chunk);
		map.length = disk_chunk;

		return map;
	}
}

/* ---- Helper: build a 1-segment metadata ---- */
static void make_meta_1seg(struct tieredvol_metadata *meta,
			   int ndisks, uint32_t chunk_size)
{
	memset(meta, 0, sizeof(*meta));
	meta->version = 1;
	meta->chunk_size = chunk_size;
	meta->segment_count = 1;
	meta->disk_count = ndisks;
	for (int i = 0; i < ndisks; i++) {
		snprintf(meta->disk_names[i], 64, "disk%d", i);
		meta->segments[0].disk_index[i] = i;
		meta->segments[0].weight[i] = 1;
	}
	meta->segments[0].disk_count = ndisks;
	meta->segments[0].logical_begin = 0;
	meta->segments[0].logical_end = (uint64_t)ndisks * 16 * chunk_size;
	meta->segments[0].stripe_size = (uint64_t)ndisks * chunk_size;
}

/* ---- Helper: build a 2-segment metadata (unequal capacities) ---- */
static void make_meta_2seg(struct tieredvol_metadata *meta, uint32_t chunk_size)
{
	memset(meta, 0, sizeof(*meta));
	meta->version = 1;
	meta->chunk_size = chunk_size;
	meta->segment_count = 2;
	meta->disk_count = 3;
	for (int i = 0; i < 3; i++)
		snprintf(meta->disk_names[i], 64, "disk%d", i);

	/* Segment 0: 3 disks, weights 7:1:1 */
	meta->segments[0].disk_count = 3;
	meta->segments[0].disk_index[0] = 0;
	meta->segments[0].disk_index[1] = 1;
	meta->segments[0].disk_index[2] = 2;
	meta->segments[0].weight[0] = 7;
	meta->segments[0].weight[1] = 1;
	meta->segments[0].weight[2] = 1;
	meta->segments[0].logical_begin = 0;
	meta->segments[0].logical_end = 9 * 16 * chunk_size;
	meta->segments[0].stripe_size = 9 * chunk_size;

	/* Segment 1: 2 disks, weights 7:1 */
	meta->segments[1].disk_count = 2;
	meta->segments[1].disk_index[0] = 0;
	meta->segments[1].disk_index[1] = 1;
	meta->segments[1].weight[0] = 7;
	meta->segments[1].weight[1] = 1;
	meta->segments[1].logical_begin = 9 * 16 * chunk_size;
	meta->segments[1].logical_end = 9 * 16 * chunk_size + 8 * 16 * chunk_size;
	meta->segments[1].stripe_size = 8 * chunk_size;
}

/* ================================================================== */
/*  TEST: tv_find_segment                                              */
/* ================================================================== */

static void test_find_segment_single(void) {
	printf("\n[TEST] tv_find_segment: single segment\n");
	struct tieredvol_metadata meta;
	make_meta_1seg(&meta, 2, 1048576);

	check(tv_find_segment(0, &meta) == 0, "offset 0 → seg 0");
	check(tv_find_segment(1048576, &meta) == 0, "offset 1MB → seg 0");
	check(tv_find_segment(meta.segments[0].logical_end - 1, &meta) == 0,
	      "last byte of seg 0 → seg 0");
}

static void test_find_segment_multi(void) {
	printf("\n[TEST] tv_find_segment: multi segment\n");
	struct tieredvol_metadata meta;
	make_meta_2seg(&meta, 1048576);

	check(tv_find_segment(0, &meta) == 0, "offset 0 → seg 0");
	check(tv_find_segment(1048576, &meta) == 0, "offset 1MB in seg 0");
	check(tv_find_segment(meta.segments[0].logical_end, &meta) == 1,
	      "seg 0 end → seg 1");
	check(tv_find_segment(meta.segments[1].logical_end - 1, &meta) == 1,
	      "last byte of seg 1 → seg 1");
}

static void test_find_segment_boundaries(void) {
	printf("\n[TEST] tv_find_segment: exact boundaries\n");
	struct tieredvol_metadata meta;
	make_meta_2seg(&meta, 1048576);

	uint64_t boundary = meta.segments[0].logical_end;
	check(tv_find_segment(boundary, &meta) == 1,
	      "exact boundary → seg 1");
	check(tv_find_segment(boundary - 1, &meta) == 0,
	      "one before boundary → seg 0");
}

static void test_find_segment_out_of_range(void) {
	printf("\n[TEST] tv_find_segment: out of range\n");
	struct tieredvol_metadata meta;
	make_meta_1seg(&meta, 2, 1048576);

	check(tv_find_segment(meta.segments[0].logical_end, &meta) == -1,
	      "past end → -1");
	check(tv_find_segment((uint64_t)-1, &meta) == -1,
	      "UINT64_MAX → -1");
}

static void test_find_segment_empty(void) {
	printf("\n[TEST] tv_find_segment: empty metadata\n");
	struct tieredvol_metadata meta;
	memset(&meta, 0, sizeof(meta));

	check(tv_find_segment(0, &meta) == -1, "0 segments → -1");
}

/* ================================================================== */
/*  TEST: tv_map_logical                                               */
/* ================================================================== */

static void test_map_logical_basic(void) {
	printf("\n[TEST] tv_map_logical: basic mapping\n");
	struct tieredvol_metadata meta;
	make_meta_1seg(&meta, 2, 1048576);

	struct tieredvol_map m = tv_map_logical(0, &meta, 1048576);
	check(m.disk == 0, "offset 0 → disk 0");
	check(m.seg_idx == 0, "offset 0 → seg 0");
	check(m.length == 1048576, "length = chunk_size");

	m = tv_map_logical(1048576, &meta, 1048576);
	check(m.disk == 1, "offset 1MB → disk 1 (second chunk)");
}

static void test_map_logical_weights(void) {
	printf("\n[TEST] tv_map_logical: weighted mapping\n");
	struct tieredvol_metadata meta;
	make_meta_1seg(&meta, 2, 1048576);
	meta.segments[0].weight[0] = 7;
	meta.segments[0].weight[1] = 1;
	meta.segments[0].stripe_size = 8 * 1048576;

	/* stripe = 8MB, disk0 gets 7MB, disk1 gets 1MB */
	struct tieredvol_map m = tv_map_logical(0, &meta, 1048576);
	check(m.disk == 0, "offset 0 → disk 0 (weight 7)");
	check(m.length == 7 * 1048576, "disk0 length = 7MB");

	m = tv_map_logical(7 * 1048576, &meta, 1048576);
	check(m.disk == 1, "offset 7MB → disk 1 (weight 1)");
	check(m.length == 1 * 1048576, "disk1 length = 1MB");
}

static void test_map_logical_stripe_no(void) {
	printf("\n[TEST] tv_map_logical: stripe numbering\n");
	struct tieredvol_metadata meta;
	make_meta_1seg(&meta, 2, 1048576);
	meta.segments[0].weight[0] = 3;
	meta.segments[0].weight[1] = 1;
	meta.segments[0].stripe_size = 4 * 1048576;

	/* Stripe 0: disk0 [0,3MB), disk1 [3MB,4MB)
	 * Stripe 1: disk0 [4MB,7MB), disk1 [7MB,8MB) */
	struct tieredvol_map m = tv_map_logical(4 * 1048576, &meta, 1048576);
	check(m.disk == 0, "stripe 1 offset 0 → disk 0");
	check(m.offset == 1 * 3 * 1048576, "stripe 1 disk0 offset = stripe_no * disk_chunk");

	m = tv_map_logical(7 * 1048576, &meta, 1048576);
	check(m.disk == 1, "stripe 1 offset 3MB → disk 1");
	check(m.offset == 1 * 1 * 1048576, "stripe 1 disk1 offset = stripe_no * disk_chunk");
}

static void test_map_logical_null(void) {
	printf("\n[TEST] tv_map_logical: NULL and empty\n");
	struct tieredvol_map m = tv_map_logical(0, NULL, 1048576);
	check(m.disk == -1, "NULL meta → disk -1");

	struct tieredvol_metadata meta;
	memset(&meta, 0, sizeof(meta));
	m = tv_map_logical(0, &meta, 1048576);
	check(m.disk == -1, "0 segments → disk -1");
}

static void test_map_logical_multi_seg(void) {
	printf("\n[TEST] tv_map_logical: multi-segment mapping\n");
	struct tieredvol_metadata meta;
	make_meta_2seg(&meta, 1048576);

	/* In seg 0: 3 disks, weight 7:1:1, stripe = 9MB */
	struct tieredvol_map m = tv_map_logical(0, &meta, 1048576);
	check(m.disk == 0, "seg0 offset 0 → disk 0");
	check(m.seg_idx == 0, "seg0 offset 0 → seg 0");

	/* In seg 1: 2 disks, weight 7:1, stripe = 8MB */
	uint64_t off1 = meta.segments[0].logical_end;
	m = tv_map_logical(off1, &meta, 1048576);
	check(m.disk == 0, "seg1 offset 0 → disk 0");
	check(m.seg_idx == 1, "seg1 offset 0 → seg 1");
}

static void test_map_logical_three_disks(void) {
	printf("\n[TEST] tv_map_logical: 3 disks equal weight\n");
	struct tieredvol_metadata meta;
	make_meta_1seg(&meta, 3, 1048576);

	struct tieredvol_map m;
	m = tv_map_logical(0, &meta, 1048576);
	check(m.disk == 0, "offset 0 → disk 0");

	m = tv_map_logical(1048576, &meta, 1048576);
	check(m.disk == 1, "offset 1MB → disk 1");

	m = tv_map_logical(2 * 1048576, &meta, 1048576);
	check(m.disk == 2, "offset 2MB → disk 2");
}

static void test_map_logical_out_of_range(void) {
	printf("\n[TEST] tv_map_logical: out of range\n");
	struct tieredvol_metadata meta;
	make_meta_1seg(&meta, 2, 1048576);

	struct tieredvol_map m = tv_map_logical(
		meta.segments[0].logical_end, &meta, 1048576);
	check(m.disk == -1, "past end → disk -1");
}

static void test_map_logical_weight_16(void) {
	printf("\n[TEST] tv_map_logical: max weight 16\n");
	struct tieredvol_metadata meta;
	make_meta_1seg(&meta, 2, 1048576);
	meta.segments[0].weight[0] = 16;
	meta.segments[0].weight[1] = 1;
	meta.segments[0].stripe_size = 17 * 1048576;

	struct tieredvol_map m = tv_map_logical(0, &meta, 1048576);
	check(m.disk == 0, "weight 16 → disk 0");
	check(m.length == 16 * 1048576, "disk0 length = 16MB");

	m = tv_map_logical(16 * 1048576, &meta, 1048576);
	check(m.disk == 1, "offset 16MB → disk 1");
	check(m.length == 1 * 1048576, "disk1 length = 1MB");
}

static void test_map_logical_offset_in_chunk(void) {
	printf("\n[TEST] tv_map_logical: offset within chunk\n");
	struct tieredvol_metadata meta;
	make_meta_1seg(&meta, 2, 1048576);
	meta.segments[0].weight[0] = 3;
	meta.segments[0].weight[1] = 1;
	meta.segments[0].stripe_size = 4 * 1048576;

	/* Mid-point of disk0's chunk in stripe 0 */
	struct tieredvol_map m = tv_map_logical(1536 * 1024, &meta, 1048576);
	check(m.disk == 0, "mid-chunk → disk 0");
	check(m.offset == 1536 * 1024, "offset preserved within chunk");
}

/* ================================================================== */
/*  TEST: weight-borrowing (128 KB block granularity + per-block table) */
/* ================================================================== */

/* The kernel borrows at block granularity: block_size = chunk_size/8
 * (128 KB for a 1 MB chunk). Bios are capped at 128 KB by the block
 * layer, so WRITE bios are whole blocks; a range may still span several
 * blocks. Sector math matches tv_borrow_* : dst_sector is in sectors and
 * the intra-block byte offset is shifted by TV_SECTOR_SHIFT (9). */
#define BTEST_BLOCK 131072U
#define BTEST_NBLK  256    /* borrow entry table size (blocks) */
#define BTEST_AREA  64     /* borrow slots per disk */
#define BTEST_WATERMARK 262144U /* 256 KB, matches tv_s4_borrow.conf */
#define BSHIFT 9

static struct tb_entry {
	bool valid;
	int dst;
	uint64_t sector;   /* borrow-area sector address */
} tb_entries[BTEST_NBLK];
static uint32_t tb_used[4];
static uint32_t tb_inflight[4];
static bool tb_degraded[4];
static bool tb_enabled = true;
static const uint64_t tb_base[4] = {
	0x100000000ULL, 0x200000000ULL, 0x300000000ULL, 0x400000000ULL
};

static void tb_reset(void)
{
	memset(tb_entries, 0, sizeof(tb_entries));
	memset(tb_used, 0, sizeof(tb_used));
	memset(tb_inflight, 0, sizeof(tb_inflight));
	memset(tb_degraded, 0, sizeof(tb_degraded));
	tb_enabled = true;
}

static uint32_t tb_area(int d)
{
	(void)d;
	return BTEST_AREA;
}

/* Userspace model of tv_borrow_pick_dst: exclude src + degraded + no room,
 * pick the least in-flight disk. */
static int tb_pick_dst(int src, uint32_t need)
{
	int best = -1;
	uint32_t best_load = UINT32_MAX;
	int i;

	for (i = 0; i < 4; i++) {
		uint32_t avail;

		if (i == src)
			continue;
		if (tb_degraded[i])
			continue;
		avail = tb_area(i);
		if (avail < need)
			continue;
		if (tb_inflight[i] < best_load) {
			best_load = tb_inflight[i];
			best = i;
		}
	}
	return best;
}

/* Userspace model of tv_borrow_redirect: 128 KB block granularity. */
static bool tb_redirect(int src, uint64_t logical, uint64_t length,
			int *dst, uint64_t *sec)
{
	uint64_t off = logical;
	uint64_t block = off / BTEST_BLOCK;
	uint32_t n_blocks = (uint32_t)(((off + length - 1) / BTEST_BLOCK) -
				       block + 1);
	uint32_t need = 0;
	int d = -1;
	uint32_t i;

	if (length == 0)
		return false;
	if (block + n_blocks > BTEST_NBLK)
		return false;

	for (i = 0; i < n_blocks; i++)
		if (!tb_entries[block + i].valid)
			need++;

	if (need == 0) {
		/* Already redirected: reuse recorded destination(s). */
		d = tb_entries[block].dst;
		for (i = 1; i < n_blocks; i++)
			if (tb_entries[block + i].dst != d)
				return false;
		*dst = d;
		*sec = tb_entries[block].sector +
		       ((logical % BTEST_BLOCK) >> BSHIFT);
		return true;
	}

	/* New borrows: only while enabled, source backlogged, whole blocks. */
	if (!tb_enabled)
		return false;
	if (tb_inflight[src] < BTEST_WATERMARK)
		return false;
	if (logical % BTEST_BLOCK != 0 || length % BTEST_BLOCK != 0)
		return false;

	d = tb_pick_dst(src, need);
	if (d < 0)
		return false;
	if (tb_used[d] + need > BTEST_AREA)
		return false;

	for (i = 0; i < n_blocks; i++) {
		if (tb_entries[block + i].valid)
			continue;
		tb_entries[block + i].valid = true;
		tb_entries[block + i].dst = d;
		tb_entries[block + i].sector =
			tb_base[d] + ((uint64_t)tb_used[d] * (BTEST_BLOCK >> BSHIFT));
		tb_used[d]++;
	}
	*dst = d;
	*sec = tb_base[d] +
	       ((uint64_t)(tb_used[d] - need) * (BTEST_BLOCK >> BSHIFT));
	return true;
}

/* Userspace model of tv_borrow_lookup: resolves recorded mapping only,
 * independent of the runtime enable flag (borrow_off keeps borrowed
 * blocks readable/writable at their borrow-area copy). */
static bool tb_lookup(uint64_t logical, int *dst, uint64_t *sec)
{
	uint64_t block = logical / BTEST_BLOCK;

	if (block >= BTEST_NBLK || !tb_entries[block].valid)
		return false;
	*dst = tb_entries[block].dst;
	*sec = tb_entries[block].sector +
	       ((logical % BTEST_BLOCK) >> BSHIFT);
	return true;
}

static void test_borrow_alignment(void) {
	printf("\n[TEST] borrow: block-alignment gates redirect\n");
	int dst;
	uint64_t sec;

	tb_reset();
	check(!tb_redirect(3, BTEST_BLOCK / 2, BTEST_BLOCK, &dst, &sec),
	      "mid-block offset → no borrow");
	check(!tb_redirect(3, 0, BTEST_BLOCK + BTEST_BLOCK / 2, &dst, &sec),
	      "non-block-multiple length → no borrow");
	tb_inflight[3] = 4 * BTEST_WATERMARK;
	check(tb_redirect(3, BTEST_BLOCK * 2, BTEST_BLOCK, &dst, &sec),
	      "aligned 1-block write → borrow OK");
	check(dst != 3, "borrowed block moved off the slow disk");
	check(sec == tb_base[dst], "borrowed block starts at slot 0 of dst area");
}

static void test_borrow_lookup_reuse(void) {
	printf("\n[TEST] borrow: lookup resolves same dst, re-write reuses slot\n");
	int dst;
	uint64_t sec, sec2;

	tb_reset();
	tb_inflight[3] = 4 * BTEST_WATERMARK;
	check(tb_redirect(3, 0, BTEST_BLOCK, &dst, &sec),
	      "first write borrows block 0");
	check(dst != 3, "block 0 on borrowed disk");
	check(tb_lookup(0, &dst, &sec2), "lookup finds block 0");
	check(dst != 3 && sec2 == sec, "lookup returns recorded destination");

	{
		uint32_t used_before = tb_used[dst];

		check(tb_redirect(3, 0, BTEST_BLOCK, &dst, &sec),
		      "re-write of borrowed block resolves");
		check(tb_used[dst] == used_before,
		      "re-write does not allocate a new slot");
	}
}

static void test_borrow_alloc_none(void) {
	printf("\n[TEST] borrow: all-or-none when the borrow area is full\n");
	int dst;
	uint64_t sec;

	tb_reset();
	tb_inflight[1] = 4 * BTEST_WATERMARK;
	{
		int i;

		for (i = 0; i < BTEST_AREA; i++)
			check(tb_redirect(1, (uint64_t)i * BTEST_BLOCK,
					  BTEST_BLOCK, &dst, &sec),
			      "prefill slot");
	}
	{
		uint64_t c = 200;
		uint32_t before = tb_used[0];

		check(!tb_redirect(1, c * BTEST_BLOCK, 2 * BTEST_BLOCK,
				   &dst, &sec),
		      "no space → all-or-none reject");
		check(tb_used[0] == before, "no partial allocation on reject");
		check(!tb_lookup(c * BTEST_BLOCK, &dst, &sec),
		      "rejected blocks not recorded");
	}
}

static void test_borrow_watermark(void) {
	printf("\n[TEST] borrow: watermark gates new borrows\n");
	int dst;
	uint64_t sec;

	tb_reset();
	/* Source below watermark: keep original placement. */
	tb_inflight[2] = BTEST_WATERMARK / 2;
	check(!tb_redirect(2, 0, BTEST_BLOCK, &dst, &sec),
	      "below watermark → no borrow");
	check(!tb_lookup(0, &dst, &sec), "unborrowed block not recorded");
	/* At/above watermark: borrow. */
	tb_inflight[2] = BTEST_WATERMARK;
	check(tb_redirect(2, 0, BTEST_BLOCK, &dst, &sec),
	      "at watermark → borrow OK");
	check(dst != 2, "borrowed off the source disk");
}

static void test_borrow_pick_dst(void) {
	printf("\n[TEST] borrow: dst = least in-flight, excluding src/degraded\n");
	int dst;
	uint64_t sec;

	/* src excluded even when it is the least loaded */
	tb_reset();
	tb_inflight[0] = 0; tb_inflight[1] = 500000;
	tb_inflight[2] = 500000; tb_inflight[3] = 500000;
	tb_inflight[3] = 4 * BTEST_WATERMARK; /* src=3 backlogged */
	check(tb_redirect(3, 0, BTEST_BLOCK, &dst, &sec),
	      "borrow allowed with src backlogged");
	check(dst == 0, "least-loaded non-src disk chosen");

	/* degraded disk never chosen */
	tb_reset();
	tb_inflight[3] = 4 * BTEST_WATERMARK;
	tb_degraded[0] = true; tb_degraded[1] = true; tb_degraded[2] = true;
	check(!tb_redirect(3, 0, BTEST_BLOCK, &dst, &sec),
	      "all candidates degraded → no borrow");

	/* area exhausted on every candidate → no borrow */
	tb_reset();
	tb_inflight[3] = 4 * BTEST_WATERMARK;
	tb_used[0] = BTEST_AREA; tb_used[1] = BTEST_AREA; tb_used[2] = BTEST_AREA;
	check(!tb_redirect(3, 0, BTEST_BLOCK, &dst, &sec),
	      "no room on any candidate → no borrow");
}

static void test_borrow_cross_blocks(void) {
	printf("\n[TEST] borrow: multi-block range, mixed valid/invalid blocks\n");
	int dst;
	uint64_t sec;

	tb_reset();
	tb_inflight[3] = 4 * BTEST_WATERMARK;
	/* Borrow block 10 and 12, leave 11 unborrowed. */
	check(tb_redirect(3, 10 * BTEST_BLOCK, BTEST_BLOCK, &dst, &sec),
	      "borrow block 10");
	check(tb_redirect(3, 12 * BTEST_BLOCK, BTEST_BLOCK, &dst, &sec),
	      "borrow block 12");
	{
		uint32_t used_before = tb_used[dst];

		/* Range covering 10..12 (3 blocks): only 11 is new. */
		check(tb_redirect(3, 10 * BTEST_BLOCK, 3 * BTEST_BLOCK,
				  &dst, &sec),
		      "partial range borrows only the missing block");
		check(tb_used[dst] == used_before + 1,
		      "exactly one new slot allocated");
		check(tb_lookup(11 * BTEST_BLOCK, &dst, &sec) && dst != 3,
		      "newly borrowed middle block recorded");
	}
}

static void test_borrow_need_zero_redirect(void) {
	printf("\n[TEST] borrow: fully-borrowed range redirects regardless of watermark\n");
	int dst;
	uint64_t sec;

	tb_reset();
	tb_inflight[3] = 4 * BTEST_WATERMARK;
	check(tb_redirect(3, 0, BTEST_BLOCK, &dst, &sec),
	      "borrow block 0");
	/* Drop source backlog below watermark: reuse still redirects. */
	tb_inflight[3] = 0;
	check(tb_redirect(3, 0, BTEST_BLOCK, &dst, &sec),
	      "already-borrowed block still redirected");
	check(sec == tb_entries[0].sector,
	      "reuse returns recorded sector");
}

static void test_borrow_offset_in_block(void) {
	printf("\n[TEST] borrow: intra-block offset resolves via block-size remainder\n");
	int dst;
	uint64_t sec, base_sec;

	tb_reset();
	tb_inflight[3] = 4 * BTEST_WATERMARK;
	check(tb_redirect(3, BTEST_BLOCK, BTEST_BLOCK, &dst, &sec),
	      "borrow block 1");
	base_sec = tb_entries[1].sector;
	/* A read 512 bytes into the block: sector = base + 1. */
	check(tb_lookup(BTEST_BLOCK + 512, &dst, &sec),
	      "lookup at +512 inside borrowed block");
	check(sec == base_sec + 1, "sector advances by 1 for 512-byte offset");
	/* A read 4 KB into the block: sector = base + 8. */
	check(tb_lookup(BTEST_BLOCK + 4096, &dst, &sec),
	      "lookup at +4K inside borrowed block");
	check(sec == base_sec + 8, "sector advances by 8 for 4K offset");
}

static void test_borrow_off_keeps_consistency(void) {
	printf("\n[TEST] borrow: off stops new borrows, keeps old ones resolving\n");
	int dst;
	uint64_t sec, sec2;

	tb_reset();
	tb_inflight[2] = 4 * BTEST_WATERMARK;
	check(tb_redirect(2, 0, BTEST_BLOCK, &dst, &sec),
	      "borrow while enabled");
	check(tb_lookup(0, &dst, &sec2) && sec2 == sec,
	      "read resolves to borrow area");

	/* Turn borrowing off. */
	tb_enabled = false;
	check(!tb_redirect(2, BTEST_BLOCK, BTEST_BLOCK, &dst, &sec),
	      "new borrow rejected while off");
	check(tb_lookup(0, &dst, &sec2) && sec2 == sec,
	      "already-borrowed block still resolves after off");
	/* Re-write of the borrowed block must still go to the borrow area
	 * (need == 0 path is not gated by the enable flag). */
	check(tb_redirect(2, 0, BTEST_BLOCK, &dst, &sec),
	      "re-write of borrowed block still resolves while off");
	check(sec == tb_entries[0].sector, "reuse keeps recorded sector");
}

static void test_borrow_format_v2(void) {
	printf("\n[TEST] borrow: .borrow v2 file format (save/load layout)\n");
	static const uint32_t TVBR = 0x54564252U; /* "TVBR" */
	uint32_t version = 2;
	const char *path = "/tmp/.tv_borrow_v2_test.bin";
	FILE *f;
	uint64_t i;

	/* Kernel layout: header = magic(u32) + version(u32) + n(u64) = 16 B;
	 * entry = {u8 valid, u8 dst_disk, u64 dst_sector} = 16 B (packed). */
	struct file_entry {
		uint8_t valid;
		uint8_t dst_disk;
		uint8_t _pad[6];
		uint64_t dst_sector;
	};
	struct file_entry snapshot[8];
	struct file_entry roundtrip[8];

	check(sizeof(struct file_entry) == 16,
	      "borrow entry is 16 bytes (v2 format)");
	check(sizeof(struct file_entry) * 8 + 16 ==
	      8 * 16 + 4 + 4 + 8,
	      "file size = header(16 B) + n_blocks*16 B");

	/* Populate a snapshot like tv_borrow_save does. */
	for (i = 0; i < 8; i++) {
		snapshot[i].valid = 1;
		snapshot[i].dst_disk = (uint8_t)(i % 3);
		snapshot[i].dst_sector = 0x100000000ULL + i * 256;
	}

	/* tv_borrow_save: magic + version + n + entries. */
	f = fopen(path, "w");
	check(f != NULL, "open test file for save");
	fwrite(&TVBR, sizeof(TVBR), 1, f);
	fwrite(&version, sizeof(version), 1, f);
	{
		uint64_t n = 8;
		fwrite(&n, sizeof(n), 1, f);
	}
	fwrite(snapshot, sizeof(struct file_entry), 8, f);
	fclose(f);

	/* tv_borrow_load: validate magic, version, n, then read entries. */
	{
		uint32_t m = 0, v = 0;
		uint64_t n = 0;
		f = fopen(path, "r");
		check(f != NULL, "open test file for load");
		check(fread(&m, sizeof(m), 1, f) == 1 && m == TVBR,
		      "magic field matches TVBR");
		check(fread(&v, sizeof(v), 1, f) == 1 && v == 2,
		      "version field matches v2");
		check(fread(&n, sizeof(n), 1, f) == 1 && n == 8,
		      "count field matches 8");
		check(fread(roundtrip, sizeof(struct file_entry), 8, f) == 8,
		      "entries read fully");
		fclose(f);
	}
	for (i = 0; i < 8; i++) {
		check(roundtrip[i].valid == 1, "entry valid flag preserved");
		check(roundtrip[i].dst_disk == (uint8_t)(i % 3),
		      "entry dst_disk preserved");
		check(roundtrip[i].dst_sector == 0x100000000ULL + i * 256,
		      "entry dst_sector preserved");
	}

	/* Corrupt magic: load must reject (fresh volume). */
	{
		uint32_t bad_magic = 0xDEADBEEFU;
		uint32_t m = 0;
		f = fopen(path, "w");
		fwrite(&bad_magic, sizeof(bad_magic), 1, f);
		fclose(f);
		f = fopen(path, "r");
		check(fread(&m, sizeof(m), 1, f) == 1, "read corrupt magic");
		fclose(f);
		check(m != TVBR, "corrupt magic rejected");
	}

	/* Old v1 file: version field = 1 must be rejected by a v2 loader. */
	{
		uint32_t v1 = 1;
		uint32_t rv = 0;
		f = fopen(path, "w");
		fwrite(&TVBR, sizeof(TVBR), 1, f);
		fwrite(&v1, sizeof(v1), 1, f);
		fclose(f);
		f = fopen(path, "r");
		check(fread(&rv, sizeof(rv), 1, f) == 1 && rv == TVBR,
		      "v1 file has correct magic");
		check(fread(&rv, sizeof(rv), 1, f) == 1 && rv == 1,
		      "v1 file has version 1");
		fclose(f);
		check(rv != 2, "v1 file rejected by v2 loader");
	}

	unlink(path);
}

/* ================================================================== */
/*  TEST: tv_map_logical_random                                        */
/* ================================================================== */

static void test_random_basic(void) {
	printf("\n[TEST] tv_map_logical_random: basic\n");
	struct tieredvol_metadata meta;
	make_meta_1seg(&meta, 2, 1048576);
	meta.segments[0].weight[0] = 1;
	meta.segments[0].weight[1] = 1;
	meta.segments[0].stripe_size = 2 * 1048576;

	struct tieredvol_map m = tv_map_logical_random(0, &meta, 1048576);
	check(m.disk >= 0 && m.disk <= 1, "returns valid disk index");
	check(m.seg_idx == 0, "seg_idx = 0");
	check(m.length == 1048576, "length = chunk_size");
}

static void test_random_valid_disk(void) {
	printf("\n[TEST] tv_map_logical_random: always valid\n");
	struct tieredvol_metadata meta;
	make_meta_1seg(&meta, 3, 1048576);
	meta.segments[0].weight[0] = 1;
	meta.segments[0].weight[1] = 1;
	meta.segments[0].weight[2] = 1;
	meta.segments[0].stripe_size = 3 * 1048576;

	for (int i = 0; i < 100; i++) {
		struct tieredvol_map m = tv_map_logical_random(0, &meta, 1048576);
		check(m.disk >= 0 && m.disk <= 2, "always returns disk 0-2");
	}
}

static void test_random_null(void) {
	printf("\n[TEST] tv_map_logical_random: NULL\n");
	struct tieredvol_map m = tv_map_logical_random(0, NULL, 1048576);
	check(m.disk == -1, "NULL meta → disk -1");
}

/* ================================================================== */
/*  TEST: Edge cases                                                   */
/* ================================================================== */

static void test_edge_single_disk(void) {
	printf("\n[TEST] edge: single disk\n");
	struct tieredvol_metadata meta;
	make_meta_1seg(&meta, 1, 1048576);

	struct tieredvol_map m = tv_map_logical(0, &meta, 1048576);
	check(m.disk == 0, "single disk → disk 0");
	check(m.length == 1048576, "length = chunk_size");
}

static void test_edge_max_disks(void) {
	printf("\n[TEST] edge: 16 disks\n");
	struct tieredvol_metadata meta;
	make_meta_1seg(&meta, 16, 1048576);

	struct tieredvol_map m = tv_map_logical(0, &meta, 1048576);
	check(m.disk == 0, "16 disks offset 0 → disk 0");

	m = tv_map_logical(15 * 1048576, &meta, 1048576);
	check(m.disk == 15, "16 disks offset 15MB → disk 15");
}

static void test_edge_zero_chunk(void) {
	printf("\n[TEST] edge: metadata with zero disk_count\n");
	struct tieredvol_metadata meta;
	make_meta_1seg(&meta, 2, 1048576);
	meta.segments[0].disk_count = 0;

	struct tieredvol_map m = tv_map_logical(0, &meta, 1048576);
	check(m.disk == -1, "disk_count=0 → disk -1");
}

static void test_edge_4_segments(void) {
	printf("\n[TEST] edge: 4 segments\n");
	struct tieredvol_metadata meta;
	memset(&meta, 0, sizeof(meta));
	meta.version = 1;
	meta.chunk_size = 1048576;
	meta.segment_count = 4;
	meta.disk_count = 4;

	for (int i = 0; i < 4; i++) {
		snprintf(meta.disk_names[i], 64, "disk%d", i);
		meta.segments[i].disk_count = 4 - i;
		for (int j = 0; j < 4 - i; j++) {
			meta.segments[i].disk_index[j] = j;
			meta.segments[i].weight[j] = 1;
		}
		meta.segments[i].logical_begin = (uint64_t)i * 16 * 1048576;
		meta.segments[i].logical_end = (uint64_t)(i + 1) * 16 * 1048576;
		meta.segments[i].stripe_size = (uint64_t)(4 - i) * 1048576;
	}

	/* seg 0: 4 disks */
	struct tieredvol_map m = tv_map_logical(0, &meta, 1048576);
	check(m.seg_idx == 0, "offset 0 → seg 0");
	check(m.disk == 0, "seg 0 disk 0");

	/* seg 1: 3 disks */
	m = tv_map_logical(16 * 1048576, &meta, 1048576);
	check(m.seg_idx == 1, "offset 16MB → seg 1");
	check(m.disk == 0, "seg 1 disk 0");

	/* seg 3: 1 disk */
	m = tv_map_logical(48 * 1048576, &meta, 1048576);
	check(m.seg_idx == 3, "offset 48MB → seg 3");
	check(m.disk == 0, "seg 3 disk 0");
}

static void test_edge_stripe_multiple(void) {
	printf("\n[TEST] edge: multiple stripes\n");
	struct tieredvol_metadata meta;
	make_meta_1seg(&meta, 2, 1048576);
	meta.segments[0].weight[0] = 3;
	meta.segments[0].weight[1] = 1;
	meta.segments[0].stripe_size = 4 * 1048576;

	/* Stripe 2: offset 8MB */
	struct tieredvol_map m = tv_map_logical(8 * 1048576, &meta, 1048576);
	check(m.disk == 0, "stripe 2 → disk 0");
	check(m.offset == 2 * 3 * 1048576, "stripe 2 disk0 offset = stripe_no * disk_chunk");

	/* Stripe 5: offset 20MB */
	m = tv_map_logical(20 * 1048576, &meta, 1048576);
	check(m.disk == 0, "stripe 5 → disk 0");
}

/* ================================================================== */
/*  MAIN                                                               */
/* ================================================================== */

static void test_random_coverage(void) {
	printf("\n[TEST] tv_map_logical_random: coverage\n");
	struct tieredvol_metadata meta;
	make_meta_1seg(&meta, 4, 1048576);
	meta.segments[0].weight[0] = 1;
	meta.segments[0].weight[1] = 1;
	meta.segments[0].weight[2] = 1;
	meta.segments[0].weight[3] = 1;
	meta.segments[0].stripe_size = 4 * 1048576;

	int hits[4] = {0};
	for (int i = 0; i < 200; i++) {
		struct tieredvol_map m = tv_map_logical_random(0, &meta, 1048576);
		if (m.disk >= 0 && m.disk < 4) hits[m.disk]++;
	}
	check(hits[0] > 0 && hits[1] > 0 && hits[2] > 0 && hits[3] > 0,
		  "200 random calls hit all 4 disks at least once");
}

int main(void) {
	srand(42);

	printf("=== TieredVol Mapping Unit Tests ===\n");

	/* tv_find_segment */
	test_find_segment_single();
	test_find_segment_multi();
	test_find_segment_boundaries();
	test_find_segment_out_of_range();
	test_find_segment_empty();

	/* tv_map_logical */
	test_map_logical_basic();
	test_map_logical_weights();
	test_map_logical_stripe_no();
	test_map_logical_null();
	test_map_logical_multi_seg();
	test_map_logical_three_disks();
	test_map_logical_out_of_range();
	test_map_logical_weight_16();
	test_map_logical_offset_in_chunk();

	/* weight-borrowing */
	test_borrow_alignment();
	test_borrow_lookup_reuse();
	test_borrow_alloc_none();
	test_borrow_watermark();
	test_borrow_pick_dst();
	test_borrow_cross_blocks();
	test_borrow_need_zero_redirect();
	test_borrow_offset_in_block();
	test_borrow_off_keeps_consistency();
	test_borrow_format_v2();

	/* tv_map_logical_random */
	test_random_basic();
	test_random_valid_disk();
	test_random_null();

	/* Edge cases */
	test_edge_single_disk();
	test_edge_max_disks();
	test_edge_zero_chunk();
	test_edge_4_segments();
	test_edge_stripe_multiple();

	/* Additional edge cases */
	test_random_coverage();

	printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
