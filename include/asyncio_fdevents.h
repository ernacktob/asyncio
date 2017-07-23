#ifndef ASYNCIO_FDEVENTS_H
#define ASYNCIO_FDEVENTS_H

#include <stddef.h>
#include <stdint.h>

#include "asyncio_fdevents_poll.h"
#include "asyncio_fdevents_select.h"

#define ASYNCIO_FDEVENTS_BACKEND_POLL		0
#define ASYNCIO_FDEVENTS_BACKEND_SELECT		1
#define ASYNCIO_FDEVENTS_MAX_BACKEND_TYPES	2

struct asyncio_fdevents_handle_priv;
struct asyncio_fdevents_loop_priv;

struct asyncio_fdevents_loop;

typedef void (*asyncio_fdevents_callback)(const struct asyncio_fdevents_loop *eventloop, int fd, const void *revinfo, void *arg, int *continued);

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
	int (*listen)(const struct asyncio_fdevents_loop *self, int fd, const void *evinfo, asyncio_fdevents_callback cb, void *arg, uint32_t threadpool_flags, struct asyncio_fdevents_handle **handlep);
	void (*release)(const struct asyncio_fdevents_loop *self);
};

int asyncio_fdevents_init(void);
int asyncio_fdevents_eventloop(const struct asyncio_fdevents_options *options, struct asyncio_fdevents_loop **eventloop);
void asyncio_fdevents_cleanup(void);

#endif
