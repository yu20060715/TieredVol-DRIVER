// SPDX-License-Identifier: GPL-2.0-only
/*
 * tieredvol_msg_stats.c — Stats message handlers
 */
#include <linux/module.h>
#include <linux/string.h>
#include <linux/device-mapper.h>
#include "tieredvol.h"
#include "tieredvol_msg.h"

static int msg_reset_stats(struct dm_target *ti, unsigned int argc,
			   char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	int i;

	for (i = 0; i < ctx->ndisks; i++) {
		atomic64_set(&ctx->io.total_read_bytes[i], 0);
		atomic64_set(&ctx->io.total_write_bytes[i], 0);
		atomic64_set(&ctx->io.total_read_ops[i], 0);
		atomic64_set(&ctx->io.total_write_ops[i], 0);
		atomic64_set(&ctx->io.total_latency_ns[i], 0);
		atomic64_set(&ctx->io.total_completions[i], 0);
		atomic64_set(&ctx->io.interval_completions[i], 0);
	}
	snprintf(result, maxlen, "stats reset");
	return 0;
}

static int msg_show_stats(struct dm_target *ti, unsigned int argc,
			  char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	int i, off = 0;

	for (i = 0; i < ctx->ndisks && off < (int)maxlen - 2; i++) {
		off += snprintf(result + off, maxlen - off,
				"%s:rd=%llu/%llu wr=%llu/%llu",
				ctx->meta.disk_names[i],
				atomic64_read(&ctx->io.total_read_ops[i]),
				atomic64_read(&ctx->io.total_read_bytes[i]),
				atomic64_read(&ctx->io.total_write_ops[i]),
				atomic64_read(&ctx->io.total_write_bytes[i]));
		if (i + 1 < ctx->ndisks && off < (int)maxlen - 1)
			result[off++] = ' ';
	}
	return 0;
}

static int msg_status(struct dm_target *ti, unsigned int argc,
		      char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	int i, off = 0;

	for (i = 0; i < ctx->ndisks && off < (int)maxlen - 1; i++) {
		u32 w = 0;
		int si;

		for (si = 0; si < (int)ctx->meta.segment_count; si++) {
			struct tieredvol_segment *seg =
				&ctx->meta.segments[si];
			int j;

			for (j = 0; j < (int)seg->disk_count; j++) {
				if (seg->disk_index[j] == (u32)i) {
					w = seg->weight[j];
					goto found;
				}
			}
		}
found:
		off += snprintf(result + off, maxlen - off,
				"disk[%d]=%s(w=%u) ",
				i, ctx->meta.disk_names[i], w);
	}
	return 0;
}

static int msg_show_inflight(struct dm_target *ti, unsigned int argc,
			     char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	int i, off = 0;

	for (i = 0; i < ctx->ndisks && off < (int)maxlen - 1; i++) {
		off += snprintf(result + off, maxlen - off,
				"%s%s=%u", i > 0 ? " " : "",
				ctx->meta.disk_names[i],
				atomic_read(&ctx->io.in_flight_bytes[i]));
	}
	pr_info("tieredvol: %s\n", result);
	return 0;
}

static int msg_show_io_stats(struct dm_target *ti, unsigned int argc,
			     char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	int i, off = 0;

	for (i = 0; i < ctx->ndisks && off < (int)maxlen - 2; i++) {
		off += snprintf(result + off, maxlen - off,
				"%s%s:rd=%llu/%llu wr=%llu/%llu",
				i > 0 ? " " : "",
				ctx->meta.disk_names[i],
				atomic64_read(&ctx->io.total_read_ops[i]),
				atomic64_read(&ctx->io.total_read_bytes[i]),
				atomic64_read(&ctx->io.total_write_ops[i]),
				atomic64_read(&ctx->io.total_write_bytes[i]));
	}
	pr_info("tieredvol: %s\n", result);
	return 0;
}

static int msg_reset_io_stats(struct dm_target *ti, unsigned int argc,
			      char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	int i;

	for (i = 0; i < ctx->ndisks; i++) {
		atomic64_set(&ctx->io.total_read_bytes[i], 0);
		atomic64_set(&ctx->io.total_write_bytes[i], 0);
		atomic64_set(&ctx->io.total_read_ops[i], 0);
		atomic64_set(&ctx->io.total_write_ops[i], 0);
		atomic64_set(&ctx->io.total_latency_ns[i], 0);
		atomic64_set(&ctx->io.total_completions[i], 0);
		atomic64_set(&ctx->io.interval_completions[i], 0);
	}
	pr_info("tieredvol: IO stats reset\n");
	return 0;
}

/* clang-format off */
const struct tv_msg_handler tv_msg_stats[] = {
	{ "reset_stats",      1, 1, msg_reset_stats },
	{ "show_stats",       1, 1, msg_show_stats },
	{ "status",           1, 1, msg_status },
	{ "show_inflight",    1, 1, msg_show_inflight },
	{ "show_io_stats",    1, 1, msg_show_io_stats },
	{ "reset_io_stats",   1, 1, msg_reset_io_stats },
};
/* clang-format on */

const int tv_msg_stats_count = ARRAY_SIZE(tv_msg_stats);
