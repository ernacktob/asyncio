#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <inttypes.h>

#include <unistd.h>

#include "asyncio_threadpool.h"

#define NUM_CREATORS	10000
#define NUM_TASKS	10000

/* PROTOTYPES */
void do_stuff(void *arg);
void create_threads(void *arg);
/* END PROTOTYPES */

size_t ncreators = 0;

void do_stuff(void *arg)
{
	int j;

	j = *(int *)&arg;
	usleep(j * 10000);
}

void create_threads(void *arg)
{
	struct asyncio_threadpool_handle *handles[NUM_TASKS];
	struct asyncio_threadpool_dispatch_info info;
	struct asyncio_threadpool_handle *create_handle;
	int i;

	(void)arg;

	/* Just avoid infinite loop */
	if (ncreators >= NUM_CREATORS)
		return;

	++ncreators;

	info.flags = ASYNCIO_THREADPOOL_FLAG_CONTRACTOR | ASYNCIO_THREADPOOL_FLAG_CANCELLABLE;
	info.dispatch_info.fn = do_stuff;
	info.completed_info.cb = NULL;
	info.cancelled_info.cb = NULL;

	for (i = 0; i < NUM_TASKS; i++) {
		info.dispatch_info.arg = *(void **)&i;

		if (asyncio_threadpool_dispatch(&info, &handles[i]) != 0)
			printf("Failed to dispatch task #%d\n", i);
	}

	info.dispatch_info.fn = create_threads;
	info.dispatch_info.arg = NULL;

	if (asyncio_threadpool_dispatch(&info, &create_handle) != 0)
		printf("Failed to dispatch myself\n");

	for (i = 0; i < NUM_TASKS; i++) {
		if (rand() % 2) {
			if (asyncio_threadpool_cancel(handles[i]) != 0)
				printf("Failed to cancel task #%d\n", i);
		}

		if (asyncio_threadpool_join(handles[i]) != 0)
			printf("Failed to join task #%d\n", i);
	}

	if (asyncio_threadpool_cancel(create_handle) != 0)
		printf("Failed to cancel myself (not thread self...)\n");

	if (asyncio_threadpool_join(create_handle) != 0)
		printf("Failed to join myself\n");

	asyncio_threadpool_release_handle(create_handle);

	for (i = 0; i < NUM_TASKS; i++)
		asyncio_threadpool_release_handle(handles[i]);
}

int main()
{
	struct asyncio_threadpool_dispatch_info info;
	struct asyncio_threadpool_handle *handle;

	srand(time(NULL));

	if (asyncio_threadpool_init() != 0) {
		printf("Failed to initialize asyncio_threadpool module.\n");
		return -1;
	}

	info.flags = ASYNCIO_THREADPOOL_FLAG_CANCELLABLE;
	info.dispatch_info.fn = create_threads;
	info.dispatch_info.arg = NULL;
	info.completed_info.cb = NULL;
	info.cancelled_info.cb = NULL;

	if (asyncio_threadpool_dispatch(&info, &handle) != 0) {
		printf("Failed to dispatch from main!\n");
		asyncio_threadpool_cleanup();
		return -1;
	}

	if (asyncio_threadpool_join(handle) != 0)
		printf("Failed to join from main\n");

	asyncio_threadpool_release_handle(handle);
	asyncio_threadpool_cleanup();
	return 0;
}
