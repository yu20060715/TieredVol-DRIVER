/*
 * test_stripe_kernel.c — Feasibility prototype: compile REAL kernel driver
 * source (driver/tieredvol_stripe.c) into a userspace test.
 *
 * Mock kernel headers live in tests/mock/linux/. Only the type shapes and
 * the small set of APIs tieredvol_stripe.c touches are mocked; the driver
 * code itself is unmodified (this file #includes it verbatim).
 *
 * This validates that pure-math helpers (tv_stripe_calc_boundaries,
 * tv_stripe_compute_ranges) can be exercised in userspace, and that the
 * completion/timeout handoff can be driven deterministically via the
 * synchronous bio_endio mock.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "test_common.h"

struct bio_set fs_bio_set;
unsigned long jiffies = 1000;
struct bio *mock_last_submitted;

#include "../driver/tieredvol_stripe.c"

static void test_boundaries_single(void) {
	printf("\n[TEST] stripe(kernel): single-disk boundaries\n");
	struct tieredvol_segment seg = {
		.logical_begin = 0,
		.logical_end = 0x40000000ULL,
		.disk_count = 1,
		.weight = {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
		.disk_index = {2, 0},
		.stripe_size = 1048576,
	};
	struct tv_stripe_ctx sc;

	tv_stripe_calc_boundaries(&seg, 1048576, 0, 4096, &sc);
	check(sc.s_sz == 1048576, "stripe_size copied");
	check(sc.s_off == 0, "offset within stripe");
	check(sc.b_end == 4096, "end within stripe");
	check(sc.fi == 0 && sc.li == 0, "single disk spans the range");
	check(sc.disk_end[0] == 1048576, "disk 0 weight*chunk");
}

static void test_boundaries_multi_weighted(void) {
	printf("\n[TEST] stripe(kernel): weighted multi-disk boundaries\n");
	/* weights 1:2:1 over a 1 MB chunk */
	struct tieredvol_segment seg = {
		.logical_begin = 0,
		.logical_end = 0x40000000ULL,
		.disk_count = 3,
		.weight = {1, 2, 1, 0},
		.disk_index = {0, 1, 3},
		.stripe_size = 4 * 1048576,
	};
	struct tv_stripe_ctx sc;
	u64 d_start[8], d_sz[8];
	int d_id[8];
	int n;

	tv_stripe_calc_boundaries(&seg, 1048576, 1048576, 1048576, &sc);
	/* disk ends: 1M, 3M, 4M within a 4M stripe */
	check(sc.disk_end[0] == 1048576, "disk0 end 1M");
	check(sc.disk_end[1] == 3 * 1048576, "disk1 end 3M");
	check(sc.disk_end[2] == 4 * 1048576, "disk2 end 4M");

	/* Range 1M..2M starts exactly at the disk0/disk1 boundary, so it
	 * lands entirely on disk1 (s_off == disk_end[0] is exclusive). */
	n = tv_stripe_compute_ranges(&sc, &seg, 1048576, 1048576,
				     d_start, d_sz, d_id);
	check(n == 1, "range at boundary lands on one disk");
	check(d_id[0] == 1, "routes to disk_index[1]");
	check(d_start[0] == 0, "disk1 piece starts at stripe-0 chunk 0");
	check(d_sz[0] == 1048576, "full chunk size");

	/* Range 2M..3M lies wholly within disk1's share (1M..3M). */
	tv_stripe_calc_boundaries(&seg, 1048576, 2 * 1048576, 1048576, &sc);
	n = tv_stripe_compute_ranges(&sc, &seg, 2 * 1048576, 1048576,
				     d_start, d_sz, d_id);
	check(n == 1, "range within disk1 is one sub");
	check(d_id[0] == 1, "routes to disk1");
	check(d_start[0] == 1048576, "offset within disk1 share");
	check(d_sz[0] == 1048576, "disk1 piece 1M");
}

static void test_boundaries_across_stripe(void) {
	printf("\n[TEST] stripe(kernel): range crossing a stripe boundary\n");
	struct tieredvol_segment seg = {
		.logical_begin = 0,
		.logical_end = 0x40000000ULL,
		.disk_count = 2,
		.weight = {1, 1, 0},
		.disk_index = {0, 2},
		.stripe_size = 2 * 1048576,
	};
	struct tv_stripe_ctx sc;

	/* logical = 2M + 1536K: s_off = 1536K (within stripe 1), and the
	 * range runs 1M past the stripe end. */
	tv_stripe_calc_boundaries(&seg, 1048576, 2 * 1048576 + 1536 * 1024,
				  1024 * 1024, &sc);
	check(sc.s_off == 1536 * 1024, "offset within stripe");
	check(sc.b_end > sc.s_sz, "range crosses stripe end");
	check(sc.fi == 1 && sc.li == 1, "crosses into second stripe's disks");
}

