/* Minimal mocks of remaining kernel headers referenced by tieredvol.h. */
#ifndef _MOCK_LINUX_LIST_H
#define _MOCK_LINUX_LIST_H

#include <stddef.h>
#include "types.h"

struct list_head {
	struct list_head *next, *prev;
};

#endif
