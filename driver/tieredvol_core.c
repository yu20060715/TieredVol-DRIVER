// SPDX-License-Identifier: GPL-2.0-only
/*
 * tieredvol_core.c — DM lifecycle: ctr/dtr/map/status/io_hints/ioctl/iterate,
 * trigger_event, module init/exit.
 *
 * Trimmed from the original 1843-line monolith in Phase 1 refactoring.
 * Log, mirror, sysfs, message handlers moved to separate files.
 */
#include <linux/module.h>
#include <linux/device-mapper.h>
#include <linux/bio.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/kthread.h>
#include <linux/kfifo.h>
#include <linux/mempool.h>
#include "tieredvol.h"

struct tieredvol_ctx __rcu *tv_active_ctx;

static void trigger_event(struct work_struct *work)
{
	struct tieredvol_ctx *ctx = container_of(work, struct tieredvol_ctx,
						 trigger_event);
	dm_table_event(ctx->ti->table);
}

static int tieredvol_map(struct dm_target *ti, struct bio *bio)
{
	struct tieredvol_ctx *ctx = ti->private;
	u64 logical;
	struct tieredvol_map cur;
	bool borrowed = false;

	logical = (u64)bio->bi_iter.bi_sector << TV_SECTOR_SHIFT;

	{
		/* Find segment to determine per-segment policy */
		int seg_idx = -1;
		int lo = 0, hi = (int)ctx->meta.segment_count - 1;

		while (lo <= hi) {
			int mid = lo + (hi - lo) / 2;
			const struct tieredvol_segment *seg =
				&ctx->meta.segments[mid];

			if (logical < seg->logical_begin)
				hi = mid - 1;
			else if (logical >= seg->logical_end)
				lo = mid + 1;
			else {
				seg_idx = mid;
				break;
			}
		}

		int pol = seg_idx >= 0 ?
			ctx->meta.segments[seg_idx].policy : -1;
		if (pol < 0)
			pol = ctx->policy;

		switch (pol) {
		case TV_POLICY_RANDOM:
			cur = tv_map_logical_random(logical, &ctx->meta,
						    ctx->meta.chunk_size);
			break;
		case TV_POLICY_STATIC:
		default:
			cur = tv_map_logical(logical, &ctx->meta,
					     ctx->meta.chunk_size);
			break;
		}
	}

	if (cur.disk < 0 || cur.disk >= ctx->ndisks) {
		pr_err("tieredvol: map failed for sector %llu\n",
		       (unsigned long long)bio->bi_iter.bi_sector);
		tv_log(TV_LOG_ERR, TV_LOG_IO,
		       "map fail sec=%llu", bio->bi_iter.bi_sector);
		bio_io_error(bio);
		return DM_MAPIO_SUBMITTED;
	}

	/* Borrowed chunks resolve to their recorded destination on read. */
	if (bio_data_dir(bio) == READ) {
		u64 bs;

		if (tv_borrow_lookup(ctx, logical,
				     &cur.disk, &bs)) {
			if (cur.disk < 0 || cur.disk >= ctx->ndisks) {
				pr_err("tieredvol: borrow entry out of range "
				       "disk=%d sector=%llu\n",
				       cur.disk,
				       (unsigned long long)bio->bi_iter.bi_sector);
				tv_log(TV_LOG_ERR, TV_LOG_IO,
				       "borrow OOR sec=%llu disk=%d",
				       bio->bi_iter.bi_sector, cur.disk);
				bio_io_error(bio);
				return DM_MAPIO_SUBMITTED;
			}
			cur.offset = bs << TV_SECTOR_SHIFT;
			cur.length = bio->bi_iter.bi_size;
		}
	}

	/* ---- Phase D: Bad block bitmap check ---- */
	{
		u64 chunk_no = cur.offset / ctx->meta.chunk_size;
		struct tieredvol_segment *seg;

		if (bio_data_dir(bio) == READ &&
		    tv_badmap_test(ctx, cur.disk, chunk_no)) {
			seg = (cur.seg_idx >= 0 &&
			       cur.seg_idx < (int)ctx->meta.segment_count) ?
				&ctx->meta.segments[cur.seg_idx] : NULL;

			/* Mirror read-back: a badmapped chunk on the primary
			 * still returns real data from the mirror. Falls back
			 * to zero-fill when no mirror or alloc fails. */
			if (seg && seg->mirror_enabled) {
				sector_t mirror_sec =
					(logical - seg->logical_begin) >>
					TV_SECTOR_SHIFT;
				struct tv_retry_ctx *rc;

				rc = mempool_alloc(ctx->retry_ctx_pool,
						   GFP_ATOMIC);
				if (rc) {
					INIT_DELAYED_WORK(&rc->dwork,
							  tv_read_retry_work);
					rc->ctx = ctx;
					rc->orig_bio = bio;
					rc->sector = mirror_sec;
					rc->size = bio->bi_iter.bi_size;
					rc->mirror_disk = seg->mirror_disk;
					rc->retries = 32;
					bio_get(bio);
					schedule_delayed_work(&rc->dwork, 0);
					return DM_MAPIO_SUBMITTED;
				}
			}

			zero_fill_bio(bio);
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}
		if (bio_data_dir(bio) == WRITE &&
		    tv_badmap_test(ctx, cur.disk, chunk_no)) {
			tv_log(TV_LOG_WARN, TV_LOG_IO,
			       "skip bad chunk %llu disk[%d]", chunk_no, cur.disk);
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}
	}
	/* ---- Phase 1 C: Write coalescing (disabled while borrowing) ---- */
	if (!ctx->borrow.enabled) {
		int wc_ret = tv_wc_try_buffer(ctx, bio, logical, cur);

		if (wc_ret == DM_MAPIO_SUBMITTED)
			return DM_MAPIO_SUBMITTED;
	}

	/* Non-WRITE: flush buffer for read ordering */
	if (bio_data_dir(bio) != WRITE)
		tv_wc_flush(ctx);

	/* ---- B: Parallel multi-disk write ---- */
	if (bio_data_dir(bio) == WRITE &&
	    cur.seg_idx >= 0 &&
	    cur.seg_idx < (int)ctx->meta.segment_count) {
		struct tieredvol_segment *seg = &ctx->meta.segments[cur.seg_idx];
		int n_seg = (int)seg->disk_count;

		if (n_seg > 1) {
			struct tv_stripe_ctx sc;
			u64 b_sz = bio->bi_iter.bi_size;

			tv_stripe_calc_boundaries(seg, ctx->meta.chunk_size,
						  logical, b_sz, &sc);

			if (sc.fi >= 0 && sc.li - sc.fi + 1 > 1) {
				u64 d_start[TV_MAX_DISKS];
				u64 d_sz[TV_MAX_DISKS];
				int d_id[TV_MAX_DISKS];
				int n_sub = tv_stripe_compute_ranges(
					&sc, seg, logical,
					ctx->meta.chunk_size,
					d_start, d_sz, d_id);

				if (n_sub > 1) {
					/* Weight-borrowing: per-fragment */
					u64 frag_logical = logical;
					int ci;

					for (ci = 0; ci < n_sub; ci++) {
						u64 bs;

						if (tv_borrow_redirect(
							    ctx, d_id[ci],
							    frag_logical,
							    d_sz[ci],
							    &d_id[ci], &bs)) {
							if (d_id[ci] < 0 ||
							    d_id[ci] >=
								    ctx->ndisks) {
								pr_err("tieredvol: borrow redirect OOR disk=%d\n",
								       d_id[ci]);
								tv_log(TV_LOG_ERR, TV_LOG_IO,
								       "redirect OOR disk=%d sec=%llu",
								       d_id[ci],
								       (unsigned long long)frag_logical);
								bio_io_error(bio);
								return DM_MAPIO_SUBMITTED;
							}
							d_start[ci] =
								bs << TV_SECTOR_SHIFT;
						}
						frag_logical += d_sz[ci];
					}

					tv_mirror_handle(ctx, bio, cur,
							 logical);
					if (tv_parallel_submit(ctx, bio,
							       n_sub,
							       d_start, d_sz,
							       d_id) == 0)
						return DM_MAPIO_SUBMITTED;
				}
			}
		}
	}
	/* ---- End B ---- */

	/* Weight-borrowing: single-disk WRITE (B path did not apply) */
	if (bio_data_dir(bio) == WRITE && cur.seg_idx >= 0) {
		u64 bs;

		if (tv_borrow_redirect(ctx, cur.disk, logical,
				       bio->bi_iter.bi_size,
				       &cur.disk, &bs)) {
			if (cur.disk < 0 || cur.disk >= ctx->ndisks) {
				pr_err("tieredvol: borrow redirect out of range "
				       "disk=%d sector=%llu\n",
				       cur.disk,
				       (unsigned long long)bio->bi_iter.bi_sector);
				tv_log(TV_LOG_ERR, TV_LOG_IO,
				       "redirect OOR sec=%llu disk=%d",
				       bio->bi_iter.bi_sector, cur.disk);
				bio_io_error(bio);
				return DM_MAPIO_SUBMITTED;
			}
			cur.offset = bs << TV_SECTOR_SHIFT;
			borrowed = true;
		}
	}

	/* Split bio at stripe chunk boundary if it crosses to next disk */
	if (!borrowed &&
	    cur.seg_idx >= 0 &&
	    cur.seg_idx < (int)ctx->meta.segment_count) {
		struct tieredvol_segment *seg =
			&ctx->meta.segments[cur.seg_idx];
		u64 stripe_off = (logical - seg->logical_begin) %
				 seg->stripe_size;
		u64 chunk_acc = 0;
		int i;

		for (i = 0; i < (int)seg->disk_count; i++) {
			u64 csize = (u64)seg->weight[i] *
				    ctx->meta.chunk_size;

			chunk_acc += csize;
			if ((int)seg->disk_index[i] == cur.disk) {
				u64 remain = chunk_acc - stripe_off;
				sector_t remain_sect = remain >>
						      TV_SECTOR_SHIFT;

				if (remain_sect > 0 &&
				    bio_sectors(bio) > remain_sect)
					dm_accept_partial_bio(bio,
							      remain_sect);
				break;
			}
		}
	}

	bio_set_dev(bio, ctx->devs[cur.disk]->bdev);
	bio->bi_iter.bi_sector = cur.offset >> TV_SECTOR_SHIFT;
	atomic_add(bio->bi_iter.bi_size, &ctx->io.in_flight_bytes[cur.disk]);
	if (bio_data_dir(bio) == WRITE) {
		atomic64_add(bio->bi_iter.bi_size, &ctx->io.total_write_bytes[cur.disk]);
		atomic64_inc(&ctx->io.total_write_ops[cur.disk]);
	} else {
		atomic64_add(bio->bi_iter.bi_size, &ctx->io.total_read_bytes[cur.disk]);
		atomic64_inc(&ctx->io.total_read_ops[cur.disk]);
	}

	tv_mirror_handle(ctx, bio, cur, logical);

	return DM_MAPIO_REMAPPED;
}

