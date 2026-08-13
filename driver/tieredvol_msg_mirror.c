// SPDX-License-Identifier: GPL-2.0-only
/*
 * tieredvol_msg_mirror.c — Mirror / rebuild / degradation message handlers
 */
#include <linux/module.h>
#include <linux/string.h>
#include <linux/kthread.h>
#include <linux/device-mapper.h>
#include "tieredvol.h"
#include "tieredvol_msg.h"

static int msg_show_mirror(struct dm_target *ti, unsigned int argc,
			   char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	int i, off = 0;

	off += snprintf(result + off, maxlen - off,
			"mirror_wr=%llu/%llu mirror_err=%llu",
			atomic64_read(&ctx->mirror.mirror_write_ops),
			atomic64_read(&ctx->mirror.mirror_write_bytes),
			atomic64_read(&ctx->mirror.mirror_errors));
	for (i = 0; i < (int)ctx->meta.segment_count &&
		     off < (int)maxlen - 2; i++) {
		struct tieredvol_segment *seg = &ctx->meta.segments[i];

		off += snprintf(result + off, maxlen - off,
				" seg%d:mirror=%s%d", i,
				seg->mirror_enabled ? "" : "off",
				seg->mirror_enabled ? (int)seg->mirror_disk : 0);
	}
	pr_info("tieredvol: %s\n", result);
	return 0;
}

static int msg_set_mirror(struct dm_target *ti, unsigned int argc,
			  char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	u32 seg_idx, disk_idx;

	if (kstrtou32(argv[1], 10, &seg_idx) ||
	    kstrtou32(argv[2], 10, &disk_idx) ||
	    seg_idx >= ctx->meta.segment_count ||
	    disk_idx >= (u32)ctx->ndisks)
		return -EINVAL;

	/* Mirror target must not be a stripe participant */
	{
		struct tieredvol_segment *seg =
			&ctx->meta.segments[seg_idx];
		unsigned int k;

		for (k = 0; k < seg->disk_count; k++) {
			if (seg->disk_index[k] == disk_idx) {
				pr_err("tieredvol: mirror disk %u is stripe participant of seg%u\n",
				       disk_idx, seg_idx);
				return -EINVAL;
			}
		}
	}

	ctx->meta.segments[seg_idx].mirror_enabled = true;
	ctx->meta.segments[seg_idx].mirror_disk = disk_idx;
	pr_info("tieredvol: seg%u mirror -> disk%u (%s)\n", seg_idx,
		disk_idx, ctx->meta.disk_names[disk_idx]);
	tv_log(TV_LOG_INFO, TV_LOG_CONFIG, "mirror seg%u->disk%u",
	       seg_idx, disk_idx);
	tv_metadata_save_kernel(ctx);
	return 0;
}

static int msg_show_errors(struct dm_target *ti, unsigned int argc,
			   char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	int i, off = 0;

	for (i = 0; i < ctx->ndisks && off < (int)maxlen - 2; i++) {
		off += snprintf(result + off, maxlen - off,
				"%s%s=%d", i > 0 ? " " : "",
				ctx->meta.disk_names[i],
				atomic_read(&ctx->deg.error_count[i]));
	}
	return 0;
}

static int msg_reset_errors(struct dm_target *ti, unsigned int argc,
			    char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	int i;

	for (i = 0; i < ctx->ndisks; i++)
		atomic_set(&ctx->deg.error_count[i], 0);
	pr_info("tieredvol: error counts reset\n");
	tv_log(TV_LOG_INFO, TV_LOG_CONFIG, "errors reset");
	return 0;
}

static int msg_set_error_threshold(struct dm_target *ti, unsigned int argc,
				   char **argv, char *result,
				   unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	u32 thresh;

	if (kstrtou32(argv[1], 10, &thresh) || thresh == 0)
		return -EINVAL;
	ctx->deg.error_threshold = thresh;
	pr_info("tieredvol: error_threshold=%u\n", thresh);
	tv_log(TV_LOG_INFO, TV_LOG_CONFIG, "err_thresh=%u", thresh);
	return 0;
}

