// SPDX-License-Identifier: GPL-2.0-only
/*
 * tieredvol_mirror.c — Mirror I/O, pending read/write tracking,
 * read retry, rebuild thread, DM end_io handler.
 *
 * Extracted from tieredvol_core.c in Phase 1 refactoring.
 */
#include <linux/module.h>
#include <linux/bio.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/highmem.h>
#include <linux/device-mapper.h>
#include "tieredvol.h"

DEFINE_SPINLOCK(tv_pending_lock);
EXPORT_SYMBOL_GPL(tv_pending_lock);

int tv_mirror_init_ctx(struct tieredvol_ctx *ctx)
{
	ctx->pcpu_reads = alloc_percpu(struct tv_pending_read_cpu);
	if (!ctx->pcpu_reads)
		return -ENOMEM;
	ctx->pcpu_writes = alloc_percpu(struct tv_pending_write_cpu);
	if (!ctx->pcpu_writes) {
		free_percpu(ctx->pcpu_reads);
		return -ENOMEM;
	}

	ctx->mirror_pw_pool = mempool_create_kmalloc_pool(
		128, sizeof(struct tv_mirror_pw_ctx));
	if (!ctx->mirror_pw_pool) {
		free_percpu(ctx->pcpu_writes);
		free_percpu(ctx->pcpu_reads);
		return -ENOMEM;
	}
	ctx->retry_ctx_pool = mempool_create_kmalloc_pool(
		32, sizeof(struct tv_retry_ctx));
	if (!ctx->retry_ctx_pool) {
		mempool_destroy(ctx->mirror_pw_pool);
		free_percpu(ctx->pcpu_writes);
		free_percpu(ctx->pcpu_reads);
		return -ENOMEM;
	}
	return 0;
}

void tv_mirror_destroy_ctx(struct tieredvol_ctx *ctx)
{
	mempool_destroy(ctx->mirror_pw_pool);
	mempool_destroy(ctx->retry_ctx_pool);
	free_percpu(ctx->pcpu_writes);
	free_percpu(ctx->pcpu_reads);
}

/* ---- Pending-read tracking (per-CPU, lockless) ---- */

void tv_pending_add(struct tieredvol_ctx *ctx, struct block_device *bdev,
		    sector_t sector, unsigned int size, int mirror_disk,
		    sector_t mirror_sector)
{
	struct tv_pending_read_cpu *pcpu;
	unsigned long flags;
	unsigned int idx;

	spin_lock_irqsave(&tv_pending_lock, flags);
	pcpu = this_cpu_ptr(ctx->pcpu_reads);
	idx = (pcpu->head + pcpu->count) % TV_PENDING_RING_SIZE;
	if (pcpu->count < TV_PENDING_RING_SIZE) {
		pcpu->entries[idx].bdev = bdev;
		pcpu->entries[idx].sector = sector;
		pcpu->entries[idx].mirror_sector = mirror_sector;
		pcpu->entries[idx].size = size;
		pcpu->entries[idx].mirror_disk = mirror_disk;
		pcpu->count++;
	} else {
		pr_warn_once("tieredvol: per-cpu pending-read full, dropping entry\n");
	}
	spin_unlock_irqrestore(&tv_pending_lock, flags);
}
EXPORT_SYMBOL_GPL(tv_pending_add);

int tv_pending_find_and_remove(struct tieredvol_ctx *ctx,
			       struct block_device *bdev, sector_t sector,
			       unsigned int size, sector_t *mirror_sector_out)
{
	int mirror_disk = -1;
	unsigned long flags;
	int cpu;

	spin_lock_irqsave(&tv_pending_lock, flags);
	for_each_possible_cpu(cpu) {
		struct tv_pending_read_cpu *pcpu = per_cpu_ptr(ctx->pcpu_reads, cpu);
		unsigned int i;

		for (i = 0; i < pcpu->count; i++) {
			unsigned int idx = (pcpu->head + i) % TV_PENDING_RING_SIZE;
			struct tv_pending_read_entry *pr = &pcpu->entries[idx];

			if (pr->bdev == bdev && pr->sector == sector &&
			    pr->size == size) {
				unsigned int j;

				mirror_disk = pr->mirror_disk;
				if (mirror_sector_out)
					*mirror_sector_out = pr->mirror_sector;
				for (j = i; j + 1 < pcpu->count; j++) {
					unsigned int next =
						(pcpu->head + j + 1) % TV_PENDING_RING_SIZE;

					pcpu->entries[(pcpu->head + j) % TV_PENDING_RING_SIZE] =
						pcpu->entries[next];
				}
				pcpu->count--;
				goto out;
			}
		}
	}
out:
	spin_unlock_irqrestore(&tv_pending_lock, flags);
	return mirror_disk;
}
EXPORT_SYMBOL_GPL(tv_pending_find_and_remove);
/* ---- Pending-write tracking (per-CPU, lockless) ---- */

