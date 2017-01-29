#include <stdio.h>
#include <stdarg.h>

#include <unistd.h>

#include "threadpool.h"

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
	struct threadpool_dispatch_info info;
	threadpool_handle_t handle, handle2;

	info.flags = THREADPOOL_FLAG_CANCELLABLE;
	info.dispatch_info.fn = do_stuff;
	info.dispatch_info.arg = NULL;
	info.completed_info.cb = NULL;
	info.cancelled_info.cb = NULL;

	if (threadpool_dispatch(&info, &handle2) != 0) {
		printf_locked("Failed to dispatch.\n");
		return -1;
	}

	info.dispatch_info.fn = do_things;

	if (threadpool_dispatch(&info, &handle) != 0) {
		printf_locked("Failed to dispatch.\n");
		return -1;
	}

	if (threadpool_join(handle) != 0)
		printf_locked("Failed to join.\n");

	if (threadpool_cancel(handle2) != 0)
		printf_locked("Failed to cancel.\n");

	if (threadpool_join(handle2) != 0)
		printf_locked("Failed to join2\n");

	threadpool_release_handle(handle2);
	threadpool_release_handle(handle);
	return 0;
}