static int msg_show_degraded(struct dm_target *ti, unsigned int argc,
			     char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	int i, off = 0;

	for (i = 0; i < ctx->ndisks && off < (int)maxlen - 2; i++) {
		off += snprintf(result + off, maxlen - off,
				"%s%s=%c(err=%d)", i > 0 ? " " : "",
				ctx->meta.disk_names[i],
				ctx->deg.degraded[i] ? 'D' : 'A',
				atomic_read(&ctx->deg.error_count[i]));
	}
	return 0;
}

static int msg_clear_degraded(struct dm_target *ti, unsigned int argc,
			      char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	int i, cleared = 0;

	for (i = 0; i < ctx->ndisks; i++) {
		if (ctx->deg.degraded[i]) {
			ctx->deg.degraded[i] = false;
			atomic_set(&ctx->deg.error_count[i], 0);
			cleared++;
			pr_info("tieredvol: disk[%d] %s cleared from DEGRADED\n",
				i, ctx->meta.disk_names[i]);
			tv_log(TV_LOG_INFO, TV_LOG_IO, "CLEARED degraded");
		}
	}
	snprintf(result, maxlen, "%d disk(s) cleared", cleared);
	return 0;
}

static int msg_start_rebuild(struct dm_target *ti, unsigned int argc,
			     char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	u32 seg_idx;
	u64 max_bytes = 0;

	if (kstrtou32(argv[1], 10, &seg_idx) ||
	    seg_idx >= ctx->meta.segment_count)
		return -EINVAL;
	if (argc >= 3) {
		if (kstrtou64(argv[2], 10, &max_bytes) || max_bytes == 0)
			return -EINVAL;
	}
	if (atomic_read(&ctx->rebuild.running))
		return -EBUSY;
	if (!ctx->meta.segments[seg_idx].mirror_enabled)
		return -EINVAL;

	ctx->rebuild.seg_idx = seg_idx;
	ctx->rebuild.offset = 0;
	ctx->rebuild.total =
		ctx->meta.segments[seg_idx].logical_end -
		ctx->meta.segments[seg_idx].logical_begin;
	if (max_bytes > 0 && max_bytes < ctx->rebuild.total)
		ctx->rebuild.total = max_bytes;
	atomic_set(&ctx->rebuild.running, 1);
	reinit_completion(&ctx->rebuild.done_r);
	reinit_completion(&ctx->rebuild.done_w);

	ctx->rebuild.thread = kthread_run(tv_rebuild_thread, ctx,
					  "tv_rebuild_%d", seg_idx);
	if (IS_ERR(ctx->rebuild.thread)) {
		atomic_set(&ctx->rebuild.running, 0);
		return PTR_ERR(ctx->rebuild.thread);
	}
	pr_info("tieredvol: rebuild started seg%u %llu bytes\n",
		seg_idx, ctx->rebuild.total);
	tv_log(TV_LOG_INFO,
	       TV_LOG_MIRROR, "rebuild start seg%u %llu bytes",
	       seg_idx, ctx->rebuild.total);
	return 0;
}

static int msg_stop_rebuild(struct dm_target *ti, unsigned int argc,
			    char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;

	if (!atomic_read(&ctx->rebuild.running))
		return 0;
	atomic_set(&ctx->rebuild.running, 0);
	complete(&ctx->rebuild.done_r);
	complete(&ctx->rebuild.done_w);
	if (!IS_ERR_OR_NULL(ctx->rebuild.thread)) {
		kthread_stop(ctx->rebuild.thread);
		ctx->rebuild.thread = NULL;
	}
	pr_info("tieredvol: rebuild stopped at %llu/%llu\n",
		ctx->rebuild.offset, ctx->rebuild.total);
	tv_log(TV_LOG_WARN, TV_LOG_MIRROR,
	       "rebuild stopped %llu/%llu",
	       ctx->rebuild.offset, ctx->rebuild.total);
	return 0;
}

