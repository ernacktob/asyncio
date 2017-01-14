#include "safe_malloc.h"
#include "logging.h"

#ifndef MALLOC_IS_THREAD_SAFE
void *malloc_locked(size_t size)
{
	static pthread_mutex_t malloc_mtx = PTHREAD_MUTEX_INITIALIZER;
	void *ptr;
	int rc;

	ASYNCIO_DEBUG_ENTER(1 ARG("%lu", size));

	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_lock) ARG("%p", &malloc_mtx));
	if ((rc = pthread_mutex_lock(&malloc_mtx)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_mutex_lock");
		ASYNCIO_DEBUG_RETURN(RET("%p", NULL));
		return NULL;
	}

	ASYNCIO_DEBUG_CALL(2 FUNC(malloc) ARG("%lu", size));
	ptr = malloc(size);

	if (ptr == NULL)
		ASYNCIO_SYSERROR("malloc");

	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_unlock) ARG("%p", &malloc_mtx));
	if ((rc = pthread_mutex_unlock(&malloc_mtx) != 0)) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_mutex_unlock");
	}

	ASYNCIO_DEBUG_RETURN(RET("%p", ptr));
	return ptr;
}
#endif

#ifndef FREE_IS_THREAD_SAFE
void free_locked(void *ptr)
{
	static pthread_mutex_t free_mtx = PTHREAD_MUTEX_INITIALIZER;
	int rc;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", ptr));

	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_lock) ARG("%p", &free_mtx));
	if ((rc = pthread_mutex_lock(&free_mtx)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_mutex_lock");
		ASYNCIO_DEBUG_RETURN(VOIDRET);
		return;
	}

	ASYNCIO_DEBUG_CALL(2 FUNC(free) ARG("%p", ptr));
	free(ptr);

	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_unlock) ARG("%p", &free_mtx));
	if ((rc = pthread_mutex_unlock(&free_mtx)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_mutex_unlock");
	}

	ASYNCIO_DEBUG_RETURN(VOIDRET);
}
#endif
