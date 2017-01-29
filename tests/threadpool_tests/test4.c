#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <inttypes.h>

#include <unistd.h>

#include "threadpool.h"

#define NUM_CREATORS	10000
#define NUM_TASKS	10000

size_t ncreators = 0;

void do_stuff(void *arg)
{
	int j;

	j = *(int *)&arg;
	usleep(j * 10000);
}

void create_threads(void *arg)
{
	threadpool_handle_t handles[NUM_TASKS];
	struct threadpool_dispatch_info info;
	threadpool_handle_t create_handle;
	int i;

	(void)arg;

	/* Just avoid infinite loop */
	if (ncreators >= NUM_CREATORS)
		return;

	++ncreators;

	info.flags = THREADPOOL_FLAG_CONTRACTOR | THREADPOOL_FLAG_CANCELLABLE;
	info.dispatch_info.fn = do_stuff;
	info.completed_info.cb = NULL;
	info.cancelled_info.cb = NULL;

	for (i = 0; i < NUM_TASKS; i++) {
		info.dispatch_info.arg = *(void **)&i;

		if (threadpool_dispatch(&info, &handles[i]) != 0)
			printf("Failed to dispatch task #%d\n", i);
	}

	info.dispatch_info.fn = create_threads;
	info.dispatch_info.arg = NULL;

	if (threadpool_dispatch(&info, &create_handle) != 0)
		printf("Failed to dispatch myself\n");

	for (i = 0; i < NUM_TASKS; i++) {
		if (rand() % 2) {
			if (threadpool_cancel(handles[i]) != 0)
				printf("Failed to cancel task #%d\n", i);
		}

		if (threadpool_join(handles[i]) != 0)
			printf("Failed to join task #%d\n", i);
	}

	if (threadpool_cancel(create_handle) != 0)
		printf("Failed to cancel myself (not thread self...)\n");

	if (threadpool_join(create_handle) != 0)
		printf("Failed to join myself\n");

	threadpool_release_handle(create_handle);

	for (i = 0; i < NUM_TASKS; i++)
		threadpool_release_handle(handles[i]);
}

int main()
{
	struct threadpool_dispatch_info info;
	threadpool_handle_t handle;

	srand(time(NULL));

	info.flags = THREADPOOL_FLAG_CANCELLABLE;
	info.dispatch_info.fn = create_threads;
	info.dispatch_info.arg = NULL;
	info.completed_info.cb = NULL;
	info.cancelled_info.cb = NULL;

	if (threadpool_dispatch(&info, &handle) != 0) {
		printf("Failed to dispatch from main!\n");
		return -1;
	}

	if (threadpool_join(handle) != 0)
		printf("Failed to join from main\n");

	threadpool_release_handle(handle);
	return 0;
}
