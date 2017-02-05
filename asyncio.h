#ifndef ASYNCIO_H
#define ASYNCIO_H

#include <stdint.h>

#define ASYNCIO_FLAG_NONE		0
#define ASYNCIO_FLAG_CONTRACTOR		(1 << 0)
#define ASYNCIO_FLAG_CANCELLABLE	(1 << 1)
#define ASYNCIO_FLAG_ASYNCCANCEL	(1 << 2)

#define ASYNCIO_FDEVENT_READ		(1 << 0)
#define ASYNCIO_FDEVENT_WRITE		(1 << 1)
#define ASYNCIO_FDEVENT_ERROR		(1 << 2)

typedef uint16_t asyncio_fdevent_t;
typedef uint32_t asyncio_flag_t;
typedef int asyncio_time_t;
typedef int *asyncio_continue_t;

typedef void *asyncio_handle_t;
typedef void (*asyncio_dispatch_fn)(void *arg);
typedef void (*asyncio_fdevent_cb)(int fd, asyncio_fdevent_t revents, void *arg, asyncio_continue_t continued);
typedef void (*asyncio_timevent_cb)(void *arg, asyncio_continue_t continued);

#define asyncio_continue(continued)	(*(continued) = 1)

int asyncio_dispatch(asyncio_dispatch_fn fn, void *arg, asyncio_flag_t flags, asyncio_handle_t *handle);
int asyncio_fdevent(int fd, asyncio_fdevent_t events, asyncio_fdevent_cb cb, void *arg, asyncio_flag_t flags, asyncio_handle_t *handle);
int asyncio_timevent(asyncio_time_t timeout, asyncio_timevent_cb cb, void *arg, asyncio_flag_t flags, asyncio_handle_t *handle);
int asyncio_join(asyncio_handle_t handle);
int asyncio_cancel(asyncio_handle_t handle);
int asyncio_acquire(asyncio_handle_t handle);
void asyncio_release(asyncio_handle_t handle);

#endif
