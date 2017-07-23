#ifndef ASYNCIO_THREADPOOL_H
#define ASYNCIO_THREADPOOL_H

#include <stdint.h>

#define ASYNCIO_THREADPOOL_FLAG_NONE		0
#define ASYNCIO_THREADPOOL_FLAG_CONTRACTOR	(1 << 0)
#define ASYNCIO_THREADPOOL_FLAG_CANCELLABLE	(1 << 1)
#define ASYNCIO_THREADPOOL_FLAG_ASYNCCANCEL	(1 << 2)

struct asyncio_threadpool_handle;

typedef void (*asyncio_threadpool_function)(void *arg);
typedef void (*asyncio_threadpool_callback)(void *arg);

struct asyncio_threadpool_function_info {
	asyncio_threadpool_function fn;
	void *arg;
};

struct asyncio_threadpool_callback_info {
	asyncio_threadpool_callback cb;
	void *arg;
};

struct asyncio_threadpool_dispatch_info {
	uint32_t flags;
	struct asyncio_threadpool_function_info dispatch_info;          /* Function to be executed asynchronously by the thread pool. Cannot be NULL. */
	struct asyncio_threadpool_callback_info completed_info;         /* Function to be called after dispatch_func has completed. Ignored if NULL. */
	struct asyncio_threadpool_callback_info cancelled_info;         /* Function to be called after dispatch_func has been cancelled. Ignored if NULL. */
};

int asyncio_threadpool_init(void);
int asyncio_threadpool_dispatch(const struct asyncio_threadpool_dispatch_info *task, struct asyncio_threadpool_handle **handlep);
int asyncio_threadpool_cancel(struct asyncio_threadpool_handle *handle);
int asyncio_threadpool_join(struct asyncio_threadpool_handle *handle);
int asyncio_threadpool_acquire_handle(struct asyncio_threadpool_handle *handle);
void asyncio_threadpool_release_handle(struct asyncio_threadpool_handle *handle);
void asyncio_threadpool_cleanup(void);

#endif
