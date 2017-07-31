#ifndef ASYNCIO_STREAMS_H
#define ASYNCIO_STREAMS_H

#include <stdint.h>
#include <stddef.h>

#include "asyncio_fdevents.h"

struct asyncio_streams_handle_priv;

struct asyncio_streams_handle {
	struct asyncio_streams_handle_priv *priv;

	int (*wait)(const struct asyncio_streams_handle *self);
	int (*cancel)(const struct asyncio_streams_handle *self);
	int (*acquire)(const struct asyncio_streams_handle *self);
	void (*release)(const struct asyncio_streams_handle *self);
};

struct asyncio_streams_policy {
	const struct asyncio_fdevents_loop *eventloop;
	const void *read_evinfo;
	const void *write_evinfo;

	void *state;

	int fd;
	uint32_t threadpool_flags;

	int (*is_errored)(const void *revinfo, void *state);

	int (*read_cb)(uint8_t *buf, size_t len, size_t *rdlen, void *state);
	int (*write_cb)(const uint8_t *buf, size_t len, size_t *wrlen, void *state);

	int (*until_cb)(size_t len, uint8_t **unread_buf, size_t *unread_len, void *state);

	void (*eof_cb)(void *state);
	void (*error_cb)(void *state);
	void (*cancelled_cb)(void *state);
};

int asyncio_streams_read(uint8_t *buf, size_t len, void (*done_cb)(size_t len, void *state), const struct asyncio_streams_policy *policy, struct asyncio_streams_handle **handlep);
int asyncio_streams_write(const uint8_t *buf, size_t len, void (*done_cb)(void *state), const struct asyncio_streams_policy *policy, struct asyncio_streams_handle **handlep);

#endif
