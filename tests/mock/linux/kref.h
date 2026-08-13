/* Minimal mock of <linux/kref.h> for the feasibility prototype. */
#ifndef _MOCK_LINUX_KREF_H
#define _MOCK_LINUX_KREF_H

#include "atomic.h"

struct kref {
	atomic_t refcount;
};

static inline void kref_init(struct kref *k)
{
	atomic_set(&k->refcount, 1);
}

static inline void kref_get(struct kref *k)
{
	atomic_inc(&k->refcount);
}

static inline int kref_put(struct kref *k, void (*release)(struct kref *))
{
	if (atomic_dec_and_test(&k->refcount)) {
		release(k);
		return 1;
	}
	return 0;
}

static inline int kref_get_unless_zero(struct kref *k)
{
	int r = atomic_read(&k->refcount);

	while (r > 0) {
		if (__sync_bool_compare_and_swap(&k->refcount.counter, r, r + 1))
			return 1;
		r = atomic_read(&k->refcount);
	}
	return 0;
}

#endif
