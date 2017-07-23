#include <stdio.h>
#include <stdarg.h>

#include <unistd.h>

#include "asyncio_threadpool.h"

/* PROTOTYPES */
void printf_locked(const char *fmt, ...);
void do_stuff(void *arg);
void do_things(void *arg);
/* END PROTOTYPES */

void printf_locked(const char *fmt, ...)
{
	va_list ap;

	flockfile(stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	funlockfile(stderr);
}

void do_stuff(void *arg)
{
	int i;
	(void)arg;

	for (i = 0; i < 10; i++)
		printf_locked("do_stuff: %d\n", i);

	printf_locked("do_stuff: sleeping...\n");
	usleep(10000000);

	printf_locked("do_stuff: done\n");
}

void do_things(void *arg)
{
	int i;
	(void)arg;

	for (i = 10; i < 20; i++)
		printf_locked("do_things: %d\n", i);

	printf_locked("do_things: sleeping...\n");
	usleep(5000000);
	printf_locked("do_things: done\n");
}

int main()
{
	struct asyncio_threadpool_dispatch_info info;
	struct asyncio_threadpool_handle *handle, *handle2;

	if (asyncio_threadpool_init() != 0) {
		printf_locked("Failed to initialize threadpool module.\n");
		return -1;
	}

	info.flags = ASYNCIO_THREADPOOL_FLAG_CANCELLABLE;
	info.dispatch_info.fn = do_stuff;
	info.dispatch_info.arg = NULL;
	info.completed_info.cb = NULL;
	info.cancelled_info.cb = NULL;

	if (asyncio_threadpool_dispatch(&info, &handle2) != 0) {
		printf_locked("Failed to dispatch.\n");
		asyncio_threadpool_cleanup();
		return -1;
	}

	info.dispatch_info.fn = do_things;

	if (asyncio_threadpool_dispatch(&info, &handle) != 0) {
		printf_locked("Failed to dispatch.\n");
		asyncio_threadpool_release_handle(handle2);
		asyncio_threadpool_cleanup();
		return -1;
	}

	printf_locked("joining\n");
	if (asyncio_threadpool_join(handle) != 0)
		printf_locked("Failed to join.\n");

	printf_locked("joined handle\n");
	if (asyncio_threadpool_cancel(handle2) != 0)
		printf_locked("Failed to cancel.\n");

	if (asyncio_threadpool_join(handle2) != 0)
		printf_locked("Failed to join2\n");

	printf_locked("joined handle2\n");
	asyncio_threadpool_release_handle(handle2);
	asyncio_threadpool_release_handle(handle);
	asyncio_threadpool_cleanup();
	return 0;
}
