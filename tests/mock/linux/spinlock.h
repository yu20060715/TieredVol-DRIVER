/* Minimal mocks of remaining kernel headers referenced by tieredvol.h. */
#ifndef _MOCK_LINUX_SPINLOCK_H
#define _MOCK_LINUX_SPINLOCK_H

#include "types.h"

typedef struct {
	int locked;
} spinlock_t;

typedef struct {
	int locked;
} raw_spinlock_t;

#define spin_lock_init(l) ((l)->locked = 0)
#define spin_lock(l)      ((l)->locked = 1)
#define spin_unlock(l)    ((l)->locked = 0)
#define spin_lock_irqsave(l, f)  do { (f) = 0; (l)->locked = 1; } while (0)
#define spin_unlock_irqrestore(l, f) do { (l)->locked = 0; (void)(f); } while (0)

#endif