static int tieredvol_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct tieredvol_ctx *ctx;
	int ret, i;
	bool mirror_init_done = false;
	bool borrow_init_done = false;

	if (argc != 1) {
		ti->error = "tieredvol: expected 1 argument (config path)";
		return -EINVAL;
	}

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		ti->error = "tieredvol: out of memory";
		return -ENOMEM;
	}

	ctx->ti = ti;
	strscpy(ctx->config_path, argv[0], sizeof(ctx->config_path));

	ret = tv_metadata_load_kernel(&ctx->meta, argv[0]);
	if (ret) {
		ti->error = "tieredvol: failed to load metadata";
		goto free_ctx;
	}

	if (ctx->meta.disk_count == 0 || ctx->meta.disk_count > TV_MAX_DISKS) {
		ti->error = "tieredvol: invalid disk count";
		ret = -EINVAL;
		goto free_ctx;
	}

	ctx->ndisks = ctx->meta.disk_count;

	ctx->devs = kcalloc(ctx->ndisks, sizeof(*ctx->devs), GFP_KERNEL);
	ctx->disk_sectors = kcalloc(ctx->ndisks, sizeof(*ctx->disk_sectors),
				    GFP_KERNEL);
	if (!ctx->devs || !ctx->disk_sectors) {
		ti->error = "tieredvol: out of memory for devs";
		ret = -ENOMEM;
		goto free_devs;
	}

	for (i = 0; i < ctx->ndisks; i++) {
		ret = dm_get_device(ti, ctx->meta.disk_names[i],
				    dm_table_get_mode(ti->table),
				    &ctx->devs[i]);
		if (ret) {
			ti->error = "tieredvol: device lookup failed";
			goto put_devices;
		}
		/* Reject non-physical devices (loop, ram, zram) */
		{
			const char *dn = ctx->devs[i]->bdev->bd_disk->disk_name;
			if (strncmp(dn, "loop", 4) == 0 ||
			    strncmp(dn, "ram", 3) == 0 ||
			    strncmp(dn, "zram", 4) == 0) {
				pr_err("tieredvol: %s is a virtual device (%s) — use physical disks only\n",
				       ctx->meta.disk_names[i], dn);
				ti->error = "tieredvol: virtual device rejected";
				dm_put_device(ti, ctx->devs[i]);
				ret = -EINVAL;
				goto put_devices;
			}
		}
		ctx->disk_sectors[i] = bdev_nr_sectors(ctx->devs[i]->bdev);
	}

	ctx->deg.error_count = kcalloc(ctx->ndisks, sizeof(atomic_t),
					GFP_KERNEL);
	if (!ctx->deg.error_count) {
		ti->error = "tieredvol: out of memory for error_count";
		ret = -ENOMEM;
		goto put_devices;
	}

	INIT_WORK(&ctx->trigger_event, trigger_event);

	ctx->policy = ctx->meta.runtime_policy;
	atomic64_set(&ctx->mirror.mirror_write_bytes, 0);
	atomic64_set(&ctx->mirror.mirror_write_ops, 0);
	atomic64_set(&ctx->mirror.mirror_errors, 0);
	ctx->deg.error_threshold = 10;
	ctx->rebuild.thread = NULL;
	ctx->rebuild.seg_idx = -1;
	ctx->rebuild.offset = 0;
	ctx->rebuild.total = 0;
	atomic_set(&ctx->rebuild.running, 0);
	init_completion(&ctx->rebuild.done_r);
	init_completion(&ctx->rebuild.done_w);
	tv_wc_init_ctx(ctx);

	for (i = 0; i < ctx->ndisks; i++)
		ctx->bench[i].start_time = ktime_get();

	for (i = 0; i < ctx->ndisks; i++)
		pr_info("tieredvol: disk[%d] %s -> %pg (%llu sectors)\n",
			i, ctx->meta.disk_names[i], ctx->devs[i]->bdev,
			(unsigned long long)ctx->disk_sectors[i]);

	tv_badmap_init(ctx);

	/* Load bad block ranges from metadata into bitmaps */
	for (i = 0; i < ctx->ndisks; i++) {
		const char *ranges = ctx->meta.badmap_ranges[i];

		if (ranges && *ranges) {
			const char *p = ranges;
			u64 start, end;

			pr_info("tieredvol: badmap disk[%d] ranges: %s\n",
				i, ranges);
			while (*p) {
				while (*p == ',') p++;
				if (!*p) break;
				if (sscanf(p, "%llu-%llu", &start, &end) == 2) {
					u64 c;
					for (c = start; c <= end && c < ctx->badmaps[i].n_chunks; c++)
						set_bit(c, ctx->badmaps[i].bitmap);
				} else if (sscanf(p, "%llu", &start) == 1) {
					if (start < ctx->badmaps[i].n_chunks)
						set_bit(start, ctx->badmaps[i].bitmap);
				}
				while (*p && *p != ',') p++;
			}
		}
	}

	if (ctx->meta.segment_count == 0) {
		ti->error = "tieredvol: no segments";
		ret = -EINVAL;
		goto free_error_count;
	}

	/* Validate segments are sorted by logical_begin */
	for (i = 1; i < (int)ctx->meta.segment_count; i++) {
		if (ctx->meta.segments[i].logical_begin <
		    ctx->meta.segments[i - 1].logical_begin) {
			ti->error =
				"tieredvol: segments not sorted by logical_begin";
			ret = -EINVAL;
			goto free_error_count;
		}
	}

	/* Validate mirror disk capacity */
	{
		u32 si;

		for (si = 0; si < ctx->meta.segment_count; si++) {
			struct tieredvol_segment *seg =
				&ctx->meta.segments[si];
			u64 seg_sectors;
			u64 mirror_sectors;

			if (!seg->mirror_enabled)
				continue;

			if (seg->mirror_disk >= ctx->ndisks) {
				ti->error =
					"tieredvol: mirror_disk out of range";
				ret = -EINVAL;
				goto free_error_count;
			}

			seg_sectors = (seg->logical_end -
				       seg->logical_begin) >> 9;
			mirror_sectors =
				ctx->disk_sectors[seg->mirror_disk];

			if (mirror_sectors < seg_sectors) {
				pr_err("tieredvol: segment[%u] needs %llu sectors, mirror disk[%u] has %llu\n",
				       si,
				       (unsigned long long)seg_sectors,
				       seg->mirror_disk,
				       (unsigned long long)mirror_sectors);
				ti->error =
					"tieredvol: mirror disk too small for segment";
				ret = -EINVAL;
				goto free_error_count;
			}
		}
	}

	/* Check if any segment has mirror enabled */
	{
		u32 si;

	ctx->mirror_enabled_any = false;
	for (si = 0; si < ctx->meta.segment_count; si++) {
		if (ctx->meta.segments[si].mirror_enabled) {
			ctx->mirror_enabled_any = true;
			break;
		}
	}

	ret = tv_mirror_init_ctx(ctx);
	if (ret) {
		ti->error = "tieredvol: mempool alloc failed";
		goto free_error_count;
	}
	mirror_init_done = true;
	}

	ret = tv_borrow_init(ctx);
	if (ret) {
		ti->error = "tieredvol: borrow init failed";
		goto free_error_count;
	}
	borrow_init_done = true;

	/* Compute min_chunk_sectors and stripe_sectors */
	{
		sector_t global_min_chunk = (sector_t)-1;
		sector_t max_stripe = 0;
		u32 si, j;
		sector_t chunk_sectors = ctx->meta.chunk_size >> TV_SECTOR_SHIFT;

		for (si = 0; si < ctx->meta.segment_count; si++) {
			struct tieredvol_segment *seg =
				&ctx->meta.segments[si];
			sector_t seg_min;

			if (seg->disk_count == 0)
				continue;

			seg_min = (sector_t)seg->weight[0] * chunk_sectors;
			for (j = 1; j < seg->disk_count; j++) {
				sector_t w = (sector_t)seg->weight[j] *
					     chunk_sectors;

				if (w < seg_min)
					seg_min = w;
			}
			if (seg_min < global_min_chunk)
				global_min_chunk = seg_min;
			if (seg->stripe_size > max_stripe)
				max_stripe = seg->stripe_size;
		}

		if (global_min_chunk == (sector_t)-1 ||
		    global_min_chunk == 0) {
			ti->error = "tieredvol: invalid chunk geometry";
			ret = -EINVAL;
			goto free_error_count;
		}

		ctx->min_chunk_sectors = global_min_chunk;
		ctx->stripe_sectors = max_stripe >> TV_SECTOR_SHIFT;
		if (ctx->stripe_sectors == 0)
			ctx->stripe_sectors = chunk_sectors;
	}

	for (i = 0; i < (int)ctx->meta.segment_count; i++)
		pr_info("tieredvol: segment[%d] [%llu, %llu) stripe=%llu disks=%u\n",
			i,
			(unsigned long long)ctx->meta.segments[i].logical_begin,
			(unsigned long long)ctx->meta.segments[i].logical_end,
			(unsigned long long)ctx->meta.segments[i].stripe_size,
			ctx->meta.segments[i].disk_count);

	pr_info("tieredvol: min_chunk=%llu sectors, stripe=%llu sectors\n",
		(unsigned long long)ctx->min_chunk_sectors,
		(unsigned long long)ctx->stripe_sectors);

	ret = dm_set_target_max_io_len(ti, ctx->stripe_sectors);
	if (ret) {
		ti->error = "tieredvol: dm_set_target_max_io_len failed";
		goto free_error_count;
	}

	ti->num_flush_bios = ctx->ndisks;
	ti->num_discard_bios = ctx->ndisks;
	ti->flush_bypasses_map = true;

	ti->private = ctx;
	rcu_assign_pointer(tv_active_ctx, ctx);
	return 0;

