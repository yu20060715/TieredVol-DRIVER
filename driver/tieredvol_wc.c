// SPDX-License-Identifier: GPL-2.0-only
/*
 * tieredvol_wc.c — Write-coalescing buffer + flush (Phase 1 C).
 *
 * Extracted from tieredvol_core.c in Phase 1 refactoring.
 */
#include <linux/module.h>
#include <linux/bio.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/device-mapper.h>
#include "tieredvol.h"

bool wc_enabled = true;
module_param(wc_enabled, bool, 0644);
MODULE_PARM_DESC(wc_enabled, "Enable write coalescing (Phase 1 C)");

/* ---- Workqueue wrapper ---- */

static void tv_wc_flush_work_fn(struct work_struct *work)
{
	struct tieredvol_ctx *ctx = container_of(work, struct tieredvol_ctx,
						 wc.flush_work.work);
	tv_wc_flush(ctx);
}

int tv_wc_init_ctx(struct tieredvol_ctx *ctx)
{
	spin_lock_init(&ctx->wc.lock);
	INIT_LIST_HEAD(&ctx->wc.entries);
	ctx->wc.accumulated = 0;
	ctx->wc.seg_idx = -1;
	INIT_DELAYED_WORK(&ctx->wc.flush_work, tv_wc_flush_work_fn);
	return 0;
}

void tv_wc_destroy_ctx(struct tieredvol_ctx *ctx)
{
	struct tv_wc_entry *e, *tmp;
	unsigned long flags;

	cancel_delayed_work_sync(&ctx->wc.flush_work);
	tv_wc_flush(ctx);
	spin_lock_irqsave(&ctx->wc.lock, flags);
	list_for_each_entry_safe(e, tmp, &ctx->wc.entries, list) {
		list_del(&e->list);
		bio_io_error(e->bio);
		kfree(e);
	}
	spin_unlock_irqrestore(&ctx->wc.lock, flags);
}

/* ---- Flush: drain buffered WC entries ---- */

void tv_wc_flush(struct tieredvol_ctx *ctx)
{
	LIST_HEAD(batch);
	struct tv_wc_entry *entry, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&ctx->wc.lock, flags);
	if (list_empty(&ctx->wc.entries)) {
		spin_unlock_irqrestore(&ctx->wc.lock, flags);
		return;
	}
	list_splice_init(&ctx->wc.entries, &batch);
	ctx->wc.accumulated = 0;
	ctx->wc.seg_idx = -1;
	spin_unlock_irqrestore(&ctx->wc.lock, flags);

	list_for_each_entry_safe(entry, tmp, &batch, list) {
		struct bio *bio = entry->bio;
		u64 logical = entry->logical;
		struct tieredvol_map cur = entry->map;
		struct tieredvol_segment *seg;

		list_del(&entry->list);
		kfree(entry);

		if (cur.disk < 0 || cur.disk >= ctx->ndisks) {
			bio_io_error(bio);
			continue;
		}
		if (cur.seg_idx < 0 ||
		    cur.seg_idx >= (int)ctx->meta.segment_count) {
			bio_io_error(bio);
			continue;
		}

		seg = &ctx->meta.segments[cur.seg_idx];

		/* Check for cross-disk boundary (B logic) */
		if (seg->disk_count > 1) {
			struct tv_stripe_ctx sc;
			u64 b_sz = bio->bi_iter.bi_size;

			tv_stripe_calc_boundaries(seg, ctx->meta.chunk_size,
						  logical, b_sz, &sc);

			if (sc.fi >= 0 && sc.li >= 0 &&
			    sc.li - sc.fi + 1 > 1) {
				u64 d_start[TV_MAX_DISKS];
				u64 d_sz[TV_MAX_DISKS];
				int d_id[TV_MAX_DISKS];
				int n_sub = tv_stripe_compute_ranges(
					&sc, seg, logical,
					ctx->meta.chunk_size,
					d_start, d_sz, d_id);

				if (n_sub > 1) {
					tv_mirror_handle(ctx, bio, cur,
							 logical);
					if (tv_parallel_submit(ctx, bio,
							       n_sub,
							       d_start, d_sz,
							       d_id) == 0)
						continue;
				}
			}
		}

		tv_mirror_handle(ctx, bio, cur, logical);

		/* Direct single-disk submit */
		bio_set_dev(bio, ctx->devs[cur.disk]->bdev);
		bio->bi_iter.bi_sector = cur.offset >> TV_SECTOR_SHIFT;
		atomic_add(bio->bi_iter.bi_size,
			   &ctx->io.in_flight_bytes[cur.disk]);
		atomic64_add(bio->bi_iter.bi_size,
			     &ctx->io.total_write_bytes[cur.disk]);
		atomic64_inc(&ctx->io.total_write_ops[cur.disk]);
		submit_bio(bio);
	}
}
EXPORT_SYMBOL_GPL(tv_wc_flush);

