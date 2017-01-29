#ifndef FDEVENTS_H
#define FDEVENTS_H

#include <stdint.h>

#define FDEVENT_EVENT_POLLERR		(1 << 0)
#define FDEVENT_EVENT_POLLHUP		(1 << 1)
#define FDEVENT_EVENT_POLLIN		(1 << 2)
#define FDEVENT_EVENT_POLLNVAL		(1 << 3)
#define FDEVENT_EVENT_POLLOUT		(1 << 4)
#define FDEVENT_EVENT_POLLPRI		(1 << 5)
#define FDEVENT_EVENT_POLLRDBAND	(1 << 6)
#define FDEVENT_EVENT_POLLRDNORM	(1 << 7)
#define FDEVENT_EVENT_POLLWRBAND	(1 << 8)
#define FDEVENT_EVENT_POLLWRNORM	(1 << 9)

#define FDEVENT_FLAG_NONE		0
#define FDEVENT_FLAG_ONESHOT		(1 << 0)
#define FDEVENT_FLAG_CONTRACTOR		(1 << 1)
#define FDEVENT_FLAG_CANCELLABLE	(1 << 2)
#define FDEVENT_FLAG_ASYNCCANCEL	(1 << 3)

typedef void *fdevent_handle_t;
typedef void (*fdevent_callback)(int fd, short revents, void *arg);

struct fdevent_info {
	int fd;
	uint16_t events;
	uint32_t flags;
	fdevent_callback cb;	/* Called when event occurs */
	void *arg;
};

int fdevent_register(struct fdevent_info *evinfo, fdevent_handle_t *handle);
int fdevent_join(fdevent_handle_t handle);
int fdevent_cancel(fdevent_handle_t handle);
int fdevent_acquire_handle(fdevent_handle_t handle);
void fdevent_release_handle(fdevent_handle_t handle);

#endif
