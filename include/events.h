#ifndef EVENTS_H
#define EVENTS_H

#include <stdint.h>

#include "threadpool.h"
#include "queue.h"
#include "threading.h"

#define EVENTS_BACKEND_QUEUE_ID			0
#define NUMBER_EVENTS_BACKEND_QUEUES		1

#define EVENTS_EVENTLOOP_QUEUE_ID		0
#define NUMBER_EVENTS_EVENTLOOP_QUEUES		1

#define EVENTS_HANDLE_QUEUE_ID			0
#define NUMBER_EVENTS_HANDLE_QUEUES		1

struct events_handle {
	struct events_loop *eventloop;

	int (*wait)(const struct events_handle *self, int old_cancelstate);
	int (*cancel)(const struct events_handle *self);
	int (*acquire)(const struct events_handle *self);
	int (*release)(const struct events_handle *self);

	uint64_t refcount;

	struct events_handle *prev[NUMBER_EVENTS_HANDLE_QUEUES];
	struct events_handle *next[NUMBER_EVENTS_HANDLE_QUEUES];

	uint32_t flags;

	int in_eventloop_database;
	int has_threadpool_handle;
	threadpool_handle *th_handle;

	int continued;
	int finished;
	ASYNCIO_COND_T finished_cond;

	void *instance;
	void (*backend_cleanup_events_handle)(void *instance);
};

struct events_loop {
	struct events_backend *backend;

	ASYNCIO_MUTEX_T mtx;
	uint64_t refcount;
	int stopped;
	int changed;

	decl_queue(struct events_handle, all_handles);

	struct events_loop *prev[NUMBER_EVENTS_EVENTLOOP_QUEUES];
	struct events_loop *next[NUMBER_EVENTS_EVENTLOOP_QUEUES];

	int (*acquire)(struct events_loop *self);
	int (*release)(struct events_loop *self);

	void *instance;

	void (*backend_callback)(void *instance, uint32_t flags, int *continued);

	int (*backend_insert_events_handle_locked)(void *eventloop_instance, void *handle_instance);
	void (*backend_remove_events_handle_locked)(void *eventloop_instance, void *handle_instance);

	int (*backend_initialize_eventloop_thread)(void *instance);
	int (*backend_wait_for_events)(void *instance);
	void (*backend_process_changed_events_locked)(void *instance);
	void (*backend_scan_for_events_locked)(void *instance);
	void (*backend_continue_eventloop_thread_locked)(void *instance);
	void (*backend_cleanup_eventloop_thread)(void *instance);

	void (*backend_wakeup_eventloop_locked)(void *instance);
	void (*backend_clearwake_eventloop_locked)(void *instance);
	void (*backend_cleanup_eventloop)(void *instance);
};

struct events_backend {
	ASYNCIO_MUTEX_T mtx;
	int initialized;
	uint64_t refcount;

	decl_queue(struct events_loop, all_eventloops);

	struct events_backend *prev[NUMBER_EVENTS_BACKEND_QUEUES];
	struct events_backend *next[NUMBER_EVENTS_BACKEND_QUEUES];
};

#define EVENTS_BACKEND_INITIALIZER {ASYNCIO_MUTEX_INITIALIZER, 0}

#endif
