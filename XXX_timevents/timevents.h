#ifndef TIMEVENTS_H
#define TIMEVENTS_H

#include <stdint.h>

typedef void *timevent_handle_t;
typedef void (*timevent_callback)(void *arg, int *continued);

struct timevent_info {
	int timeout;
	uint32_t flags;
	timevent_callback cb;
	void *arg;
};

#define timevent_continue(continued)	(*(continued) = 1)

int timevent_register(struct timevent_info *timinfo, timevent_handle_t *handle);
int timevent_join(timevent_handle_t handle);
int timevent_cancel(timevent_handle_t handle);
int timevent_acquire_handle(timevent_handle_t handle);
void timevent_release_handle(timevent_handle_t handle);

#endif
