/* Minimal mock of <linux/fs.h> (block_device only). */
#ifndef _MOCK_LINUX_FS_H
#define _MOCK_LINUX_FS_H

#include "types.h"

struct block_device {
	int dummy;
};

#endif