free_error_count:
	if (borrow_init_done)
		tv_borrow_destroy(ctx);
	if (mirror_init_done)
		tv_mirror_destroy_ctx(ctx);
	tv_badmap_destroy(ctx);
	kfree(ctx->deg.error_count);
put_devices:
	for (i = i - 1; i >= 0; i--)
		dm_put_device(ti, ctx->devs[i]);
free_devs:
	kfree(ctx->devs);
	kfree(ctx->disk_sectors);
free_ctx:
	kfree(ctx);
	return ret;
}

static void tieredvol_dtr(struct dm_target *ti)
{
	struct tieredvol_ctx *ctx = ti->private;
	int i;

	tv_borrow_destroy(ctx);
	flush_work(&ctx->trigger_event);

	tv_wc_destroy_ctx(ctx);
	tv_mirror_destroy_ctx(ctx);

	if (atomic_read(&ctx->rebuild.running)) {
		atomic_set(&ctx->rebuild.running, 0);
		complete(&ctx->rebuild.done_r);
		complete(&ctx->rebuild.done_w);
		if (!IS_ERR_OR_NULL(ctx->rebuild.thread))
			kthread_stop(ctx->rebuild.thread);
	}

	tv_badmap_destroy(ctx);
	kfree(ctx->deg.error_count);

	for (i = 0; i < ctx->ndisks; i++)
		dm_put_device(ti, ctx->devs[i]);

	kfree(ctx->devs);
	kfree(ctx->disk_sectors);
	if (rcu_dereference_raw(tv_active_ctx) == ctx) {
		rcu_assign_pointer(tv_active_ctx, NULL);
		synchronize_rcu();
	}
	kfree(ctx);
}

