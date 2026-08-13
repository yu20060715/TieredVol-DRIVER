/* Minimal mock of <linux/slab.h> for the feasibility prototype. */
#ifndef _MOCK_LINUX_SLAB_H
#define _MOCK_LINUX_SLAB_H

#include <stdlib.h>

#define GFP_NOIO  0
#define GFP_KERNEL 0

#define kmalloc(size, flags)  malloc(size)
#define kzalloc(size, flags)  calloc(1, size)
#define kfree(p)              free(p)

#endif
