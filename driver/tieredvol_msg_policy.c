// SPDX-License-Identifier: GPL-2.0-only
/*
 * tieredvol_msg_policy.c — Adaptive / policy / wear message handlers
 */
#include <linux/module.h>
#include <linux/string.h>
#include <linux/device-mapper.h>
#include "tieredvol.h"
#include "tieredvol_msg.h"

static int msg_adaptive_on(struct dm_target *ti, unsigned int argc,
			   char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;

	ctx->adaptive.policy = TV_POLICY_ADAPTIVE;
	pr_info("tieredvol: policy = adaptive\n");
	tv_log(TV_LOG_INFO, 0, TV_LOG_CONFIG, "policy=adaptive");
	tv_metadata_save_kernel(ctx);
	return 0;
}

static int msg_adaptive_off(struct dm_target *ti, unsigned int argc,
			    char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;

	ctx->adaptive.policy = TV_POLICY_STATIC;
	pr_info("tieredvol: policy = static\n");
	tv_log(TV_LOG_INFO, 0, TV_LOG_CONFIG, "policy=static");
	tv_metadata_save_kernel(ctx);
	return 0;
}

static int msg_set_policy(struct dm_target *ti, unsigned int argc,
			  char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;

	if (strcmp(argv[1], "static") == 0)
		ctx->adaptive.policy = TV_POLICY_STATIC;
	else if (strcmp(argv[1], "adaptive") == 0)
		ctx->adaptive.policy = TV_POLICY_ADAPTIVE;
	else if (strcmp(argv[1], "random") == 0)
		ctx->adaptive.policy = TV_POLICY_RANDOM;
	else
		return -EINVAL;
	pr_info("tieredvol: policy = %s\n", argv[1]);
	tv_log(TV_LOG_INFO, 0, TV_LOG_CONFIG, "policy=%s", argv[1]);
	tv_metadata_save_kernel(ctx);
	return 0;
}

static int msg_set_ema_shift(struct dm_target *ti, unsigned int argc,
			     char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	u32 shift;

	if (kstrtou32(argv[1], 10, &shift) || shift > 10)
		return -EINVAL;
	ctx->adaptive.ema_weight_shift = shift;
	pr_info("tieredvol: ema_weight_shift=%u (alpha=%u/1024)\n",
		shift, 1 << shift);
	tv_log(TV_LOG_INFO, 0, TV_LOG_CONFIG, "ema_shift=%u", shift);
	tv_metadata_save_kernel(ctx);
	return 0;
}

static int msg_set_stale_ms(struct dm_target *ti, unsigned int argc,
			    char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	u32 ms;

	if (kstrtou32(argv[1], 10, &ms))
		return -EINVAL;
	ctx->adaptive.stale_after_ns = (u64)ms * 1000000ULL;
	pr_info("tieredvol: stale_after=%ums\n", ms);
	tv_log(TV_LOG_INFO, 0, TV_LOG_CONFIG, "stale_ms=%u", ms);
	tv_metadata_save_kernel(ctx);
	return 0;
}

static int msg_show_adaptive(struct dm_target *ti, unsigned int argc,
			     char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	int i, off = 0;

	off += snprintf(result + off, maxlen - off,
			"policy=%d ema_shift=%u stale_ms=%llu wear_bias=%u",
			ctx->adaptive.policy,
			ctx->adaptive.ema_weight_shift,
			ctx->adaptive.stale_after_ns / 1000000ULL,
			ctx->adaptive.wear_bias);
	for (i = 0; i < ctx->ndisks && off < (int)maxlen - 1; i++) {
		off += snprintf(result + off, maxlen - off,
				" %s:load=%llu lat=%lluus writes=%llu stale=%d",
				ctx->meta.disk_names[i],
				ctx->adaptive.ema_load[i],
				ctx->adaptive.ema_latency_ns[i] / 1000ULL,
				atomic64_read(&ctx->io.total_write_bytes[i]),
				ctx->adaptive.stale[i]);
	}
	pr_info("tieredvol: %s\n", result);
	return 0;
}

static int msg_show_wear(struct dm_target *ti, unsigned int argc,
			 char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	int i, off = 0;

	off += snprintf(result + off, maxlen - off,
			"wear_bias=%u", ctx->adaptive.wear_bias);
	for (i = 0; i < ctx->ndisks && off < (int)maxlen - 2; i++) {
		off += snprintf(result + off, maxlen - off,
				" %s=%llu",
				ctx->meta.disk_names[i],
				atomic64_read(&ctx->io.total_write_bytes[i]));
	}
	pr_info("tieredvol: %s\n", result);
	return 0;
}

static int msg_set_wear_bias(struct dm_target *ti, unsigned int argc,
			     char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	u32 bias;

	if (kstrtou32(argv[1], 10, &bias) || bias > 1024)
		return -EINVAL;
	ctx->adaptive.wear_bias = bias;
	pr_info("tieredvol: wear_bias=%u\n", bias);
	tv_log(TV_LOG_INFO, 0, TV_LOG_CONFIG, "wear_bias=%u", bias);
	tv_metadata_save_kernel(ctx);
	return 0;
}

static int msg_reset_wear(struct dm_target *ti, unsigned int argc,
			  char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	int i;

	for (i = 0; i < ctx->ndisks; i++)
		atomic64_set(&ctx->io.total_write_bytes[i], 0);
	pr_info("tieredvol: wear counters reset\n");
	tv_log(TV_LOG_INFO, 0, TV_LOG_CONFIG, "wear reset");
	return 0;
}

static int msg_set_seg_policy(struct dm_target *ti, unsigned int argc,
			      char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	u32 seg_idx;
	int pol;

	if (kstrtou32(argv[1], 10, &seg_idx))
		return -EINVAL;
	if (seg_idx >= ctx->meta.segment_count)
		return -EINVAL;

	if (strcmp(argv[2], "inherit") == 0)
		pol = -1;
	else if (strcmp(argv[2], "static") == 0)
		pol = TV_POLICY_STATIC;
	else if (strcmp(argv[2], "adaptive") == 0)
		pol = TV_POLICY_ADAPTIVE;
	else if (strcmp(argv[2], "random") == 0)
		pol = TV_POLICY_RANDOM;
	else
		return -EINVAL;

	ctx->meta.segments[seg_idx].policy = pol;
	pr_info("tieredvol: seg%u policy=%d\n", seg_idx, pol);
	tv_log(TV_LOG_INFO, 0, TV_LOG_CONFIG,
	       "seg%u policy=%d", seg_idx, pol);
	tv_metadata_save_kernel(ctx);
	return 0;
}

/* clang-format off */
const struct tv_msg_handler tv_msg_policy[] = {
	{ "adaptive_on",      1, 1, msg_adaptive_on },
	{ "adaptive_off",     1, 1, msg_adaptive_off },
	{ "set_policy",       2, 2, msg_set_policy },
	{ "set_seg_policy",   3, 3, msg_set_seg_policy },
	{ "set_ema_shift",    2, 2, msg_set_ema_shift },
	{ "set_stale_ms",     2, 2, msg_set_stale_ms },
	{ "show_adaptive",    1, 1, msg_show_adaptive },
	{ "show_wear",        1, 1, msg_show_wear },
	{ "set_wear_bias",    2, 2, msg_set_wear_bias },
	{ "reset_wear",       1, 1, msg_reset_wear },
};
/* clang-format on */

const int tv_msg_policy_count = ARRAY_SIZE(tv_msg_policy);
