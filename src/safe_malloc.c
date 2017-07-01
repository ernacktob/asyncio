#include "safe_malloc.h"
#include "logging.h"
#include "constants.h"
#ifndef MALLOC_IS_THREAD_SAFE
#include "threading.h"
#endif

#ifndef MALLOC_IS_THREAD_SAFE
static ASYNCIO_MUTEX_T malloc_mtx = ASYNCIO_MUTEX_INITIALIZER;
#endif

void *asyncio_safe_malloc(size_t count, size_t size)
{
	size_t total_size;
	void *ptr;

	ASYNCIO_DEBUG_ENTER(2 ARG("%lu", count) ARG("%lu", size));

	/* Check for overflows */
	if (size != 0) {
		if (count > SIZET_MAX / size) {
			ASYNCIO_ERROR("Integer overflow for size_t during malloc.\n");
			ASYNCIO_DEBUG_RETURN(RET("%p", NULL));
			return NULL;
		}
	}

	total_size = count * size;

#ifndef MALLOC_IS_THREAD_SAFE
	if (ASYNCIO_MUTEX_LOCK(&malloc_mtx) != 0) {
		ASYNCIO_ERROR("Failed to lock malloc_mtx.\n");
		ASYNCIO_DEBUG_RETURN(RET("%p", NULL));
		return NULL;
	}
#endif

	ASYNCIO_DEBUG_CALL(2 FUNC(malloc) ARG("%lu", total_size));
	ptr = malloc(total_size);

	if (ptr == NULL)
		ASYNCIO_SYSERROR("malloc");

#ifndef MALLOC_IS_THREAD_SAFE
	ASYNCIO_MUTEX_UNLOCK(&malloc_mtx);
#endif

	ASYNCIO_DEBUG_RETURN(RET("%p", ptr));
	return ptr;
}

void asyncio_safe_free(void *ptr)
{
	ASYNCIO_DEBUG_ENTER(1 ARG("%p", ptr));

#ifndef MALLOC_IS_THREAD_SAFE
	if (ASYNCIO_MUTEX_LOCK(&malloc_mtx) != 0) {
		ASYNCIO_ERROR("Failed to lock malloc_mtx.\n");
		ASYNCIO_DEBUG_RETURN(VOIDRET);
		return;
	}
#endif

	ASYNCIO_DEBUG_CALL(2 FUNC(free) ARG("%p", ptr));
	free(ptr);

#ifndef MALLOC_IS_THREAD_SAFE
	ASYNCIO_MUTEX_UNLOCK(&malloc_mtx);
#endif

	ASYNCIO_DEBUG_RETURN(VOIDRET);
}