void tv_pw_add(struct tieredvol_ctx *ctx, struct block_device *bdev,
	       sector_t sector, unsigned int size)
{
	struct tv_pending_write_cpu *pcpu = this_cpu_ptr(ctx->pcpu_writes);
	unsigned long flags;
	unsigned int idx;

	spin_lock_irqsave(&tv_pending_lock, flags);
	idx = (pcpu->head + pcpu->count) % TV_PENDING_RING_SIZE;
	if (pcpu->count < TV_PENDING_RING_SIZE) {
		pcpu->entries[idx].bdev = bdev;
		pcpu->entries[idx].sector = sector;
		pcpu->entries[idx].size = size;
		pcpu->count++;
	} else {
		pr_warn_once("tieredvol: per-cpu pending-write full, dropping entry\n");
	}
	spin_unlock_irqrestore(&tv_pending_lock, flags);
}
EXPORT_SYMBOL_GPL(tv_pw_add);

static bool tv_pw_remove(struct tieredvol_ctx *ctx, struct block_device *bdev,
			 sector_t sector, unsigned int size)
{
	unsigned long flags;
	int cpu;
	bool found = false;

	spin_lock_irqsave(&tv_pending_lock, flags);
	for_each_possible_cpu(cpu) {
		struct tv_pending_write_cpu *pcpu = per_cpu_ptr(ctx->pcpu_writes, cpu);
		unsigned int i;

		for (i = 0; i < pcpu->count; i++) {
			unsigned int idx = (pcpu->head + i) % TV_PENDING_RING_SIZE;
			struct tv_pending_write_entry *pw = &pcpu->entries[idx];

			if (pw->bdev == bdev && pw->sector == sector &&
			    pw->size == size) {
				unsigned int j;

				for (j = i; j + 1 < pcpu->count; j++) {
					unsigned int next =
						(pcpu->head + j + 1) % TV_PENDING_RING_SIZE;

					pcpu->entries[(pcpu->head + j) % TV_PENDING_RING_SIZE] =
						pcpu->entries[next];
				}
				pcpu->count--;
				found = true;
				goto out;
			}
		}
	}
out:
	spin_unlock_irqrestore(&tv_pending_lock, flags);
	return found;
}

static bool tv_pw_is_pending(struct tieredvol_ctx *ctx,
			     struct block_device *bdev, sector_t sector,
			     unsigned int size)
{
	unsigned long flags;
	int cpu;
	bool pending = false;

	spin_lock_irqsave(&tv_pending_lock, flags);
	for_each_possible_cpu(cpu) {
		struct tv_pending_write_cpu *pcpu = per_cpu_ptr(ctx->pcpu_writes, cpu);
		unsigned int i;

		for (i = 0; i < pcpu->count; i++) {
			unsigned int idx = (pcpu->head + i) % TV_PENDING_RING_SIZE;
			struct tv_pending_write_entry *pw = &pcpu->entries[idx];

			if (pw->bdev == bdev && pw->sector == sector &&
			    pw->size == size) {
				pending = true;
				goto out;
			}
		}
	}
out:
	spin_unlock_irqrestore(&tv_pending_lock, flags);
	return pending;
}

/* ---- Mirror I/O completion ---- */

void tv_mirror_handle(struct tieredvol_ctx *ctx, struct bio *bio,
		       struct tieredvol_map cur, u64 logical)
{
	struct tieredvol_segment *seg;
	sector_t mirror_sec;

	if (cur.seg_idx < 0 ||
	    cur.seg_idx >= (int)ctx->meta.segment_count)
		return;

	seg = &ctx->meta.segments[cur.seg_idx];
	if (!seg->mirror_enabled ||
	    seg->mirror_disk >= (u32)ctx->ndisks ||
	    seg->mirror_disk == (u32)cur.disk)
		return;

	mirror_sec = (logical - seg->logical_begin) >> TV_SECTOR_SHIFT;

