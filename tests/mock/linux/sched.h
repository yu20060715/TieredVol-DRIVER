/* Minimal mock of <linux/sched.h>. */
#ifndef _MOCK_LINUX_SCHED_H
#define _MOCK_LINUX_SCHED_H

#include "types.h"

struct task_struct {
	int pid;
};

#endif
