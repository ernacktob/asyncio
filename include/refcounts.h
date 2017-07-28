#ifndef REFCOUNTS_H
#define REFCOUNTS_H

#include "threading.h"

struct refcount_base {
	ASYNCIO_MUTEX_T mtx;
	unsigned long refcount;

	void *instance;
	void (*destructor)(void *instance);
};

int asyncio_refcount_init(struct refcount_base *base, void *instance, void (*destructor)(void *instance), unsigned long init_refcount);
void asyncio_refcount_deinit(struct refcount_base *base); /* Cleanup imemdiately after creating it, without calling destructor (used in case of early errors) */

int asyncio_refcount_acquire(struct refcount_base *base);
void asyncio_refcount_release(struct refcount_base *base);

#endif
