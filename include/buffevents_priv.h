#ifndef BUFFEVENTS_PRIV_H
#define BUFFEVENTS_PRIV_H

#include <stddef.h>

#include "asyncio_buffevents.h"
#include "asyncio_fdevents.h"
#include "refcounts.h"

struct asyncio_buffevents_handle_priv {
	struct asyncio_buffevents_handle pub;
	struct asyncio_fdevents_handle *evhandle;
	struct refcount_base refcount;

	size_t pos;

	union {
		struct asyncio_buffevents_read_info read_info;
		struct asyncio_buffevents_write_info write_info;
	} info;
};

#endif
