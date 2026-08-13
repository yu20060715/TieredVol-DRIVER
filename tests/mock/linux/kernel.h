/* Minimal mock of <linux/kernel.h> for the feasibility prototype. */
#ifndef _MOCK_LINUX_KERNEL_H
#define _MOCK_LINUX_KERNEL_H

#include <stdio.h>
#include <stddef.h>

#define container_of(ptr, type, member) ({				\
		const typeof(((type *)0)->member) *__mptr = (ptr);	\
		(type *)((char *)__mptr - offsetof(type, member)); })

#define HZ 1000UL

#define pr_warn(fmt, ...) fprintf(stderr, "tieredvol: " fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...) fprintf(stderr, "tieredvol: " fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...)  fprintf(stderr, "tieredvol: " fmt, ##__VA_ARGS__)

#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#endif