static int msg_show_rebuild(struct dm_target *ti, unsigned int argc,
			    char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;

	if (atomic_read(&ctx->rebuild.running)) {
		u64 pct = ctx->rebuild.total ?
				  (ctx->rebuild.offset * 100 /
				   ctx->rebuild.total) :
				  0;

		snprintf(result, maxlen,
			 "rebuilding seg%d %llu/%llu (%llu%%)",
			 ctx->rebuild.seg_idx,
			 ctx->rebuild.offset, ctx->rebuild.total, pct);
	} else {
		snprintf(result, maxlen, "idle");
	}
	return 0;
}

static int msg_rebuild_badmap(struct dm_target *ti, unsigned int argc,
			      char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;

	if (atomic_read(&ctx->rebuild.running))
		return -EBUSY;
	tv_badmap_rebuild(ctx);
	snprintf(result, maxlen, "badmap rebuild done");
	return 0;
}

static int msg_set_badmap(struct dm_target *ti, unsigned int argc,
			  char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	u32 disk;
	u64 chunk_no;

	if (kstrtou32(argv[1], 10, &disk) ||
	    kstrtou64(argv[2], 10, &chunk_no) ||
	    disk >= (u32)ctx->ndisks)
		return -EINVAL;
	tv_badmap_set(ctx, disk, chunk_no);
	snprintf(result, maxlen, "badmap set disk=%u chunk=%llu",
		 disk, chunk_no);
	pr_info("tieredvol: badmap set disk=%u chunk=%llu\n", disk, chunk_no);
	return 0;
}

static int msg_clear_badmap(struct dm_target *ti, unsigned int argc,
			    char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	u32 disk;
	u64 chunk_no;

	if (kstrtou32(argv[1], 10, &disk) ||
	    kstrtou64(argv[2], 10, &chunk_no) ||
	    disk >= (u32)ctx->ndisks)
		return -EINVAL;
	tv_badmap_clear(ctx, disk, chunk_no);
	snprintf(result, maxlen, "badmap clear disk=%u chunk=%llu",
		 disk, chunk_no);
	pr_info("tieredvol: badmap clear disk=%u chunk=%llu\n", disk, chunk_no);
	return 0;
}

static int msg_show_badmap(struct dm_target *ti, unsigned int argc,
			   char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	int i, off = 0;

	for (i = 0; i < ctx->ndisks && off < (int)maxlen - 2; i++) {
		u64 count = 0;
		u64 c;

		if (!ctx->badmaps[i].bitmap)
			continue;
		for (c = 0; c < ctx->badmaps[i].n_chunks; c++) {
			if (test_bit(c, ctx->badmaps[i].bitmap))
				count++;
		}
		off += snprintf(result + off, maxlen - off,
				"%s%s=%llu/%llu", i > 0 ? " " : "",
				ctx->meta.disk_names[i],
				count, ctx->badmaps[i].n_chunks);
	}
	return 0;
}

/* clang-format off */
const struct tv_msg_handler tv_msg_mirror[] = {
	{ "show_mirror",      1, 1, msg_show_mirror },
	{ "set_mirror",       3, 3, msg_set_mirror },
	{ "show_errors",      1, 1, msg_show_errors },
	{ "reset_errors",     1, 1, msg_reset_errors },
	{ "set_error_threshold", 2, 2, msg_set_error_threshold },
	{ "show_degraded",    1, 1, msg_show_degraded },
	{ "clear_degraded",   1, 1, msg_clear_degraded },
	{ "start_rebuild",    2, 0, msg_start_rebuild },
	{ "stop_rebuild",     1, 1, msg_stop_rebuild },
	{ "show_rebuild",     1, 1, msg_show_rebuild },
	{ "rebuild_badmap",   1, 1, msg_rebuild_badmap },
	{ "set_badmap",       3, 3, msg_set_badmap },
	{ "clear_badmap",     3, 3, msg_clear_badmap },
	{ "show_badmap",      1, 1, msg_show_badmap },
};
/* clang-format on */

const int tv_msg_mirror_count = ARRAY_SIZE(tv_msg_mirror);
