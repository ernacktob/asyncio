#ifndef ASYNCIO_TIMEVENTS_H
#define ASYNCIO_TIMEVENTS_H

#include <stddef.h>
#include <stdint.h>

struct asyncio_timevents_handle_priv;
struct asyncio_timevents_loop_priv;

typedef void (*asyncio_timevents_callback)(void *arg, int *continued);

struct asyncio_timevents_options {
	size_t max_ndeadlines;
};

struct asyncio_timevents_handle {
	struct asyncio_timevents_handle_priv *priv;

	int (*wait)(const struct asyncio_timevents_handle *self);
	int (*cancel)(const struct asyncio_timevents_handle *self);
	int (*acquire)(const struct asyncio_timevents_handle *self);
	void (*release)(const struct asyncio_timevents_handle *self);
};

struct asyncio_timevents_loop {
	struct asyncio_timevents_loop_priv *priv;

	int (*acquire)(const struct asyncio_timevents_loop *self);
	int (*listen)(struct asyncio_timevents_loop *self, int timeout, asyncio_timevents_callback cb, void *arg, uint32_t threadpool_flags, struct asyncio_timevents_handle **handlep);
	void (*release)(const struct asyncio_timevents_loop *self);
};

int asyncio_timevents_init(void);
int asyncio_timevents_eventloop(const struct asyncio_timevents_options *options, struct asyncio_timevents_loop **eventloop);
void asyncio_timevents_cleanup(void);

#endif