	if (bio_data_dir(bio) == WRITE) {
		struct bio *clone;
		struct tv_mirror_pw_ctx *pwc;
		struct bvec_iter it;
		struct bio_vec bvl;
		unsigned int bio_sz = bio->bi_iter.bi_size;
		unsigned int nsegs = 0;

		pwc = mempool_alloc(ctx->mirror_pw_pool, GFP_NOIO);
		if (!pwc) {
			atomic64_inc(&ctx->mirror.mirror_errors);
			tv_log(TV_LOG_ERR, cur.disk, TV_LOG_MIRROR,
			       "mirror pwc alloc fail seg%d", cur.seg_idx);
			return;
		}

		bio_for_each_segment(bvl, bio, it)
			nsegs++;

		/*
		 * Do NOT bio_alloc_clone() here: a clone shares orig's bvec
		 * array, but that array is owned by the submitting (DIO) bio,
		 * not by orig. The primary write completes on fast NVMe while
		 * this clone is still queued on the slow SATA mirror disk; by
		 * then the DIO bio has been freed and its bvec array recycled,
		 * so dispatching the clone walks freed/reused memory and hits a
		 * NULL deref in scsi_alloc_sgtables() (system freeze). Pin via
		 * get_page() is also wrong: fio reuses the same user buffer for
		 * the next I/O and overwrites the pages, corrupting mirror data.
		 * Instead, build a fully self-contained bio that COPIES the data
		 * into pages the mirror owns.
		 */
		clone = bio_alloc_bioset(ctx->devs[seg->mirror_disk]->bdev,
					 nsegs, REQ_OP_WRITE, GFP_NOIO,
					 &fs_bio_set);
		if (!clone) {
			mempool_free(pwc, ctx->mirror_pw_pool);
			atomic64_inc(&ctx->mirror.mirror_errors);
			tv_log(TV_LOG_ERR, cur.disk, TV_LOG_MIRROR,
			       "mirror alloc fail seg%d", cur.seg_idx);
			return;
		}

		it = bio->bi_iter;
		bio_for_each_segment(bvl, bio, it) {
			struct page *pg = alloc_page(GFP_NOIO);
			void *src, *dst;

			if (!pg)
				goto fail_copy;

			src = kmap_local_page(bvl.bv_page) + bvl.bv_offset;
			dst = kmap_local_page(pg);
			memcpy(dst, src, bvl.bv_len);
			kunmap_local(dst);
			kunmap_local(src);

			if (bio_add_page(clone, pg, bvl.bv_len, 0) !=
			    bvl.bv_len) {
				__free_page(pg);
				goto fail_copy;
			}
		}

		clone->bi_iter.bi_sector = mirror_sec;
		pwc->ctx = ctx;
		pwc->bdev = ctx->devs[seg->mirror_disk]->bdev;
		pwc->sector = mirror_sec;
		pwc->size = bio_sz;
		clone->bi_private = pwc;
		clone->bi_end_io = tv_mirror_end_io;
		atomic64_add(bio_sz, &ctx->mirror.mirror_write_bytes);
		atomic64_add(bio_sz,
			     &ctx->io.total_write_bytes[seg->mirror_disk]);
		atomic64_inc(&ctx->io.total_write_ops[seg->mirror_disk]);
		tv_pw_add(ctx, ctx->devs[seg->mirror_disk]->bdev,
			  mirror_sec, bio_sz);
		submit_bio(clone);
		tv_log(TV_LOG_INFO, cur.disk, TV_LOG_MIRROR,
		       "mirrored %uKB seg%d->disk%d",
		       bio_sz >> 10, cur.seg_idx, seg->mirror_disk);
		goto done;

fail_copy:
		{
			int i;

			for (i = 0; i < clone->bi_vcnt; i++)
				__free_page(clone->bi_io_vec[i].bv_page);
		}
		bio_put(clone);
		mempool_free(pwc, ctx->mirror_pw_pool);
		atomic64_inc(&ctx->mirror.mirror_errors);
		tv_log(TV_LOG_ERR, cur.disk, TV_LOG_MIRROR,
		       "mirror copy fail seg%d", cur.seg_idx);
done:
		return;
	} else if (bio_data_dir(bio) == READ) {
		tv_pending_add(ctx, ctx->devs[cur.disk]->bdev,
			       bio->bi_iter.bi_sector,
			       bio->bi_iter.bi_size,
			       (int)seg->mirror_disk,
			       mirror_sec);
	}
}
EXPORT_SYMBOL_GPL(tv_mirror_handle);

