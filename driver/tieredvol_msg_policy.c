// SPDX-License-Identifier: GPL-2.0-only
/*
 * tieredvol_msg_policy.c — Policy and weight-borrowing message handlers
 */
#include <linux/module.h>
#include <linux/string.h>
#include <linux/device-mapper.h>
#include "tieredvol.h"
#include "tieredvol_msg.h"

static int msg_set_policy(struct dm_target *ti, unsigned int argc,
			  char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;

	if (strcmp(argv[1], "static") == 0)
		ctx->policy = TV_POLICY_STATIC;
	else if (strcmp(argv[1], "random") == 0)
		ctx->policy = TV_POLICY_RANDOM;
	else
		return -EINVAL;
	pr_info("tieredvol: policy = %s\n", argv[1]);
	tv_log(TV_LOG_INFO, 0, TV_LOG_CONFIG, "policy=%s", argv[1]);
	tv_metadata_save_kernel(ctx);
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

static int msg_borrow_on(struct dm_target *ti, unsigned int argc,
			 char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;

	ctx->meta.runtime_borrow_enable = 1;
	if (ctx->borrow.entries) {
		ctx->borrow.enabled = true;
		pr_info("tieredvol: borrow enabled\n");
	} else {
		pr_info("tieredvol: borrow enable requested "
			"(applied on reload)\n");
	}
	tv_log(TV_LOG_INFO, 0, TV_LOG_CONFIG, "borrow_enable=1");
	tv_metadata_save_kernel(ctx);
	return 0;
}

static int msg_borrow_off(struct dm_target *ti, unsigned int argc,
			  char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;

	ctx->meta.runtime_borrow_enable = 0;
	ctx->borrow.enabled = false;
	pr_info("tieredvol: borrow disabled (existing borrowed blocks still "
		"resolve to their borrow-area copy)\n");
	tv_log(TV_LOG_INFO, 0, TV_LOG_CONFIG, "borrow_enable=0");
	tv_metadata_save_kernel(ctx);
	return 0;
}

static int msg_show_borrow(struct dm_target *ti, unsigned int argc,
			   char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	int i, off = 0;

	off += snprintf(result + off, maxlen - off,
			"enabled=%d watermark=%u borrowed=%llu",
			ctx->borrow.enabled,
			ctx->borrow.watermark_bytes,
			(unsigned long long)ctx->borrow.n_borrowed);
	for (i = 0; i < ctx->ndisks && off < (int)maxlen - 2; i++) {
		off += snprintf(result + off, maxlen - off,
				" %s:area=%u used=%u borrowedB=%llu",
				ctx->meta.disk_names[i],
				ctx->borrow.area_blocks[i],
				ctx->borrow.used_blocks[i],
				atomic64_read(&ctx->borrow.borrow_write_bytes[i]));
	}
	return 0;
}

/* clang-format off */
const struct tv_msg_handler tv_msg_policy[] = {
	{ "set_policy",       2, 2, msg_set_policy },
	{ "set_seg_policy",   3, 3, msg_set_seg_policy },
	{ "borrow_on",        1, 1, msg_borrow_on },
	{ "borrow_off",       1, 1, msg_borrow_off },
	{ "show_borrow",      1, 1, msg_show_borrow },
};
/* clang-format on */

const int tv_msg_policy_count = ARRAY_SIZE(tv_msg_policy);
