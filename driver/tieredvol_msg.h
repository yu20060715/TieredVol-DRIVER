/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * tieredvol_msg.h — Internal declarations for message handler domains.
 *
 * Each domain file (msg_stats, msg_policy, msg_mirror, msg_config)
 * implements its handlers and declares them here. The main dispatch
 * table in tieredvol_message.c references them via this header.
 */
#ifndef TIEREDVOL_MSG_H
#define TIEREDVOL_MSG_H

#include <linux/device-mapper.h>

typedef int (*tv_msg_fn)(struct dm_target *ti, unsigned int argc,
			 char **argv, char *result, unsigned int maxlen);

struct tv_msg_handler {
	const char *name;
	int min_argc;
	int max_argc; /* 0 = unlimited */
	tv_msg_fn fn;
};

/* Stats handlers (tieredvol_msg_stats.c) */
extern const struct tv_msg_handler tv_msg_stats[];
extern const int tv_msg_stats_count;

/* Policy / borrow handlers (tieredvol_msg_policy.c) */
extern const struct tv_msg_handler tv_msg_policy[];
extern const int tv_msg_policy_count;

/* Mirror / rebuild / degradation handlers (tieredvol_msg_mirror.c) */
extern const struct tv_msg_handler tv_msg_mirror[];
extern const int tv_msg_mirror_count;

/* Log / config handlers (tieredvol_msg_config.c) */
extern const struct tv_msg_handler tv_msg_config[];
extern const int tv_msg_config_count;

#endif
