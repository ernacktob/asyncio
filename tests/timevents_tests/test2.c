#include <stdio.h>
#include <unistd.h>

#include "timevents.h"

static void periodic_callback(void *arg, timevent_handle_t self_handle)
{
	(void)arg;

	printf("Hello world!\n");

	if (timevent_continue(self_handle) != 0)
		printf("Failed to continue self.\n");
}

int main()
{
	struct timevent_info info;
	timevent_handle_t handle;

	info.timeout = 1000;	/* 1 second */
	info.flags = TIMEVENT_FLAG_CANCELLABLE;
	info.cb = periodic_callback;
	info.arg = NULL;

	if (timevent_register(&info, &handle) != 0) {
		printf("Failed to register timevent.\n");
		return -1;
	}

	printf("Sleeping for 10s...\n");
	usleep(10000000);

	printf("Cancelling...\n");

	if (timevent_cancel(handle) != 0) {
		printf("Failed to cancel timevent.\n");
		return -1;
	}

	printf("Joining...\n");
	if (timevent_join(handle) != 0) {
		printf("Failed to join timevent.\n");
		return -1;
	}

	timevent_release_handle(handle);
	return 0;
}