static void test_parallel_timeout(void) {
	printf("\n[TEST] stripe(kernel): parallel timeout degrades disks\n");
	struct tieredvol_ctx ctx;
	struct dm_dev devs[2];
	struct dm_dev *devs_ptr[2];
	struct bio *bio;
	struct tv_parallel_block *block;
	int rc;

	memset(&ctx, 0, sizeof(ctx));
	ctx.ndisks = 2;
	memset(devs, 0, sizeof(devs));
	devs_ptr[0] = &devs[0];
	devs_ptr[1] = &devs[1];
	ctx.devs = devs_ptr;
	memset(ctx.deg.degraded, 0, sizeof(ctx.deg.degraded));

	bio = bio_alloc_clone(NULL, &(struct bio){ .bi_iter.bi_size = 131072 },
			      0, &fs_bio_set);
	assert(bio);

	{
		u64 ds[2] = {0, 0};
		u64 sz[2] = {65536, 65536};
		int ids[2] = {0, 1};

		rc = tv_parallel_submit(&ctx, bio, 2, ds, sz, ids);
		check(rc == 0, "parallel submit OK");
		block = ((struct tv_parallel_sub *)mock_last_submitted->bi_private)->block;
		check(block->n_sub == 2, "two subs recorded");
		check(ctx.deg.degraded[0] == false &&
		      ctx.deg.degraded[1] == false,
		      "no degradation before timeout");
	}

	/* Simulate the timeout callback firing. */
	tv_parallel_timeout(&block->timer);
	check(ctx.deg.degraded[0] == true && ctx.deg.degraded[1] == true,
	      "timeout degrades both disks");
	check(atomic_read(&block->completed) == 1,
	      "timeout completed the orig bio");
}

static void test_parallel_complete(void) {
	printf("\n[TEST] stripe(kernel): all subs complete before timeout\n");
	struct tieredvol_ctx ctx;
	struct dm_dev devs[2];
	struct dm_dev *devs_ptr[2];
	struct bio *bio;
	struct tv_parallel_block *block;
	int rc;

	memset(&ctx, 0, sizeof(ctx));
	ctx.ndisks = 2;
	memset(devs, 0, sizeof(devs));
	devs_ptr[0] = &devs[0];
	devs_ptr[1] = &devs[1];
	ctx.devs = devs_ptr;
	memset(ctx.deg.degraded, 0, sizeof(ctx.deg.degraded));

	bio = bio_alloc_clone(NULL, &(struct bio){ .bi_iter.bi_size = 131072 },
			      0, &fs_bio_set);
	assert(bio);
	bio->bi_status = 0;

	{
		u64 ds[2] = {0, 0};
		u64 sz[2] = {65536, 65536};
		int ids[2] = {0, 1};

		rc = tv_parallel_submit(&ctx, bio, 2, ds, sz, ids);
		check(rc == 0, "parallel submit OK");
		block = ((struct tv_parallel_sub *)mock_last_submitted->bi_private)->block;
	}

	/* Complete both clones; the mock calls bi_end_io synchronously and
	 * frees each clone inside tv_parallel_end_io (bio_put). */
	{
		int i;

		for (i = 0; i < block->n_sub; i++) {
			struct bio *clone = bio_alloc_clone(NULL, bio, 0,
							   &fs_bio_set);

			clone->bi_private = &block->subs[i];
			clone->bi_status = 0;
			tv_parallel_end_io(clone);
		}
	}
	check(bio->bi_status == 0, "orig bio completed OK");
	check(atomic_read(&ctx.io.in_flight_bytes[0]) == 0 &&
	      atomic_read(&ctx.io.in_flight_bytes[1]) == 0,
	      "in-flight counters back to zero");
}

int main(void) {
	printf("=== TieredVol Kernel-Source Userspace Tests (stripe) ===\n");

	test_boundaries_single();
	test_boundaries_multi_weighted();
	test_boundaries_across_stripe();
	test_parallel_timeout();
	test_parallel_complete();

	printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
	return tests_passed == tests_run ? 0 : 1;
}
