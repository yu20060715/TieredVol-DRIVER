/* Minimal mock of <linux/device-mapper.h> for the feasibility prototype.
 * Only forward declarations and the structs referenced by tieredvol.h. */
#ifndef _MOCK_LINUX_DEVICE_MAPPER_H
#define _MOCK_LINUX_DEVICE_MAPPER_H

#include "types.h"
#include "spinlock.h"
#include "list.h"
#include "workqueue.h"
#include "rcupdate.h"

struct dm_target;

struct dm_dev {
	void *bdev;
};

#endif
