#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <stdint.h>

#define THREADPOOL_FLAG_NONE		0
#define THREADPOOL_FLAG_CONTRACTOR	(1 << 0)
#define THREADPOOL_FLAG_CANCELLABLE	(1 << 1)
#define THREADPOOL_FLAG_ASYNCCANCEL	(1 << 2)

typedef void *threadpool_handle_t;

typedef void (*threadpool_function)(void *arg);
typedef void (*threadpool_callback)(void *arg);

struct threadpool_function_info {
	threadpool_function fn;
	void *arg;
};

struct threadpool_callback_info {
	threadpool_callback cb;
	void *arg;
};

struct threadpool_dispatch_info {
	uint32_t flags;
	struct threadpool_function_info dispatch_info;          /* Function to be executed asynchronously by the thread pool. Cannot be NULL. */
	struct threadpool_callback_info completed_info;         /* Function to be called after dispatch_func has completed. Ignored if NULL. */
	struct threadpool_callback_info cancelled_info;         /* Function to be called after dispatch_func has been cancelled. Ignored if NULL. */
};

int threadpool_init(void);
int threadpool_dispatch(struct threadpool_dispatch_info *task, threadpool_handle_t *handlep);
int threadpool_cancel(threadpool_handle_t handle);
int threadpool_join(threadpool_handle_t handle);
int threadpool_acquire_handle(threadpool_handle_t handle);
void threadpool_release_handle(threadpool_handle_t handle);
void threadpool_cleanup(void);

#endif