static int tieredvol_prepare_ioctl(struct dm_target *ti,
				   struct block_device **bdev)
{
	struct tieredvol_ctx *ctx = ti->private;

	if (ctx->ndisks > 0)
		*bdev = ctx->devs[0]->bdev;

	return 0;
}

static void tieredvol_io_hints(struct dm_target *ti, struct queue_limits *limits)
{
	struct tieredvol_ctx *ctx = ti->private;

	limits->logical_block_size = 512;
	limits->physical_block_size = 512;
	limits->chunk_sectors = ctx->stripe_sectors;
	limits->io_min = ctx->min_chunk_sectors;
	limits->io_opt = ctx->stripe_sectors;
}

static int tieredvol_iterate_devices(struct dm_target *ti,
				     iterate_devices_callout_fn fn, void *data)
{
	struct tieredvol_ctx *ctx = ti->private;
	int ret = 0;
	int i;

	for (i = 0; !ret && i < ctx->ndisks; i++)
		ret = fn(ti, ctx->devs[i], 0,
			 bdev_nr_sectors(ctx->devs[i]->bdev), data);

	return ret;
}

static void tieredvol_status(struct dm_target *ti, status_type_t type,
			     unsigned int status_flags, char *result,
			     unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;

	switch (type) {
	case STATUSTYPE_INFO: {
		int i, off = 0;

		off += snprintf(result + off, maxlen - off,
				"policy=%d borrow=%d/%llu mirror=%llu/%llu err=%llu",
				ctx->policy,
				ctx->borrow.enabled,
				(unsigned long long)ctx->borrow.n_borrowed,
				atomic64_read(&ctx->mirror.mirror_write_ops),
				atomic64_read(&ctx->mirror.mirror_write_bytes),
				atomic64_read(&ctx->mirror.mirror_errors));

		for (i = 0; i < ctx->ndisks && off < (int)maxlen - 2; i++) {
			char status;

			if (ctx->deg.degraded[i])
				status = 'D';
			else if (atomic_read(&ctx->deg.error_count[i]))
				status = 'E';
			else
				status = 'A';

			off += snprintf(result + off, maxlen - off,
					" %c%s:rd=%llu/%llu wr=%llu/%llu bw=%llu",
					status, ctx->meta.disk_names[i],
					atomic64_read(&ctx->io.total_read_ops[i]),
					atomic64_read(&ctx->io.total_read_bytes[i]),
					atomic64_read(&ctx->io.total_write_ops[i]),
					atomic64_read(&ctx->io.total_write_bytes[i]),
					atomic64_read(&ctx->borrow.borrow_write_bytes[i]));
		}
		if (atomic_read(&ctx->rebuild.running))
			off += snprintf(result + off, maxlen - off,
					" rebuild=%d/%d",
					ctx->rebuild.seg_idx,
					(int)(ctx->rebuild.offset /
					      ctx->meta.chunk_size));
		break;
	}
	case STATUSTYPE_TABLE: {
		int off = 0;
		int i;

		for (i = 0; i < ctx->ndisks && off < maxlen; i++) {
			int n = snprintf(result + off, maxlen - off,
					 "%s%s", i > 0 ? " " : "",
					 ctx->meta.disk_names[i]);
			if (n < 0)
				break;
			off += n;
		}
		break;
	}
	case STATUSTYPE_IMA:
		result[0] = '\0';
		break;
	}
}

