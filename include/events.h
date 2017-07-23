#ifndef EVENTS_H
#define EVENTS_H

#include <stdint.h>

#include "asyncio_threadpool.h"
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

	int (*wait)(struct events_handle *self, int old_cancelstate);
	int (*cancel)(struct events_handle *self);
	int (*acquire)(struct events_handle *self);
	void (*release)(struct events_handle *self);

	unsigned long refcount;

	struct events_handle *prev[NUMBER_EVENTS_HANDLE_QUEUES];
	struct events_handle *next[NUMBER_EVENTS_HANDLE_QUEUES];

	uint32_t threadpool_flags;

	int in_eventloop_database;
	int has_threadpool_handle;
	struct asyncio_threadpool_handle *threadpool_handle;

	int continued;
	int finished;
	ASYNCIO_COND_T finished_cond;

	void *instance;
};

struct events_loop {
	struct events_backend *backend;

	ASYNCIO_MUTEX_T mtx;
	unsigned long refcount;
	int stopped;
	int changed;

	struct asyncio_threadpool_handle *threadpool_handle;
	decl_queue(struct events_handle, all_handles);

	struct events_loop *prev[NUMBER_EVENTS_EVENTLOOP_QUEUES];
	struct events_loop *next[NUMBER_EVENTS_EVENTLOOP_QUEUES];

	int (*acquire)(struct events_loop *self);
	void (*release)(struct events_loop *self);

	int (*handle_init)(struct events_loop *eventloop, struct events_handle *handle, uint32_t threadpool_flags);
	void (*handle_cleanup_before_dispatch)(struct events_handle *handle);

	int (*dispatch_handle_to_eventloop)(struct events_loop *eventloop, struct events_handle *handle);
	int (*dispatch_handle_to_threadpool_locked)(struct events_handle *handle);

	void *instance;

	void (*backend_callback)(void *handle_instance, uint32_t threadpool_flags, int *continued);

	int (*backend_insert_events_handle_locked)(void *eventloop_instance, void *handle_instance);
	void (*backend_remove_events_handle_locked)(void *eventloop_instance, void *handle_instance);
	void (*backend_cleanup_events_handle)(void *eventloop_instance, void *handle_instance);

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

int asyncio_events_eventloop(struct events_loop *eventloop);

#endif
