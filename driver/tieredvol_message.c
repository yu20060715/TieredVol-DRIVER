// SPDX-License-Identifier: GPL-2.0-only
/*
 * tieredvol_message.c — dmsetup message handler dispatch
 *
 * Handlers are split by domain into:
 *   tieredvol_msg_stats.c   — Stats / IO counters
 *   tieredvol_msg_policy.c  — Adaptive policy / wear
 *   tieredvol_msg_mirror.c  — Mirror / rebuild / degradation
 *   tieredvol_msg_config.c  — Log / config
 *
 * Metadata save lives in tieredvol_meta.c.
 */
#include <linux/module.h>
#include <linux/string.h>
#include <linux/device-mapper.h>
#include "tieredvol.h"
#include "tieredvol_msg.h"

/* ---- Combined dispatch table ---- */

/* clang-format off */
static const struct tv_msg_handler *tv_all_handlers[] = {
	tv_msg_stats,
	tv_msg_policy,
	tv_msg_mirror,
	tv_msg_config,
};
/* clang-format on */

#define TV_HANDLER_TABLES (sizeof(tv_all_handlers) / sizeof(tv_all_handlers[0]))

int tieredvol_message(struct dm_target *ti, unsigned int argc, char **argv,
		      char *result, unsigned int maxlen)
{
	unsigned int t, i;
	const int counts[] = {
		tv_msg_stats_count,
		tv_msg_policy_count,
		tv_msg_mirror_count,
		tv_msg_config_count,
	};

	for (t = 0; t < TV_HANDLER_TABLES; t++) {
		const struct tv_msg_handler *tbl = tv_all_handlers[t];

		for (i = 0; i < (unsigned int)counts[t]; i++) {
			const struct tv_msg_handler *h = &tbl[i];

			if (strcmp(argv[0], h->name) != 0)
				continue;
			if (argc < h->min_argc)
				return -EINVAL;
			if (h->max_argc > 0 && argc > h->max_argc)
				return -EINVAL;
			return h->fn(ti, argc, argv, result, maxlen);
		}
	}

	return -EINVAL;
}
