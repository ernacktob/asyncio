#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <inttypes.h>

#include <unistd.h>

#include "asyncio_threadpool.h"

#define NUM_TASKS	100000
#define MAX_COUNT	1000000

/* PROTOTYPES */
void do_stuff(void *arg);
/* END PROTOTYPES */

static size_t counters[NUM_TASKS] = {0};
static size_t sum = 0;
static struct asyncio_threadpool_handle *handles[NUM_TASKS];
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
	struct asyncio_threadpool_dispatch_info info;

	int i;

	srand(time(NULL));

	if (asyncio_threadpool_init() != 0) {
		printf("Failed to initialize threadpool module.\n");
		return -1;
	}

	info.flags = ASYNCIO_THREADPOOL_FLAG_NONE;
	info.dispatch_info.fn = do_stuff;
	info.completed_info.cb = NULL;
	info.cancelled_info.cb = NULL;

	for (i = 0; i < NUM_TASKS; i++) {
		args[i] = i;
		info.dispatch_info.arg = &args[i];

		if (asyncio_threadpool_dispatch(&info, &handles[i]) != 0)
			printf("Failed to dispatch task #%d\n", i);
	}

	for (i = 0; i < NUM_TASKS; i++) {
		if (asyncio_threadpool_join(handles[i]) != 0)
			printf("Failed to join task #%d\n", i);
	}

	for (i = 0; i < NUM_TASKS; i++)
		asyncio_threadpool_release_handle(handles[i]);

	for (i = 0; i < NUM_TASKS; i++)
		sum += counters[i];

	printf("sum = %lu\n", sum);
	asyncio_threadpool_cleanup();
	return 0;
}
