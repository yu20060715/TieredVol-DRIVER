// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/bio.h>
#include <linux/slab.h>
#include <linux/device-mapper.h>
#include "tieredvol.h"

/* ---- Parallel stripe-split completion ---- */

void tv_parallel_end_io(struct bio *bio)
{
	struct tv_parallel_sub *ps = bio->bi_private;
	struct tv_parallel_block *block = ps->block;
	struct tieredvol_ctx *ctx = block->ctx;
	u64 lat;

	atomic_sub(ps->size, &ctx->io.in_flight_bytes[ps->disk_id]);
	atomic64_inc(&ctx->io.interval_completions[ps->disk_id]);

	lat = tv_ts_complete(ps->disk_id, bio->bi_iter.bi_sector,
			     bio->bi_iter.bi_size);
	if (lat > 0) {
		atomic64_add(lat, &ctx->io.total_latency_ns[ps->disk_id]);
		atomic64_inc(&ctx->io.total_completions[ps->disk_id]);
	}

	if (bio->bi_status != BLK_STS_OK)
		block->orig_bio->bi_status = bio->bi_status;

	bio_put(bio);

	if (atomic_dec_and_test(&block->pending)) {
		del_timer_sync(&block->timer);
		if (!atomic_read(&block->timed_out))
			bio_endio(block->orig_bio);
		kfree(block);
	}
}
EXPORT_SYMBOL_GPL(tv_parallel_end_io);

void tv_parallel_timeout(struct timer_list *t)
{
	struct tv_parallel_block *block = from_timer(block, t, timer);
	struct tieredvol_ctx *ctx = block->ctx;
	int i;

	if (atomic_read(&block->pending) == 0)
		return;

	pr_warn("tieredvol: parallel submit timeout (%d pending), degrading disks\n",
		atomic_read(&block->pending));

	for (i = 0; i < block->n_sub; i++) {
		int d = block->subs[i].disk_id;

		if (d >= 0 && d < ctx->ndisks && !ctx->deg.degraded[d]) {
			ctx->deg.degraded[d] = true;
			pr_warn("tieredvol: disk[%d] DEGRADED by parallel timeout\n",
				d);
		}
	}

	atomic_set(&block->timed_out, 1);
	smp_mb();
	bio_io_error(block->orig_bio);
}
EXPORT_SYMBOL_GPL(tv_parallel_timeout);

/* ---- Stripe-split helpers ---- */

void tv_stripe_calc_boundaries(struct tieredvol_segment *seg,
			       u32 chunk_size,
			       u64 logical, u64 b_sz,
			       struct tv_stripe_ctx *sc)
{
	u64 cumul = 0;
	int i;

	sc->s_sz = seg->stripe_size;
	sc->s_off = (logical - seg->logical_begin) % sc->s_sz;
	sc->b_end = sc->s_off + b_sz;
	sc->n_seg = (int)seg->disk_count;

	for (i = 0; i < sc->n_seg; i++) {
		cumul += (u64)seg->weight[i] * chunk_size;
		sc->disk_end[i] = cumul;
	}

	sc->fi = -1;
	sc->li = -1;

	if (sc->b_end <= sc->s_sz) {
		u64 prev = 0;
		for (i = 0; i < sc->n_seg; i++) {
			if (sc->s_off < sc->disk_end[i] && sc->b_end > prev) {
				if (sc->fi < 0) sc->fi = i;
				sc->li = i;
			}
			prev = sc->disk_end[i];
		}
	} else {
		u64 prev = 0;
		for (i = 0; i < sc->n_seg; i++) {
			if (sc->s_off < sc->disk_end[i]) {
				if (sc->fi < 0) sc->fi = i;
				sc->li = i;
				break;
			}
			prev = sc->disk_end[i];
		}
	}
}
EXPORT_SYMBOL_GPL(tv_stripe_calc_boundaries);

int tv_stripe_compute_ranges(struct tv_stripe_ctx *sc,
			     struct tieredvol_segment *seg,
			     u64 logical, u32 chunk_size,
			     u64 *d_start, u64 *d_sz, int *d_id)
{
	int n_sub = 0;
	int i;

	for (i = sc->fi; i <= sc->li; i++) {
		u64 c_beg = (i == 0) ? 0 : sc->disk_end[i - 1];
		u64 c_end = sc->disk_end[i];
		u64 ps = max(sc->s_off, c_beg);
		u64 pe = min(sc->b_end, c_end);
		if (pe <= ps) continue;
		u64 stripe_no = (logical - seg->logical_begin) / sc->s_sz;
		u64 dchunk = (u64)seg->weight[i] * chunk_size;
		d_start[n_sub] = stripe_no * dchunk + (ps - c_beg);
		d_sz[n_sub] = pe - ps;
		d_id[n_sub] = (int)seg->disk_index[i];
		n_sub++;
	}
	return n_sub;
}
EXPORT_SYMBOL_GPL(tv_stripe_compute_ranges);

int tv_parallel_submit(struct tieredvol_ctx *ctx, struct bio *bio,
		       int n_sub, u64 *d_start, u64 *d_sz, int *d_id)
{
	struct tv_parallel_block *block;
	struct bio *clones[TV_MAX_DISKS];
	int ci;

	block = kmalloc(sizeof(*block) + n_sub * sizeof(block->subs[0]),
			GFP_NOIO);
	if (!block) return -ENOMEM;

	for (ci = 0; ci < n_sub; ci++) {
		int d = d_id[ci];
		u64 adv = 0;
		int j;

		clones[ci] = bio_alloc_clone(ctx->devs[d]->bdev, bio,
					     GFP_NOIO, &fs_bio_set);
		if (!clones[ci]) {
			while (--ci >= 0) bio_put(clones[ci]);
			kfree(block);
			return -ENOMEM;
		}
		for (j = 0; j < ci; j++) adv += d_sz[j];
		if (adv > 0) bio_advance(clones[ci], adv);
		bio_set_dev(clones[ci], ctx->devs[d]->bdev);
		clones[ci]->bi_iter.bi_sector = d_start[ci] >> TV_SECTOR_SHIFT;
		clones[ci]->bi_iter.bi_size = d_sz[ci];
	}

	atomic_set(&block->pending, n_sub);
	atomic_set(&block->timed_out, 0);
	block->orig_bio = bio;
	block->ctx = ctx;
	block->n_sub = n_sub;

	timer_setup(&block->timer, tv_parallel_timeout, 0);
	mod_timer(&block->timer, jiffies + TV_PARALLEL_TIMEOUT);

	for (ci = 0; ci < n_sub; ci++) {
		int d = d_id[ci];
		block->subs[ci].block = block;
		block->subs[ci].disk_id = d;
		block->subs[ci].size = d_sz[ci];
		clones[ci]->bi_private = &block->subs[ci];
		clones[ci]->bi_end_io = tv_parallel_end_io;
		atomic_add(d_sz[ci], &ctx->io.in_flight_bytes[d]);
		tv_ts_submit(d, clones[ci]->bi_iter.bi_sector, d_sz[ci]);
		atomic64_add(d_sz[ci], &ctx->io.total_write_bytes[d]);
		atomic64_inc(&ctx->io.total_write_ops[d]);
		submit_bio(clones[ci]);
	}
	return 0;
}
EXPORT_SYMBOL_GPL(tv_parallel_submit);
