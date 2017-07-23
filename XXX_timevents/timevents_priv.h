#ifndef TIMEVENTS_PRIV_H
#define TIMEVENTS_PRIV_H

#include <stdint.h>

#include "asyncio_timevents.h"
#include "events.h"
#include "priority_queue.h"
#include "queue.h"

#define TIMEVENTS_HANDLE_QUEUE_ID	0
#define NUMBER_TIMEVENTS_HANDLE_QUEUES	1

struct asyncio_timevents_handle_priv {
	struct asyncio_timevents_handle pub;
	struct events_handle base;

	/* XXX */
	int timeout;
	uint64_t deadline;
	void *evinfo;
	void *revinfo;
	asyncio_timevents_callback callback_fn;	/* Called when event occurs */
	void *callback_arg;

	struct asyncio_timevents_handle_priv *prev[NUMBER_TIMEVENTS_HANDLE_QUEUES];
	struct asyncio_timevents_handle_priv *next[NUMBER_TIMEVENTS_HANDLE_QUEUES];
};

struct asyncio_timevents_loop_priv {
	struct asyncio_timevents_loop pub;
	struct events_loop base;

	decl_queue(struct asyncio_timevents_handle_priv, *callbacks);
	struct hashtable tim_map;
	size_t last;

	int waketim;
	int clearwaketim;

	struct timevents_backend_ops *backend_ops;
	void *backend_data;
};

struct timevents_backend_ops {
	int (*init_evinfo_revinfo)(const void *evinfo, void **evinfop, void **revinfop);
	void (*cleanup_evinfo_revinfo)(void *evinfo, void *revinfo);
	int (*acquire_evinfo_locked)(void *backend_data, int tim, const void *evinfo);
	void (*release_evinfo_locked)(void *backend_data, int tim, const void *evinfo);
	int (*match_evinfo_locked)(const void *backend_data, int tim, const void *evinfo, void *revinfo);

	int (*insert_tim_locked)(void *backend_data, int tim, const void *evinfo);
	void (*remove_tim_locked)(void *backend_data, int tim);

	int (*initialize_eventloop_thread)(void *backend_data);
	int (*wait_for_events)(void *backend_data);
	void (*process_changed_events_locked)(void *backend_data);
	void (*scan_for_events_init_locked)(void *backend_data);
	int (*scan_for_events_next_locked)(void *backend_data, int *tim);
	void (*continue_eventloop_thread_locked)(void *backend_data);
	void (*cleanup_eventloop_thread)(void *backend_data);

	int (*init_backend_data)(void **backend_data, size_t max_ntims, int clearwaketim);
	void (*cleanup_backend_data)(void *backend_data);
};

#endif
