#ifndef STREAMS_PRIV_H
#define STREAMS_PRIV_H

#include <stddef.h>

#include "asyncio_streams.h"
#include "asyncio_fdevents.h"
#include "refcounts.h"

struct asyncio_streams_handle_priv {
	struct asyncio_streams_handle pub;
	struct asyncio_fdevents_handle *evhandle;
	struct refcount_base refcount;

	size_t len;
	size_t pos;

	union {
		struct {
			uint8_t *buf;
			void (*done_cb)(size_t len, void *state);
		} read_info;

		struct {
			const uint8_t *buf;
			void (*done_cb)(void *arg);
		} write_info;
	} info;

	struct asyncio_streams_policy policy;
};

#endif
