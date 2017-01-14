#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <inttypes.h>

#include <unistd.h>

#include "threadpool.h"

#define NUM_TASKS	100000
#define MAX_COUNT	100000

static size_t counters[NUM_TASKS] = {0};
static size_t sum = 0;
static threadpool_handle_t handles[NUM_TASKS];
static int args[NUM_TASKS];

void do_stuff(void *arg)
{
	int i;
	int j;

	j = *(int *)arg;

	for (i = 0; i < MAX_COUNT; i++)
		++counters[j];
}

int main()
{
	struct threadpool_dispatch_info info;

	int i;

	srand(time(NULL));

	info.flags = THREADPOOL_FLAG_CANCELLABLE;
	info.dispatch_info.fn = do_stuff;
	info.completed_info.cb = NULL;
	info.cancelled_info.cb = NULL;

	for (i = 0; i < NUM_TASKS; i++) {
		args[i] = i;
		info.dispatch_info.arg = &args[i];

		if (threadpool_dispatch(&info, &handles[i]) != 0)
			printf("Failed to dispatch task #%d\n", i);
	}

	for (i = 0; i < NUM_TASKS; i++) {
		if (rand() % 2) {
			if (threadpool_cancel(handles[i]) != 0)
				printf("Failed to cancel task #%d\n", i);
		}

		if (threadpool_join(handles[i]) != 0)
			printf("Failed to join task #%d\n", i);
	}

	for (i = 0; i < NUM_TASKS; i++)
		threadpool_release_handle(handles[i]);

	for (i = 0; i < NUM_TASKS; i++)
		sum += counters[i];

	printf("sum = %lu\n", sum);
	return 0;
}
