#include <time.h>
#include <sys/time.h>

/* See http://stackoverflow.com/questions/11680461/monotonic-clock-on-osx */
#ifdef __MACH__
#include <mach/clock.h>
#include <mach/mach.h>
#endif

#include "logging.h"
#include "monotonic_gettime.h"

int asyncio_monotonic_gettime(struct timespec *ts)
{
#ifdef __MACH__	/* OS X does not have clock_gettime, use clock_get_time */
	clock_serv_t cclock;
	mach_timespec_t mts;

	if (host_get_clock_service(mach_host_self(), SYSTEM_CLOCK, &cclock) != KERN_SUCCESS) {
		ASYNCIO_SYSERROR("host_get_clock_service");
		return -1;
	}

	if (clock_get_time(cclock, &mts) != KERN_SUCCESS) {
		ASYNCIO_SYSERROR("clock_get_time");
		return -1;
	}

	if (mach_port_deallocate(mach_task_self(), cclock) != KERN_SUCCESS) {
		ASYNCIO_SYSERROR("mach_port_deallocate");
		return -1;
	}

	ts->tv_sec = mts.tv_sec;
	ts->tv_nsec = mts.tv_nsec;
	return 0;
#else
	if (clock_gettime(CLOCK_MONOTONIC, ts) != 0) {
		ASYNCIO_SYSERROR("clock_gettime");
		return -1;
	}

	return 0;
#endif
}
