// SPDX-License-Identifier: GPL-2.0-only
/*
 * tieredvol_msg_config.c — Log / config message handlers
 */
#include <linux/module.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/kfifo.h>
#include <linux/device-mapper.h>
#include "tieredvol.h"
#include "tieredvol_msg.h"

static int msg_show_log(struct dm_target *ti, unsigned int argc,
			char **argv, char *result, unsigned int maxlen)
{
	struct tv_log_entry *entries;
	unsigned long flags;
	int cnt = 0;
	int i;

	entries = kmalloc_array(64, sizeof(*entries), GFP_KERNEL);
	if (!entries)
		return -ENOMEM;

	raw_spin_lock_irqsave(&tv_log_lock, flags);
	while (cnt < 64 && kfifo_out(&tv_log_fifo, &entries[cnt], sizeof(entries[0])))
		cnt++;
	for (i = 0; i < cnt; i++)
		kfifo_in(&tv_log_fifo, &entries[i], sizeof(entries[0]));
	raw_spin_unlock_irqrestore(&tv_log_lock, flags);

	for (i = 0; i < cnt; i++)
		pr_info("tieredvol: LOG [%lld.%06lld] %s %s: %s\n",
			entries[i].timestamp_ns / 1000000000ULL,
			(entries[i].timestamp_ns / 1000ULL) % 1000000ULL,
			entries[i].level == TV_LOG_ERR ? "ERR" :
			entries[i].level == TV_LOG_WARN ? "WRN" : "INF",
			entries[i].event_type == TV_LOG_STALE ? "STALE" :
			entries[i].event_type == TV_LOG_RECOVER ? "RCVR" :
			entries[i].event_type == TV_LOG_MIRROR ? "MIRR" :
			entries[i].event_type == TV_LOG_CONFIG ? "CONF" :
			entries[i].event_type == TV_LOG_IO ? "I/O" : "???",
			entries[i].msg);

	kfree(entries);

	if (cnt == 0)
		pr_info("tieredvol: LOG EMPTY\n");
	else
		pr_info("tieredvol: LOG DUMPED %d entries\n", cnt);
	return 0;
}

static int msg_clear_log(struct dm_target *ti, unsigned int argc,
			 char **argv, char *result, unsigned int maxlen)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&tv_log_lock, flags);
	kfifo_reset(&tv_log_fifo);
	raw_spin_unlock_irqrestore(&tv_log_lock, flags);
	pr_info("tieredvol: log cleared\n");
	return 0;
}

static int msg_set_loglevel(struct dm_target *ti, unsigned int argc,
			    char **argv, char *result, unsigned int maxlen)
{
	u32 lvl;

	if (kstrtou32(argv[1], 10, &lvl) || lvl > TV_LOG_INFO)
		return -EINVAL;
	tv_log_level = lvl;
	pr_info("tieredvol: loglevel = %u\n", tv_log_level);
	return 0;
}

/* clang-format off */
const struct tv_msg_handler tv_msg_config[] = {
	{ "show_log",         1, 1, msg_show_log },
	{ "clear_log",        1, 1, msg_clear_log },
	{ "set_loglevel",     2, 2, msg_set_loglevel },
};
/* clang-format on */

const int tv_msg_config_count = ARRAY_SIZE(tv_msg_config);
