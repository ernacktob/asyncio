#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <stdarg.h>
#include <math.h>

#include <sys/time.h>

#include "timevents.h"

#define NDEADLINES	10000
#define MAX_TIMEOUT	10000	/* 10 seconds */

#define ABS(x)	((x) > 0 ? (x) : (-(x)))

struct timer_arg {
	int timeout;
	uint64_t start;
	int i;
};

static struct timer_arg args[NDEADLINES];
static timevent_handle_t handles[NDEADLINES];
static unsigned int deltas[NDEADLINES];

static unsigned int counter = 0;
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

static void printf_locked(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	flockfile(stderr);
	vfprintf(stderr, fmt, args);
	fflush(stderr);
	funlockfile(stderr);
	va_end(args);
}

static int get_timestamp(uint64_t *timestamp)
{
	struct timeval tv;
	uint64_t now;

	if (gettimeofday(&tv, NULL) != 0) {
		perror("gettimeofday");
		return -1;
	}

	now = (uint64_t)(tv.tv_sec) * 1000 + (uint64_t)(tv.tv_usec) / 1000;	/* Convert to milliseconds */
	*timestamp = now;

	return 0;
}

static void increment_counter(void *arg, int *continued)
{
	struct timer_arg *timer;
	uint64_t now;
	(void)continued;

	if (get_timestamp(&now) != 0)
		return;

	if (pthread_mutex_lock(&mtx) != 0) {
		perror("pthread_mutex_lock");
		return;
	}

	timer = (struct timer_arg *)arg;
	deltas[timer->i] = (unsigned int)(ABS((int)(now - timer->start - timer->timeout)));
	++counter;

	if (pthread_mutex_unlock(&mtx) != 0)
		perror("pthread_mutex_unlock");
}

int main()
{
	struct timevent_info info;
	unsigned max;
	double avg, std;
	int i;

	srand(time(NULL));

	info.flags = TIMEVENT_FLAG_NONE;
	info.cb = increment_counter;

	for (i = 0; i < NDEADLINES; i++) {
		args[i].i = i;
		args[i].timeout = rand() % MAX_TIMEOUT;

		if (get_timestamp(&args[i].start) != 0) {
			printf_locked("Failed to get timestamp.\n");
			return -1;
		}

		info.timeout = args[i].timeout;
		info.arg = &args[i];

		if (timevent_register(&info, &handles[i]) != 0) {
			printf_locked("Failed to register timevent.\n");
			return -1;
		}
	}

	for (i = 0; i < NDEADLINES; i++) {
		if (timevent_join(handles[i]) != 0) {
			printf_locked("Failed to join timevent.\n");
			return -1;
		}

		timevent_release_handle(handles[i]);
	}

	if (counter != NDEADLINES) {
		printf_locked("Incorrect counter\n");
		return -1;
	}

	max = 0;
	avg = 0.0;
	std = 0.0;

	for (i = 0; i < NDEADLINES; i++) {
		if (deltas[i] > max)
			max = deltas[i];

		avg = avg * ((double)i / ((double)i + 1.0)) + (double)deltas[i] / ((double)i + 1.0);
	}

	for (i = 0; i < NDEADLINES; i++)
		std = std * ((double)i / ((double)i + 1.0)) + (((double)deltas[i] - avg) * ((double)deltas[i] - avg)) / ((double)i + 1.0);

	std = sqrt(std);

	printf_locked("Avg/Max/Std timevent absolute error delay: %ums/%ums/%ums\n", (unsigned int)avg, max, (unsigned int)std);
	printf_locked("Test passed!\n");
	return 0;
}
