/* Minimal mock of <linux/workqueue.h> for the feasibility prototype. */
#ifndef _MOCK_LINUX_WORKQUEUE_H
#define _MOCK_LINUX_WORKQUEUE_H

#include "types.h"
#include "timer.h"

struct work_struct {
	unsigned long data;
};

struct delayed_work {
	struct work_struct work;
	struct timer_list timer;
};

typedef void *mempool_t;

#define __percpu

#endif