/* ---- Try to buffer a write bio into the WC ---- */

int tv_wc_try_buffer(struct tieredvol_ctx *ctx, struct bio *bio,
		     u64 logical, struct tieredvol_map cur)
{
	struct tieredvol_segment *seg;
	sector_t stripe_no;
	unsigned long flags;

	if (!wc_enabled || bio_data_dir(bio) != WRITE ||
	    cur.seg_idx < 0 ||
	    cur.seg_idx >= (int)ctx->meta.segment_count)
		return -EAGAIN;

	seg = &ctx->meta.segments[cur.seg_idx];
	if (seg->disk_count <= 1)
		return -EAGAIN;

	/* Small writes bypass WC: with a shallow I/O queue the accumulated
	 * bytes can never reach stripe_size, so the 1-jiffy delayed flush
	 * serializes every bio and tanks throughput (e.g. 4K -> ~13 MiB/s
	 * vs 525 MiB/s direct). WC only helps large writes that actually
	 * fill a stripe, so let bio < chunk_size submit directly. */
	if (bio->bi_iter.bi_size < ctx->meta.chunk_size)
		return -EAGAIN;

	stripe_no = (logical - seg->logical_begin) / seg->stripe_size;

	/* Split at stripe boundary so each entry fits within one stripe */
	{
		u64 s_off = (logical - seg->logical_begin) % seg->stripe_size;
		u64 b_sz = bio->bi_iter.bi_size;
		u64 stripe_end = seg->stripe_size;

		if (s_off + b_sz > stripe_end) {
			u64 remain = stripe_end - s_off;
			sector_t remain_sect = remain >> TV_SECTOR_SHIFT;
			if (remain_sect > 0 && bio_sectors(bio) > remain_sect)
				dm_accept_partial_bio(bio, remain_sect);
		}
	}

	spin_lock_irqsave(&ctx->wc.lock, flags);

	if (!list_empty(&ctx->wc.entries) &&
	    (ctx->wc.seg_idx != cur.seg_idx ||
	     stripe_no != ctx->wc.stripe_start)) {
		spin_unlock_irqrestore(&ctx->wc.lock, flags);
		tv_wc_flush(ctx);
		spin_lock_irqsave(&ctx->wc.lock, flags);
	}

	{
		struct tv_wc_entry *e = kmalloc(sizeof(*e), GFP_ATOMIC);
		if (e) {
			e->bio = bio;
			e->map = cur;
			e->logical = logical;
			list_add_tail(&e->list, &ctx->wc.entries);
			ctx->wc.accumulated += bio->bi_iter.bi_size;
			ctx->wc.stripe_start = stripe_no;
			ctx->wc.seg_idx = cur.seg_idx;

			if (ctx->wc.accumulated >= seg->stripe_size) {
				cancel_delayed_work(&ctx->wc.flush_work);
				spin_unlock_irqrestore(&ctx->wc.lock, flags);
				tv_wc_flush(ctx);
			} else {
				mod_delayed_work(system_wq, &ctx->wc.flush_work, 1);
				spin_unlock_irqrestore(&ctx->wc.lock, flags);
			}
			return DM_MAPIO_SUBMITTED;
		}
	}
	spin_unlock_irqrestore(&ctx->wc.lock, flags);
	return -EAGAIN;
}
EXPORT_SYMBOL_GPL(tv_wc_try_buffer);