static struct target_type tieredvol_target = {
	.name   = "tieredvol",
	.version = {2, 0, 0},
	.module = THIS_MODULE,
	.features = DM_TARGET_NOWAIT,
	.ctr    = tieredvol_ctr,
	.dtr    = tieredvol_dtr,
	.map    = tieredvol_map,
	.end_io = tieredvol_end_io,
	.status = tieredvol_status,
	.message = tieredvol_message,
	.prepare_ioctl = tieredvol_prepare_ioctl,
	.io_hints = tieredvol_io_hints,
	.iterate_devices = tieredvol_iterate_devices,
};

static int __init tieredvol_init(void)
{
	int ret;

	if (log_size == 0)
		log_size = TV_LOG_SIZE;
	if (!is_power_of_2(log_size))
		log_size = roundup_pow_of_two(log_size);

	ret = kfifo_alloc(&tv_log_fifo,
			  log_size * sizeof(struct tv_log_entry),
			  GFP_KERNEL);
	if (ret) {
		pr_err("tieredvol: kfifo alloc failed (%u entries)\n", log_size);
		return ret;
	}

	ret = dm_register_target(&tieredvol_target);
	if (ret < 0) {
		pr_err("tieredvol: registration failed: %d\n", ret);
		kfifo_free(&tv_log_fifo);
		return ret;
	}

	tv_sysfs_init();

	pr_info("tieredvol: module loaded (log_size=%u)\n", log_size);
	return 0;
}

static void __exit tieredvol_exit(void)
{
	tv_sysfs_exit();
	dm_unregister_target(&tieredvol_target);
	kfifo_free(&tv_log_fifo);
	pr_info("tieredvol: module unloaded\n");
}

module_init(tieredvol_init);
module_exit(tieredvol_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("TieredVol");
MODULE_DESCRIPTION("Weighted striped dm target for tiered storage");
MODULE_VERSION("5.0.0");
