/* Minimal mock of <linux/timer.h> for the feasibility prototype.
 * Tracks arming state only; timers are never auto-fired in userspace.
 * tv_parallel_timeout can be invoked directly by the test. */
#ifndef _MOCK_LINUX_TIMER_H
#define _MOCK_LINUX_TIMER_H

#include <stddef.h>
#include "kernel.h"

struct timer_list {
	void (*function)(struct timer_list *);
	unsigned long expires;
	int armed;
};

#define timer_setup(t, cb, flags) do {					\
		(t)->function = (cb);					\
		(t)->armed = 0;						\
	} while (0)

#define mod_timer(t, exp) do {						\
		(t)->expires = (exp);					\
		(t)->armed = 1;						\
	} while (0)

#define del_timer(t) ((void)((t)->armed = 0))

#define from_timer(var, callback_timer, member)			\
	container_of(callback_timer, typeof(*var), member)

#endif
