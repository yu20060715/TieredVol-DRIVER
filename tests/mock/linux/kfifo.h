/* Minimal mock of <linux/kfifo.h>. */
#ifndef _MOCK_LINUX_KFIFO_H
#define _MOCK_LINUX_KFIFO_H

struct kfifo {
	unsigned char *buffer;
	unsigned int size;
	unsigned int in;
	unsigned int out;
};

#endif
