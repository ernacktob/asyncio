#ifndef FDEVENTS_PRIV_H
#define FDEVENTS_PRIV_H

#include <stdint.h>

#include "asyncio_fdevents.h"
#include "events.h"
#include "hashtable.h"
#include "queue.h"

#define FDEVENTS_HANDLE_QUEUE_ID	0
#define NUMBER_FDEVENTS_HANDLE_QUEUES	1

struct asyncio_fdevents_handle_priv {
	struct asyncio_fdevents_handle pub;
	struct events_handle base;

	int fd;
	void *evinfo;
	void *revinfo;
	asyncio_fdevents_callback callback_fn;	/* Called when event occurs */
	void *callback_arg;

	struct asyncio_fdevents_handle_priv *prev[NUMBER_FDEVENTS_HANDLE_QUEUES];
	struct asyncio_fdevents_handle_priv *next[NUMBER_FDEVENTS_HANDLE_QUEUES];
};

struct asyncio_fdevents_loop_priv {
	struct asyncio_fdevents_loop pub;
	struct events_loop base;

	decl_queue(struct asyncio_fdevents_handle_priv, *callbacks);
	struct hashtable fd_map;
	size_t last;

	int wakefd;
	int clearwakefd;

	struct fdevents_backend_ops *backend_ops;
	void *backend_data;
};

struct fdevents_backend_ops {
	int (*init_evinfo_revinfo)(const void *evinfo, void **evinfop, void **revinfop);
	void (*cleanup_evinfo_revinfo)(void *evinfo, void *revinfo);
	int (*acquire_evinfo_locked)(void *backend_data, int fd, const void *evinfo);
	void (*release_evinfo_locked)(void *backend_data, int fd, const void *evinfo);
	int (*match_evinfo_locked)(const void *backend_data, int fd, const void *evinfo, void *revinfo);

	int (*insert_fd_locked)(void *backend_data, int fd, const void *evinfo);
	void (*remove_fd_locked)(void *backend_data, int fd);

	int (*initialize_eventloop_thread)(void *backend_data);
	int (*wait_for_events)(void *backend_data);
	void (*process_changed_events_locked)(void *backend_data);
	void (*scan_for_events_init_locked)(void *backend_data);
	int (*scan_for_events_next_locked)(void *backend_data, int *fd);
	void (*continue_eventloop_thread_locked)(void *backend_data);
	void (*cleanup_eventloop_thread)(void *backend_data);

	int (*init_backend_data)(void **backend_data, size_t max_nfds, int clearwakefd);
	void (*cleanup_backend_data)(void *backend_data);
};

#endif
