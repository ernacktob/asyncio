#ifndef BUFFEVENTS_H
#define BUFFEVENTS_H

#include <stdint.h>
#include <stddef.h>

#include "asyncio_fdevents.h"

struct asyncio_buffevents_handle_priv;

struct asyncio_buffevents_handle {
	struct asyncio_buffevents_handle_priv *priv;

	int (*wait)(const struct asyncio_buffevents_handle *self);
	int (*cancel)(const struct asyncio_buffevents_handle *self);
	int (*acquire)(const struct asyncio_buffevents_handle *self);
	void (*release)(const struct asyncio_buffevents_handle *self);
};

struct asyncio_buffevents_read_info {
	const struct asyncio_fdevents_loop *eventloop;
	const void *evinfo;
	int fd;

	uint8_t *buf;
	size_t len;
	void *arg;

	uint32_t threadpool_flags;

	int (*is_errored)(const void *revinfo);

	int (*read_cb)(int fd, uint8_t *buf, size_t len, size_t *rdlen, void *arg);
	int (*until_cb)(const uint8_t *buf, size_t len, void *arg);
	void (*error_cb)(void *arg);
	void (*cancelled_cb)(void *arg);
	void (*done_cb)(uint8_t *buf, size_t len, void *arg);
};

struct asyncio_buffevents_write_info {
	const struct asyncio_fdevents_loop *eventloop;
	const void *evinfo;
	int fd;

	const uint8_t *buf;
	size_t len;
	void *arg;

	uint8_t *buf_to_free; /* We can't free a const pointer, so pass also as a non-const for done_cb */

	uint32_t threadpool_flags;

	int (*is_errored)(const void *revinfo);

	int (*write_cb)(int fd, const uint8_t *buf, size_t len, size_t *wrlen, void *arg);
	void (*error_cb)(void *arg);
	void (*cancelled_cb)(void *arg);
	void (*done_cb)(uint8_t *buf, size_t len, void *arg);
};

int asyncio_buffevents_read(const struct asyncio_buffevents_read_info *info, struct asyncio_buffevents_handle **handlep);
int asyncio_buffevents_write(const struct asyncio_buffevents_write_info *info, struct asyncio_buffevents_handle **handlep);

#endif
