/* Minimal mock of <linux/atomic.h> for the feasibility prototype.
 * Uses GCC __sync builtins (single-threaded tests: exact ordering of
 * the ops matters more than memory-model fidelity). */
#ifndef _MOCK_LINUX_ATOMIC_H
#define _MOCK_LINUX_ATOMIC_H

#include <stdint.h>

typedef struct { int counter; } atomic_t;
typedef struct { long long counter; } atomic64_t;

#define atomic_read(a)          ((a)->counter)
#define atomic_set(a, v)        ((a)->counter = (v))
#define atomic_add(v, a)        __sync_fetch_and_add(&(a)->counter, (v))
#define atomic_sub(v, a)        __sync_fetch_and_sub(&(a)->counter, (v))
#define atomic_inc(a)           __sync_fetch_and_add(&(a)->counter, 1)
#define atomic_dec_and_test(a)  (__sync_sub_and_fetch(&(a)->counter, 1) == 0)
#define atomic_cmpxchg(a, o, n) __sync_val_compare_and_swap(&(a)->counter, (o), (n))

#define atomic64_read(a)          ((a)->counter)
#define atomic64_set(a, v)        ((a)->counter = (v))
#define atomic64_add(v, a)        __sync_fetch_and_add(&(a)->counter, (v))
#define atomic64_inc(a)           __sync_fetch_and_add(&(a)->counter, 1)

#endif
