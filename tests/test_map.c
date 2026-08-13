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
/*  TEST: weight-borrowing (aligned redirect + per-chunk table)        */
/* ================================================================== */

#define BTEST_CHUNK 1048576U

struct tb_entry {
	bool valid;
	int dst;
	uint64_t sector;
};

static struct tb_entry tb_entries[64];
static uint32_t tb_used[4];
static const uint64_t tb_base[4] = {
	0x100000000ULL, 0x200000000ULL, 0x300000000ULL, 0x400000000ULL
};

/* Userspace model of tv_borrow_redirect: chunk-aligned, all-or-none. */
static bool tb_redirect(int src, uint64_t logical, uint64_t length,
			int *dst, uint64_t *sec)
{
	uint64_t c = logical / BTEST_CHUNK;
	uint32_t n = (uint32_t)(length / BTEST_CHUNK);
	uint32_t need = 0;
	int d;
	uint32_t i;

	if (length == 0 || length % BTEST_CHUNK != 0)
		return false;
	if (logical % BTEST_CHUNK != 0)
		return false;
	if (c + n > 64)
		return false;

	for (i = 0; i < n; i++)
		if (!tb_entries[c + i].valid)
			need++;

	if (need == 0) {
		*dst = tb_entries[c].dst;
		*sec = tb_entries[c].sector;
		return true;
	}

	d = (src == 0) ? 1 : 0;
	if (tb_used[d] + need > 16)
		return false; /* borrow area full → keep original disk */

	for (i = 0; i < n; i++) {
		if (tb_entries[c + i].valid)
			continue;
		tb_entries[c + i].valid = true;
		tb_entries[c + i].dst = d;
		tb_entries[c + i].sector =
			tb_base[d] + ((uint64_t)tb_used[d] * 2);
		tb_used[d]++;
	}
	*dst = d;
	*sec = tb_base[d] + ((uint64_t)(tb_used[d] - need) * 2);
	return true;
}

static bool tb_lookup(uint64_t logical, int *dst, uint64_t *sec)
{
	uint64_t c = logical / BTEST_CHUNK;

	if (c >= 64 || !tb_entries[c].valid)
		return false;
	*dst = tb_entries[c].dst;
	*sec = tb_entries[c].sector;
	return true;
}

static void test_borrow_alignment(void) {
	printf("\n[TEST] borrow: chunk-alignment gates redirect\n");
	int dst;
	uint64_t sec;

	/* Unaligned offset: no borrow, keep original placement */
	memset(tb_entries, 0, sizeof(tb_entries));
	memset(tb_used, 0, sizeof(tb_used));
	check(!tb_redirect(3, BTEST_CHUNK / 2, BTEST_CHUNK, &dst, &sec),
	      "mid-chunk offset → no borrow");
	check(!tb_redirect(3, 0, BTEST_CHUNK + BTEST_CHUNK / 2, &dst, &sec),
	      "non-chunk-multiple length → no borrow");
	check(tb_redirect(3, BTEST_CHUNK * 2, BTEST_CHUNK, &dst, &sec),
	      "aligned 1-chunk write → borrow OK");
	check(dst != 3, "borrowed chunk moved off the slow disk");
}

static void test_borrow_lookup_reuse(void) {
	printf("\n[TEST] borrow: lookup resolves same dst, re-write reuses slot\n");
	int dst;
	uint64_t sec, sec2;

	memset(tb_entries, 0, sizeof(tb_entries));
	memset(tb_used, 0, sizeof(tb_used));
	check(tb_redirect(3, 0, BTEST_CHUNK, &dst, &sec),
	      "first write borrows chunk 0");
	check(dst != 3, "chunk 0 on borrowed disk");
	check(tb_lookup(0, &dst, &sec2), "lookup finds chunk 0");
	check(dst != 3 && sec2 == sec, "lookup returns recorded destination");

	/* Re-write of the same chunk must reuse the slot, not grow used[] */
	{
		uint32_t used_before = tb_used[dst];

		check(tb_redirect(3, 0, BTEST_CHUNK, &dst, &sec),
		      "re-write of borrowed chunk resolves");
		check(tb_used[dst] == used_before,
		      "re-write does not allocate a new slot");
	}
}

static void test_borrow_alloc_none(void) {
	printf("\n[TEST] borrow: all-or-none across a multi-chunk range\n");
	int dst;
	uint64_t sec;

	memset(tb_entries, 0, sizeof(tb_entries));
	memset(tb_used, 0, sizeof(tb_used));

	/* Fill the borrow area for disk 0 fully (16 slots). */
	{
		int i;

		for (i = 0; i < 16; i++)
			check(tb_redirect(1, (uint64_t)i * BTEST_CHUNK,
					  BTEST_CHUNK, &dst, &sec),
			      "prefill slot");
	}
	/* src=1 (borrows into 0) is now full: a 2-chunk request must fail
	 * atomically and leave the table untouched. */
	{
		uint64_t c = 100;
		uint32_t before = tb_used[0];

		check(!tb_redirect(1, c * BTEST_CHUNK, 2 * BTEST_CHUNK,
				   &dst, &sec),
		      "no space → all-or-none reject");
		check(tb_used[0] == before, "no partial allocation on reject");
		check(!tb_lookup(c * BTEST_CHUNK, &dst, &sec),
		      "rejected chunks not recorded");
	}
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