void tv_mirror_end_io(struct bio *bio)
{
	struct tv_mirror_pw_ctx *pwc = bio->bi_private;
	int i;

	if (bio->bi_status != BLK_STS_OK)
		atomic64_inc(&pwc->ctx->mirror.mirror_errors);
	else
		atomic64_inc(&pwc->ctx->mirror.mirror_write_ops);

	if (!tv_pw_remove(pwc->ctx, pwc->bdev, pwc->sector, pwc->size))
		pr_warn("tieredvol: pw_remove MISS sec=%llu size=%u (endio)\n",
			(unsigned long long)pwc->sector, pwc->size);
	/*
	 * bi_iter is consumed by the device on completion, so walk the raw
	 * bvec array to release every page the copy owns.
	 */
	for (i = 0; i < bio->bi_vcnt; i++)
		__free_page(bio->bi_io_vec[i].bv_page);
	bio_put(bio);
	mempool_free(pwc, pwc->ctx->mirror_pw_pool);
}
EXPORT_SYMBOL_GPL(tv_mirror_end_io);

/* ---- Mirror retry completion (reads from mirror, completes orig bio) ---- */

static void tv_mirror_retry_end_io(struct bio *bio)
{
	struct tv_retry_ctx *rc = bio->bi_private;
	struct bio *orig_bio = rc->orig_bio;

	if (bio->bi_status == BLK_STS_OK) {
		orig_bio->bi_status = BLK_STS_OK;
		atomic64_inc(&rc->ctx->mirror.mirror_read_ops);
	} else {
		orig_bio->bi_status = bio->bi_status;
		atomic64_inc(&rc->ctx->mirror.mirror_errors);
	}

	bio_endio(orig_bio);
	bio_put(orig_bio);
	bio_put(bio);
	mempool_free(rc, rc->ctx->retry_ctx_pool);
}

/* ---- Read retry work ---- */

void tv_read_retry_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct tv_retry_ctx *rc =
		container_of(dwork, struct tv_retry_ctx, dwork);
	struct bio *clone;

	if (tv_pw_is_pending(rc->ctx, rc->ctx->devs[rc->mirror_disk]->bdev,
			     rc->sector, rc->size)) {
		if (rc->retries-- > 0) {
			schedule_delayed_work(&rc->dwork, msecs_to_jiffies(1));
			return;
		}
		pr_warn("tieredvol: mirror retry gave up after 32 retries "
			"(mirror_disk=%d sector=%llu size=%u bdev=%p)\n",
			rc->mirror_disk, (unsigned long long)rc->sector,
			rc->size,
			rc->ctx->devs[rc->mirror_disk]->bdev);
		{
			int cpu, i;
			for_each_possible_cpu(cpu) {
				struct tv_pending_write_cpu *pcpu =
					per_cpu_ptr(rc->ctx->pcpu_writes, cpu);
				for (i = 0; i < pcpu->count; i++) {
					unsigned int idx =
						(pcpu->head + i) %
						TV_PENDING_RING_SIZE;
					struct tv_pending_write_entry *pw =
						&pcpu->entries[idx];
					pr_warn("  pw[%d] cpu%d bdev=%p sec=%llu size=%u\n",
						idx, cpu, (void *)pw->bdev,
						(unsigned long long)pw->sector,
						pw->size);
				}
			}
		}
		goto fail;
	}

	clone = bio_alloc_clone(rc->ctx->devs[rc->mirror_disk]->bdev,
				rc->orig_bio, GFP_NOIO, &fs_bio_set);
	if (!clone) {
		pr_err("tieredvol: mirror retry clone alloc failed\n");
		goto fail;
	}

	clone->bi_iter.bi_sector = rc->sector;
	clone->bi_iter.bi_size = rc->size;
	clone->bi_end_io = tv_mirror_retry_end_io;
	clone->bi_private = rc;

	submit_bio(clone);
	return;

fail:
	rc->orig_bio->bi_status = BLK_STS_IOERR;
	bio_endio(rc->orig_bio);
	bio_put(rc->orig_bio);
	mempool_free(rc, rc->ctx->retry_ctx_pool);
}
EXPORT_SYMBOL_GPL(tv_read_retry_work);

/* ---- DM end_io handler ---- */

