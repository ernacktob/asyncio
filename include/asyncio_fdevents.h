#ifndef ASYNCIO_FDEVENTS_H
#define ASYNCIO_FDEVENTS_H

#include <stddef.h>
#include <stdint.h>

#include "asyncio_fdevents_poll.h"
#include "asyncio_fdevents_select.h"
#include "asyncio_threadpool.h"

#define ASYNCIO_FDEVENTS_BACKEND_POLL		0
#define ASYNCIO_FDEVENTS_BACKEND_SELECT		1
#define ASYNCIO_FDEVENTS_MAX_BACKEND_TYPES	2

struct asyncio_fdevents_handle_priv;
struct asyncio_fdevents_loop_priv;

struct asyncio_fdevents_loop;
struct asyncio_fdevents_callback_info;

typedef void (*asyncio_fdevents_callback)(const struct asyncio_fdevents_callback_info *info, int *continued);
typedef void (*asyncio_fdevents_cancelled_callback)(void *arg);
typedef void (*asyncio_fdevents_release_callback)(void *arg);

struct asyncio_fdevents_callback_info {
	const struct asyncio_fdevents_loop *eventloop;
	int fd;
	const void *revinfo;
	void *arg;
};

struct asyncio_fdevents_listen_info {
	int fd;
	const void *evinfo;
	asyncio_fdevents_callback cb;
	asyncio_fdevents_cancelled_callback cancelled_cb;
	asyncio_fdevents_release_callback release_cb;
	void *arg;
	uint32_t threadpool_flags;
};

#define ASYNCIO_FDEVENTS_LISTEN_INFO_DEFAULT_INIT(info, user_fd, user_evinfo, user_cb) \
	do {\
		info.fd = user_fd;\
		info.evinfo = user_evinfo;\
		info.cb = user_cb;\
		info.cancelled_cb = NULL;\
		info.release_cb = NULL;\
		info.arg = NULL;\
		info.threadpool_flags = ASYNCIO_THREADPOOL_FLAG_NONE;\
	} while (0)

struct asyncio_fdevents_options {
	size_t max_nfds;
	unsigned int backend_type;
};

struct asyncio_fdevents_handle {
	struct asyncio_fdevents_handle_priv *priv;

	int (*wait)(const struct asyncio_fdevents_handle *self);
	int (*cancel)(const struct asyncio_fdevents_handle *self);
	int (*acquire)(const struct asyncio_fdevents_handle *self);
	void (*release)(const struct asyncio_fdevents_handle *self);
};

struct asyncio_fdevents_loop {
	struct asyncio_fdevents_loop_priv *priv;

	int (*acquire)(const struct asyncio_fdevents_loop *self);
	int (*listen)(const struct asyncio_fdevents_loop *self, const struct asyncio_fdevents_listen_info *info, struct asyncio_fdevents_handle **handlep);
	void (*release)(const struct asyncio_fdevents_loop *self);
};

/* Utility functions */
int asyncio_fdevents_set_nonblocking(int fd);
int asyncio_fdevents_set_blocking(int fd);

int asyncio_fdevents_init(void);
int asyncio_fdevents_eventloop(const struct asyncio_fdevents_options *options, struct asyncio_fdevents_loop **eventloop);
void asyncio_fdevents_cleanup(void);

#endif
