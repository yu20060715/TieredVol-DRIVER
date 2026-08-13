// SPDX-License-Identifier: GPL-2.0-only
/*
 * tieredvol_log.c — Log ring buffer
 *
 * Extracted from tieredvol_core.c in Phase 1 refactoring.
 */
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/spinlock.h>
#include <linux/kfifo.h>
#include <linux/stdarg.h>
#include <linux/ktime.h>
#include <linux/jiffies.h>
#include <linux/timer.h>

#include "tieredvol.h"

/* ---- Log ring buffer ---- */

unsigned int log_size = TV_LOG_SIZE;
module_param(log_size, uint, 0644);
MODULE_PARM_DESC(log_size, "Ring buffer log entries (default 512, power of 2)");

struct kfifo tv_log_fifo;

raw_spinlock_t tv_log_lock = __RAW_SPIN_LOCK_UNLOCKED(tv_log_lock);

u8 tv_log_level = TV_LOG_INFO;

void tv_log(u8 level, u8 event_type, const char *fmt, ...)
{
	struct tv_log_entry entry;
	va_list args;
	unsigned long flags;

	if (level > tv_log_level)
		return;

	entry.timestamp_ns = ktime_get_ns();
	entry.level = level;
	entry.event_type = event_type;

	va_start(args, fmt);
	vsnprintf(entry.msg, sizeof(entry.msg), fmt, args);
	va_end(args);

	raw_spin_lock_irqsave(&tv_log_lock, flags);
	kfifo_in(&tv_log_fifo, &entry, sizeof(entry));
	raw_spin_unlock_irqrestore(&tv_log_lock, flags);
}

