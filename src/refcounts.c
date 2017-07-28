#include <limits.h>

#include "refcounts.h"
#include "threading.h"
#include "logging.h"

/* Note: this is not a public module. In particular, it is not cancellation aware, so caller must take care of this. */

int asyncio_refcount_init(struct refcount_base *base, void *instance, void (*destructor)(void *instance), unsigned long init_refcount)
{
	if (ASYNCIO_MUTEX_INIT(&base->mtx) != 0) {
		ASYNCIO_ERROR("Failed to init refcount base mtx.\n");
		return -1;
	}

	base->refcount = init_refcount;
	base->instance = instance;
	base->destructor = destructor;

	return 0;
}

void asyncio_refcount_deinit(struct refcount_base *base)
{
	ASYNCIO_MUTEX_DESTROY(&base->mtx);
}

int asyncio_refcount_acquire(struct refcount_base *base)
{
	/* Note: In general, only one who owns a reference to handle can acquire it, for someone else.
	 * This is to prevent race-conditions where last owner releases reference while someone else
	 * acquires it (if the acquirer loses the lock race, they get dangling pointer) */
	if (ASYNCIO_MUTEX_LOCK(&base->mtx) != 0) {
		ASYNCIO_ERROR("Failed to lock refcount base mtx.\n");
		return -1;
	}

	if (base->refcount == ULONG_MAX) {
		ASYNCIO_ERROR("Max refcount reached for refcount base.\n");
		ASYNCIO_MUTEX_UNLOCK(&base->mtx);
		return -1;
	}

	++(base->refcount);

	ASYNCIO_MUTEX_UNLOCK(&base->mtx);
	return 0;
}

void asyncio_refcount_release(struct refcount_base *base)
{
	if (ASYNCIO_MUTEX_LOCK(&base->mtx) != 0) {
		ASYNCIO_ERROR("Failed to lock refcount base mtx.\n");
		return;
	}

	if (base->refcount == 0) {
		ASYNCIO_ERROR("The refcount for refcount base is already 0.\n");
		ASYNCIO_MUTEX_UNLOCK(&base->mtx);
		return;
	}

	--(base->refcount);

	if (base->refcount > 0) {
		ASYNCIO_MUTEX_UNLOCK(&base->mtx);
		return;
	}

	ASYNCIO_MUTEX_UNLOCK(&base->mtx);
	ASYNCIO_MUTEX_DESTROY(&base->mtx);

	base->destructor(base->instance);
}
