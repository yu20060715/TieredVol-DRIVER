/* Minimal mocks of misc kernel headers needed by tieredvol.h.
 * Only type shapes required for the feasibility prototype. */
#ifndef _MOCK_LINUX_RCUPDATE_H
#define _MOCK_LINUX_RCUPDATE_H

#include "types.h"
#include "workqueue.h"

struct rcu_head {
	struct rcu_head *next;
	void (*func)(struct rcu_head *);
};

#endif