int tieredvol_end_io(struct dm_target *ti, struct bio *bio, blk_status_t *error)
{
	struct tieredvol_ctx *ctx = ti->private;
	int i, disk_id = -1;

	/* Single scan: find which disk this bio completed on */
	for (i = 0; i < ctx->ndisks; i++) {
		if (bio->bi_bdev == ctx->devs[i]->bdev) {
			disk_id = i;
			break;
		}
	}

	/* Fix 2: Decrement in_flight_bytes on every completion */
	if (disk_id >= 0) {
		atomic_sub(bio->bi_iter.bi_size,
			   &ctx->io.in_flight_bytes[disk_id]);
	}

	/* Error path */
	if (bio->bi_status != BLK_STS_OK) {
		if (disk_id >= 0) {
			int errs;

			errs = atomic_inc_return(&ctx->deg.error_count[disk_id]);
			tv_log(TV_LOG_ERR, disk_id, TV_LOG_IO,
			       "I/O error on %s status=%d err=%d",
			       ctx->meta.disk_names[disk_id],
			       bio->bi_status, errs);

			if (!ctx->deg.degraded[disk_id] &&
			    errs >= (int)ctx->deg.error_threshold) {
				ctx->deg.degraded[disk_id] = true;
				pr_warn("tieredvol: disk[%d] %s DEGRADED (errors=%d >= threshold=%u)\n",
					disk_id, ctx->meta.disk_names[disk_id],
					errs,
					ctx->deg.error_threshold);
				tv_log(TV_LOG_WARN, disk_id, TV_LOG_IO,
				       "DEGRADED err=%d", errs);
				schedule_work(&ctx->trigger_event);
			}

			/* Mark bad block on WRITE error */
			if (bio_data_dir(bio) == WRITE) {
				u64 phy_byte = (u64)bio->bi_iter.bi_sector << 9;
				u64 chunk_no = phy_byte / ctx->meta.chunk_size;

				tv_badmap_set(ctx, disk_id, chunk_no);
				tv_log(TV_LOG_ERR, disk_id, TV_LOG_IO,
				       "WRITE err mark bad chunk %llu", chunk_no);
			}
		}

		if (bio_data_dir(bio) == READ && disk_id >= 0) {
			int mirror;
			sector_t mirror_sector;

			mirror = tv_pending_find_and_remove(
				ctx, bio->bi_bdev,
				bio->bi_iter.bi_sector,
				bio->bi_iter.bi_size,
				&mirror_sector);

			if (mirror >= 0 &&
			    mirror < ctx->ndisks) {
				struct tv_retry_ctx *rc;

			rc = mempool_alloc(ctx->retry_ctx_pool,
					     GFP_ATOMIC);
			if (rc) {
					INIT_DELAYED_WORK(
						&rc->dwork,
						tv_read_retry_work);
					rc->ctx = ctx;
					rc->orig_bio = bio;
					rc->sector = mirror_sector;
					rc->size =
						bio->bi_iter.bi_size;
					rc->mirror_disk =
						mirror;
					rc->retries = 32;
					bio_get(bio);
					schedule_delayed_work(
						&rc->dwork, 0);
					return 1;
				}
			}
		}

		return 0;
	}

	/* Success path: only scan pending if mirror is configured */
	if (bio_data_dir(bio) == READ && ctx->mirror_enabled_any) {
		tv_pending_find_and_remove(ctx, bio->bi_bdev,
					   bio->bi_iter.bi_sector,
					   bio->bi_iter.bi_size,
					   NULL);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(tieredvol_end_io);

/* ---- Rebuild thread (2c) ---- */

static void tv_rebuild_end_io(struct bio *bio)
{
	struct completion *done = bio->bi_private;

	complete(done);
}

int tv_rebuild_thread(void *data)
{
	struct tieredvol_ctx *ctx = data;
	struct tieredvol_segment *seg;
	struct bio *bio_r, *bio_w;
	unsigned int chunk_bytes, sz;
	int backoff_ms = 10;

	while (!kthread_should_stop()) {
		if (!atomic_read(&ctx->rebuild.running))
			break;

		seg = &ctx->meta.segments[ctx->rebuild.seg_idx];
		chunk_bytes = ctx->meta.chunk_size;

		if (ctx->rebuild.offset >= ctx->rebuild.total) {
			pr_info("tieredvol: rebuild seg%d complete (%llu bytes)\n",
				ctx->rebuild.seg_idx, ctx->rebuild.total);
			tv_log(TV_LOG_INFO, seg->mirror_disk, TV_LOG_MIRROR,
			       "rebuild seg%d complete %llu bytes",
			       ctx->rebuild.seg_idx, ctx->rebuild.total);
			atomic_set(&ctx->rebuild.running, 0);
			schedule_work(&ctx->trigger_event);
			break;
		}

		{
			struct page *pg;
			u64 logical_addr;
			struct tieredvol_map cur;

			logical_addr = ctx->rebuild.offset + seg->logical_begin;
			cur = tv_map_logical(logical_addr, &ctx->meta,
					     ctx->meta.chunk_size);
			if (cur.disk < 0 || cur.length == 0) {
				msleep(backoff_ms);
				backoff_ms = min(backoff_ms * 2, 1000);
				continue;
			}

			sz = min_t(u64, chunk_bytes, cur.length);

			pg = alloc_pages(GFP_NOIO, get_order(sz));
			if (!pg) {
				msleep(backoff_ms);
				backoff_ms = min(backoff_ms * 2, 1000);
				continue;
			}

			reinit_completion(&ctx->rebuild.done_r);

			bio_r = bio_alloc(ctx->devs[cur.disk]->bdev, 1,
					  REQ_OP_READ, GFP_NOIO);
			if (!bio_r) {
				put_page(pg);
				msleep(backoff_ms);
				backoff_ms = min(backoff_ms * 2, 1000);
				continue;
			}
			bio_r->bi_iter.bi_sector = cur.offset >> TV_SECTOR_SHIFT;
			bio_r->bi_private = &ctx->rebuild.done_r;
			bio_r->bi_end_io = tv_rebuild_end_io;

			if (bio_add_page(bio_r, pg, sz, 0) != sz) {
				put_page(pg);
				bio_put(bio_r);
				msleep(backoff_ms);
				backoff_ms = min(backoff_ms * 2, 1000);
				continue;
			}

			submit_bio(bio_r);
			wait_for_completion(&ctx->rebuild.done_r);

			if (bio_r->bi_status != BLK_STS_OK) {
				pr_err("tieredvol: rebuild read failed at offset %llu\n",
				       ctx->rebuild.offset);
				put_page(pg);
				bio_put(bio_r);
				msleep(backoff_ms);
				backoff_ms = min(backoff_ms * 2, 1000);
				continue;
			}
			bio_put(bio_r);

			if (!atomic_read(&ctx->rebuild.running)) {
				put_page(pg);
				break;
			}

			reinit_completion(&ctx->rebuild.done_w);

			bio_w = bio_alloc(ctx->devs[seg->mirror_disk]->bdev, 1,
					  REQ_OP_WRITE, GFP_NOIO);
			if (!bio_w) {
				put_page(pg);
				msleep(backoff_ms);
				backoff_ms = min(backoff_ms * 2, 1000);
				continue;
			}
			bio_w->bi_iter.bi_sector =
				ctx->rebuild.offset >> TV_SECTOR_SHIFT;
			bio_w->bi_private = &ctx->rebuild.done_w;
			bio_w->bi_end_io = tv_rebuild_end_io;

			if (bio_add_page(bio_w, pg, sz, 0) != sz) {
				put_page(pg);
				bio_put(bio_w);
				msleep(backoff_ms);
				backoff_ms = min(backoff_ms * 2, 1000);
				continue;
			}

			submit_bio(bio_w);
			wait_for_completion(&ctx->rebuild.done_w);

			if (bio_w->bi_status != BLK_STS_OK) {
				pr_err("tieredvol: rebuild write failed at offset %llu\n",
				       ctx->rebuild.offset);
				put_page(pg);
				bio_put(bio_w);
				msleep(backoff_ms);
				backoff_ms = min(backoff_ms * 2, 1000);
				continue;
			}
			put_page(pg);
			bio_put(bio_w);
			backoff_ms = 10;
		}

		ctx->rebuild.offset += sz;

		if ((ctx->rebuild.offset % (10 * 1024 * 1024)) == 0 ||
		    ctx->rebuild.offset >= ctx->rebuild.total) {
			u64 pct = ctx->rebuild.total ?
					  (ctx->rebuild.offset * 100 /
					   ctx->rebuild.total) :
					  0;

			pr_info("tieredvol: rebuild seg%d %llu/%llu (%llu%%)\n",
				ctx->rebuild.seg_idx,
				ctx->rebuild.offset, ctx->rebuild.total, pct);
		}

		cond_resched();
	}

	atomic_set(&ctx->rebuild.running, 0);
	ctx->rebuild.thread = NULL;
	return 0;
}
EXPORT_SYMBOL_GPL(tv_rebuild_thread);
