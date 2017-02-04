#ifndef TIMEVENTS_H
#define TIMEVENTS_H

#include <stdint.h>

#define TIMEVENT_FLAG_NONE		0
#define TIMEVENT_FLAG_CONTRACTOR	(1 << 0)
#define TIMEVENT_FLAG_CANCELLABLE	(1 << 1)
#define TIMEVENT_FLAG_ASYNCCANCEL	(1 << 2)

typedef void *timevent_handle_t;
typedef void (*timevent_callback)(void *arg, timevent_handle_t self_handle);

struct timevent_info {
	int timeout;
	uint32_t flags;
	timevent_callback cb;
	void *arg;
};

int timevent_register(struct timevent_info *timinfo, timevent_handle_t *handle);
int timevent_continue(timevent_handle_t handle);
int timevent_join(timevent_handle_t handle);
int timevent_cancel(timevent_handle_t handle);
int timevent_acquire_handle(timevent_handle_t handle);
void timevent_release_handle(timevent_handle_t handle);

#endif
