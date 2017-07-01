#ifndef ASYNCIO_FDEVENTS_H
#define ASYNCIO_FDEVENTS_H

#include <stddef.h>
#include <stdint.h>

#define ASYNCIO_FDEVENTS_EVENT_READ		(1 << 0)
#define ASYNCIO_FDEVENTS_EVENT_WRITE		(1 << 1)
#define ASYNCIO_FDEVENTS_EVENT_ERROR		(1 << 2)

struct asyncio_fdevents_handle_priv;
struct asyncio_fdevents_loop_priv;

typedef void (*asyncio_fdevents_callback)(int fd, uint16_t revents, void *arg, int *continued);

struct asyncio_fdevents_info {
	int fd;
	uint16_t events;
	uint32_t flags;
	asyncio_fdevents_callback cb;	/* Called when event occurs */
	void *arg;
};

struct asyncio_fdevents_options {
	size_t max_nfds;
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
	int (*listen)(struct asyncio_fdevents_loop *self, const struct asyncio_fdevents_info *info, struct asyncio_fdevents_handle **handlep);
	void (*release)(const struct asyncio_fdevents_loop *self);
};

int asyncio_fdevents_init(void);
int asyncio_fdevents_eventloop(const struct asyncio_fdevents_options *options, struct asyncio_fdevents_loop **eventloop);
void asyncio_fdevents_cleanup(void);

#endif
