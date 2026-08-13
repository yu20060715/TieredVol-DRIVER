/* Minimal mock of <linux/types.h> — userspace type aliases.
 * Part of the kernel-source-in-userspace-test feasibility prototype. */
#ifndef _MOCK_LINUX_TYPES_H
#define _MOCK_LINUX_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#define __rcu

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;
typedef uint64_t sector_t;
typedef uint32_t blk_status_t;
typedef uint32_t gfp_t;

#define NULL ((void *)0)

#endif
