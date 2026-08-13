/* Minimal mock of <linux/completion.h> for the feasibility prototype. */
#ifndef _MOCK_LINUX_COMPLETION_H
#define _MOCK_LINUX_COMPLETION_H

#include "types.h"

struct completion {
	unsigned int done;
};

#endif
