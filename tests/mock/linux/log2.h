/* Minimal mock of <linux/log2.h>. */
#ifndef _MOCK_LINUX_LOG2_H
#define _MOCK_LINUX_LOG2_H

static inline unsigned int ilog2(unsigned int n)
{
	unsigned int r = 0;

	while (n >>= 1)
		r++;
	return r;
}

#endif
