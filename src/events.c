#include <stdint.h>

#include "threadpool.h"
#include "events.h"
#include "synchronization.h"
#include "threading.h"
#include "queue.h"
#include "logging.h"
#include "constants.h"

/* PROTOTYPES */
static uint32_t get_threadpool_flags(uint32_t fdevents_flags);

static void clear_changed_events_locked(struct events_loop *eventloop);
static void set_changed_events_locked(struct events_loop *eventloop);
static int has_changed_events_locked(const struct events_loop *eventloop);

static void notify_events_handle_finished(struct events_handle *handle);
static void fatal_error_on_handle(struct events_handle *handle);

static int events_dispatch_handle_to_loop(struct events_loop *eventloop, struct events_handle *handle);
static int events_handle_init(struct events_loop *eventloop, struct events_handle *handle, uint32_t flags);

static void execute_events_callback(void *arg);
static void events_threadpool_completed(void *arg);

static int events_dispatch_handle_to_threadpool_locked(struct events_handle *handle);
static void eventloop_thread(void *arg);
static void stop_eventloop_thread(struct events_loop *eventloop);

static int events_handle_wait(const struct events_handle *handle, int old_cancelstate);
static int events_handle_cancel(const struct events_handle *handle);
static int events_handle_acquire(const struct events_handle *handle);
static void events_handle_release(const struct events_handle *handle);

static int events_eventloop_acquire(struct events_loop *eventloop);
static void events_eventloop_release(struct events_loop *eventloop);

int events_backend_init(struct events_backend *backend);
int events_backend_eventloop(struct events_backend *backend, struct events_loop *eventloop);
void events_backend_cleanup(struct events_backend *backend);
/* END PROTOTYPES */

static uint32_t get_threadpool_flags(uint32_t fdevents_flags)
{
	uint32_t flags;

	/* Do not use CANCELLATIONS here, because we don't want
	 * execute_events_callback to be cancellable. It will set the user
	 * desired cancellation state/type there. */
	flags = THREADPOOL_FLAG_NONE;

	if (fdevents_flags & FDEVENT_FLAG_CONTRACTOR)
		flags |= THREADPOOL_FLAG_CONTRACTOR;

	return flags;
}

static void clear_changed_events_locked(struct events_loop *eventloop)
{
	eventloop->changed = 0;
}

static void set_changed_events_locked(struct events_loop *eventloop)
{
	int wake;

	if (!has_changed_events_locked(eventloop))
		wake = 1;
	else
		wake = 0;

	eventloop->changed = 1;

	if (wake)
		eventloop->backend_wakeup_eventloop_locked(eventloop->instance);
}

static int has_changed_events_locked(const struct events_loop *eventloop)
{
	return eventloop->changed;
}

static void notify_events_handle_finished(struct events_handle *handle)
{
	/* This function is responsible for ending the handle life cycle through the events module.
	 * A reference may still be held by users, but it stops existing as far as this module is concerned. */

	if (ASYNCIO_RWLOCK_WRLOCK(&handle->eventloop->lock) == 0) {
		queue_remove(&handle->eventloop->all_handles, handle);
		handle->finished = 1;

		/* Yes, other threads may still try to call fdevent_cancel, but that function
		 * will know not to touch the threadpool handle since we clear the has_threadpool_handle flag. */
		if (handle->has_threadpool_handle) {
			threadpool_release_handle(handle->threadpool_handle);
			handle->has_threadpool_handle = 0;
		}

		ASYNCIO_COND_BROADCAST(&handle->finished_cond);
		ASYNCIO_RWLOCK_UNLOCK(&handle->eventloop->lock);
	} else {
		ASYNCIO_ERROR("Failed to wrlock handle's events lock.\n");
	}

	events_handle_release(handle);
}

static void fatal_error_on_handle(struct events_handle *handle)
{
	/* TODO: add error conditions? */
	notify_events_handle_finished(handle);
}

static int events_dispatch_handle_to_loop(struct events_loop *eventloop, struct events_handle *handle)
{
	if (ASYNCIO_RWLOCK_WRLOCK(&eventloop->lock) != 0) {
		ASYNCIO_ERROR("Failed to wrlock eventloop lock.\n");
		return -1;
	}

	if (eventloop->backend_insert_events_handle_locked(eventloop->instance, handle->instance) != 0) {
		ASYNCIO_ERROR("Failed to insert events handle.\n");
		ASYNCIO_RWLOCK_UNLOCK(&eventloop->lock);
		return -1;
	}

	handle->in_worker_database = 1;
	set_changed_fdevents_locked(eventloop);

	queue_push(&eventloop->all_handles, handle);

	ASYNCIO_RWLOCK_UNLOCK(&eventloop->lock);
	return 0;
}

static int events_handle_init(struct events_loop *eventloop, struct events_handle *handle, uint32_t flags)
{
	handle->wait = events_handle_wait;
	handle->cancel = events_handle_cancel;
	handle->acquire = events_handle_acquire;
	handle->release = events_handle_release;

	/* The caller must have a reference by default to prevent race conditions
	 * where the event completes before the client had a chance to acquire the
	 * handle. Also the fdevents loop thread awakened must have a reference to
	 * prevent the case where the client releases its handle before the loop
	 * manages to acquire its handle */
	handle->refcount = 2;
	handle->flags = flags;
	handle->in_eventloop_database = 0;
	handle->has_threadpool_handle = 0;
	handle->continued = 0;
	handle->finished = 0;

	if (ASYNCIO_COND_INIT(&handle->finished_cond) != 0) {
		ASYNCIO_ERROR("Failed to init handle finished_cond.\n");
		return -1;
	}

	return 0;
}

static void execute_events_callback(void *arg)
{
	struct events_handle *handle;

	handle = (struct events_handle *)arg;	/* XXX explicit cast? */
	handle->continued = 0;

	/* Execute the backend callback */
	handle->backend_callback(handle->instance, handle->flags, &handle->continued);
}

static void events_threadpool_completed(void *arg)
{
	struct events_handle *handle;

	handle = (struct events_handle *)arg;

	if (ASYNCIO_RWLOCK_WRLOCK(&handle->eventloop->lock) != 0) {
		ASYNCIO_ERROR("Failed to wrlock handle's eventloop lock.\n");
		fatal_error_on_handle(handle);
		return;
	}

	/* No danger of double release of handle (in notify_events_handle_finished)
	 * because it checks whether there actually is a threadpool handle.
	 * But the point is that we want to release the old threadpool handle even if
	 * the events handle's lifecycle is not done, in case it is continued. */
	threadpool_release_handle(handle->threadpool_handle);
	handle->has_threadpool_handle = 0;

	if (handle->continued) {
		handle->continued = 0;

		if (handle->backend_insert_events_handle_locked(handle->eventloop->instance, handle->instance) != 0) {
			ASYNCIO_ERROR("Failed to re-dispatch handle to loop.\n");
			ASYNCIO_RWLOCK_UNLOCK(&handle->eventloop->lock);
			fatal_error_on_handle(handle);
			return;
		}

		handle->in_worker_database = 1;
		set_changed_events_locked(handle->eventloop);
		ASYNCIO_RWLOCK_UNLOCK(&handle->eventloop->lock);
	} else {
		ASYNCIO_RWLOCK_UNLOCK(&handle->eventloop->lock);
		notify_events_handle_finished(handle);
	}
}

static int events_dispatch_handle_to_threadpool_locked(struct events_handle *handle)
{
	struct threadpool_dispatch_info info;
	threadpool_handle_t threadpool_handle;

	info.flags = get_threadpool_flags(handle->flags);
	info.dispatch_info.fn = execute_events_callback;
	info.dispatch_info.arg = handle;
	info.completed_info.cb = events_threadpool_completed;
	info.completed_info.arg = handle;
	info.cancelled_info.cb = events_threadpool_completed;
	info.cancelled_info.arg = handle;

	if (threadpool_dispatch(&info, &threadpool_handle) != 0) {
		ASYNCIO_ERROR("Failed to dispatch events handle to threadpool.\n");
		/* Don't remove, maybe it will work next time? */
		return -1;
	}

	/* At this point the handle refcount is at least 2:
	 * - the client thread that got the handle
	 * - the events_loop worker */
	handle->backend_remove_events_handle_locked(handle->eventloop->instance, handle->instance);
	handle->threadpool_handle = threadpool_handle;
	handle->has_threadpool_handle = 1;
	handle->in_worker_database = 0;

	return 0;
}

static void eventloop_thread(void *arg)
{
	struct events_loop *eventloop;
	struct events_handle *handle, *next;
	struct threadpool_dispatch_info info;
	threadpool_handle_t threadpool_handle;

	eventloop = (struct events_loop *)arg;

	if (eventloop->backend_initialize_eventloop_thread(eventloop->instance) != 0) {
		ASYNCIO_ERROR("Failed to initialize backend eventloop thread.\n");
		return;
	}

	for (;;) {
		if (eventloop->backend_wait_for_events(eventloop->instance) != 0) {
			ASYNCIO_SYSERROR("Failed to wait for events.\n");
			break;
		}

		if (ASYNCIO_RWLOCK_WRLOCK(&eventloop->lock) != 0) {
			ASYNCIO_ERROR("Failed to wrlock eventloop lock.\n");
			/* XXX Have backend error callback? */
			break;
		}

		/* Stop condition for eventloop. */
		if (eventloop->stopped) {
			ASYNCIO_RWLOCK_UNLOCK(&eventloop->lock);
			break;
		}

		/* Check if events database has changed. */
		if (eventloop->changed) {
			eventloop->backend_process_changed_events_locked(eventloop->instance);
			eventloop->backend_clearwake_eventloop_locked(eventloop->instance);
			eventloop->changed = 0;
			ASYNCIO_RWLOCK_UNLOCK(&eventloop->lock);
			continue;
		}

		/* Scan for events */
		/* XXX Maybe make an iterator instead */
		eventloop->backend_scan_for_events_locked(eventloop->instance);

		eventloop->backend_continue_eventloop_thread_locked(eventloop->instance);
		ASYNCIO_RWLOCK_UNLOCK(&eventloop->lock);
	}

	eventloop->backend_cleanup_eventloop_thread(eventloop->instance);
}

static void stop_eventloop_thread(struct events_loop *eventloop)
{
	if (ASYNCIO_RWLOCK_WRLOCK(&eventloop->lock)) {
		ASYNCIO_ERROR("Failed to wrlock eventloop lock.\n");
		return;
	}

	eventloop->stopped = 1;
	eventloop->backend_wakeup_eventloop_locked(eventloop->instance);

	ASYNCIO_RWLOCK_UNLOCK(&eventloop->lock);

	if (threadpool_join(&eventloop->threadpool_handle) != 0)
		ASYNCIO_ERROR("Failed to join on eventloop threadpool handle.\n");

	threadpool_release_handle(&eventloop->threadpool_handle);
}

static int events_handle_wait(const struct events_handle *handle, int old_cancelstate)
{
	struct events_handle *handle;
	int oldstate;
	int oldtype;
	int success = 1;

	if (ASYNCIO_RWLOCK_RDLOCK(&handle->eventloop->lock) != 0) {
		ASYNCIO_ERROR("Failed to rdlock fdevents lock.\n");
		return -1;
	}

	handle = (struct events_handle *)handle;

	/* Unlock the eventloop lock in cleanup handler if cancelled here */
	ASYNCIO_CLEANUP_PUSH(unlock_eventloop_lock_cleanup, handle->eventloop);

	/* Restore cancelstate while waiting for condition variable
	 * to allow cancellation in this case. But set cancellation type to DEFERRED
	 * in order to make sure we cancel during ASYNCIO_COND_WAIT, which should guarantee
	 * that the eventloop lock is locked during the cleanup handler. */
	set_canceltype(ASYNCIO_CANCEL_DEFERRED, &oldtype);
	restore_cancelstate(old_cancelstate);

	while (!(handle->finished)) {
		if (ASYNCIO_COND_WAIT(&handle->finished_cond, &handle->eventloop->lock) != 0) {
			ASYNCIO_ERROR("Failed to wait on handle finished_cond.\n");
			success = 0;
			break;
		}
	}

	disable_cancellations(&oldstate);
	restore_canceltype(oldtype);

	/* Unlock eventloop lock. */
	ASYNCIO_CLEANUP_POP(1);

	if (success)
		return 0;
	else
		return -1;
}

static int events_handle_cancel(const struct events_handle *handle)
{
	int success = 1;
	int notify = 0;

	if (ASYNCIO_RWLOCK_WRLOCK(&handle->eventloop->lock) != 0) {
		ASYNCIO_ERROR("Failed to wrlock handle's eventloop lock.\n");
		return -1;
	}

	if (handle->in_worker_database) {
		/* Should never happen, but just putting this here to detect potential bugs... */
		if (handle->has_threadpool_handle)
			ASYNCIO_ERROR("This should never happen. The events handle is in worker database and has threadpool handle.\n");

		/* Means it has not yet been dispatched to threadpool, so eventloop won't have access to handle anymore.
		 * Note that if it was in a thread, it will have access due to the cancelled callback. */
		handle->eventloop->backend_remove_events_handle_locked(handle->eventloop->instance, handle->instance);
		set_changed_events_locked(handle->eventloop);
		notify = 1;
		success = 1;
	} else {
		notify = 0;
		success = 1;

		if (threadpool_cancel(handle->threadpool_handle) != 0) {
			ASYNCIO_ERROR("Failed to cancel threadpool handle.\n");
			success = 0;
		}
	}

	ASYNCIO_RWLOCK_UNLOCK(&handle->eventloop->lock);

	if (notify)
		notify_events_handle_finished(handle);

	if (success)
		return 0;
	else
		return -1;
}

static int events_handle_acquire(const struct events_handle *handle)
{
	/* Note: In general, only one who owns a reference to handle can acquire it, for someone else.
	 * This is to prevent race-conditions where last owner releases reference while someone else
	 * acquires it (if the acquirer loses the lock race, they get dangling pointer) */
	if (ASYNCIO_RWLOCK_WRLOCK(&handle->eventloop->lock) != 0) {
		ASYNCIO_ERROR("Failed to wrlock handle's eventloop lock.\n");
		return -1;
	}

	if (handle->refcount == UINT64T_MAX) {
		ASYNCIO_ERROR("Reached maximum refcount for handle.");
		ASYNCIO_RWLOCK_UNLOCK(&handle->eventloop->lock);
		return -1;
	}

	++(handle->refcount);
	ASYNCIO_RWLOCK_UNLOCK(&handle->eventloop->lock);

	return 0;
}

static void events_handle_release(const struct events_handle *handle)
{
	if (ASYNCIO_RWLOCK_WRLOCK(&handle->eventloop->lock) != 0) {
		ASYNCIO_ERROR("Failed to wrlock handle's eventloop lock.\n");
		return;
	}

	if (handle->refcount == 0) {
		/* Shouldn't happen */
		ASYNCIO_ERROR("events_handle refcount was 0 before dereference.\n");
		return;
	}

	--(handle->refcount);

	if (handle->refcount > 0) {
		ASYNCIO_RWLOCK_UNLOCK(&handle->eventloop->lock);
		return;
	}

	ASYNCIO_RWLOCK_UNLOCK(&handle->eventloop->lock);

	ASYNCIO_COND_DESTROY(&handle->finished_cond);
	handle->backend_cleanup_handle(handle->instance);
}

static int events_eventloop_acquire(struct events_loop *eventloop)
{
	if (ASYNCIO_RWLOCK_WRLOCK(&eventloop->lock) != 0) {
		ASYNCIO_ERROR("Failed to wrlock eventloop lock.\n");
		return -1;
	}

	if (eventloop->refcount == UINT64T_MAX) {
		ASYNCIO_ERROR("Refcount for eventloop reached maximum.\n");
		ASYNCIO_RWLOCK_UNLOCK(&eventloop->lock);
		return -1;
	}

	++(eventloop->refcount);
	ASYNCIO_RWLOCK_UNLOCK(&eventloop->lock);
	return 0;
}

static void events_eventloop_release(struct events_loop *eventloop)
{
	struct events_handle *handle;

	if (ASYNCIO_RWLOCK_WRLOCK(&eventloop->lock) != 0) {
		ASYNCIO_ERROR("Failed to wrlock eventloop lock.\n");
		return;
	}

	if (eventloop->refcount == 0) {
		ASYNCIO_ERROR("Refcount for eventloop already 0.\n");
		ASYNCIO_RWLOCK_UNLOCK(&eventloop->lock);
		return;
	}

	--(eventloop->refcount);

	if (eventloop->refcount > 0) {
		ASYNCIO_RWLOCK_UNLOCK(&eventloop->lock);
		return;
	}

	stop_eventloop_thread(eventloop);

	while (!queue_empty(&eventloop->all_handles)) {
		queue_pop(&eventloop->all_handles, &handle);

		if (handle->has_threadpool_handle) {
			if (threadpool_join(handle->threadpool_handle) != 0)
				ASYNCIO_ERROR("Failed to join events handle's threadpool handle.\n");
		}

		events_handle_finished(handle);
	}

	threadpool_cleanup();
	eventloop->backend_cleanup_eventloop(eventloop->instance);

	ASYNCIO_RWLOCK_UNLOCK(&eventloop->lock);
	ASYNCIO_RWLOCK_DESTROY(&eventloop->lock);
}

int events_backend_init(struct events_backend *backend)
{
	if (ASYNCIO_RWLOCK_WRLOCK(&backend->lock) != 0) {
		ASYNCIO_ERROR("Failed to wrlock backend lock.\n");
		return -1;
	}

	if (!(backend->initialized)) {
		queue_init(&backend->all_eventloops);
		backend->refcount = 0;
		backend->initialized = 1;
	}

	if (backend->refcount == UINT64T_MAX) {
		ASYNCIO_ERROR("Backend refcount reached maximum.\n");
		ASYNCIO_RWLOCK_UNLOCK(&backend->lock);
		return -1;
	}

	++(backend->refcount);

	ASYNCIO_RWLOCK_UNLOCK(&backend->lock);
	return 0;
}

int events_backend_eventloop(struct events_backend *backend, struct events_loop *eventloop)
{
	struct threadpool_dispatch_info eventloop_task;

	if (threadpool_init() != 0) {
		ASYNCIO_ERROR("Failed to init threadpool.\n");
		return -1;
	}

	/* One refcount for the caller. The eventloop_thread itself doesn't get a reference
	 * even though it has access to pointer, because when eventloop is released, there
	 * will be a stop signal on the thread and release will wait until it has finished. */
	eventloop->refcount = 1;
	eventloop->backend = backend;
	eventloop->acquire = events_eventloop_acquire;
	eventloop->release = events_eventloop_release;

	queue_init(&eventloop->all_handles, EVENTS_HANDLE_QUEUE_ID);
	eventloop->stopped = 0;
	eventloop->changed = 0;

	if (ASYNCIO_RWLOCK_INIT(&eventloop->lock) != 0) {
		ASYNCIO_ERROR("Failed to init eventloop lock.\n");
		threadpool_cleanup();
		return -1;
	}

	eventloop_task.flags = THREADPOOL_FLAG_CONTRACTOR;
	eventloop_task.dispatch_info.fn = eventloop_thread;
	eventloop_task.dispatch_info.arg = eventloop;
	eventloop_task.completed_info.cb = NULL;
	eventloop_task.completed_info.arg = NULL;
	eventloop_task.cancelled_info.cb = NULL;
	eventloop_task.cancelled_info.arg = NULL;

	if (threadpool_dispatch(&eventloop_task, &eventloop->threadpool_handle) != 0) {
		ASYNCIO_ERROR("Failed to dispatch eventloop task.\n");
		ASYNCIO_RWLOCK_DESTROY(&eventloop->lock);
		threadpool_cleanup();
		return -1;
	}

	return 0;
}

void events_backend_cleanup(struct events_backend *backend)
{
	struct events_loop *eventloop;

	if (ASYNCIO_RWLOCK_WRLOCK(&backend->lock) != 0) {
		ASYNCIO_ERROR("Failed to wrlock backend lock.\n");
		return -1;
	}

	if (!(backend->initialized)) {
		ASYNCIO_RWLOCK_UNLOCK(&backend->lock);
		return 0;
	}

	if (backend->refcount == 0) {
		ASYNCIO_ERROR("Backend refcount already 0, should never happen.\n");
		ASYNCIO_RWLOCK_UNLOCK(&backend->lock);
		return -1;
	}

	--(backend->refcount);

	if (backend->refcount > 0) {
		ASYNCIO_RWLOCK_UNLOCK(&backend->lock);
		return 0;
	}

	backend->initialized = 0;

	while (!queue_empty(&backend->all_eventloops)) {
		queue_pop(&backend->all_eventloops, &eventloop);
		eventloop->release(eventloop);
	}

	ASYNCIO_RWLOCK_UNLOCK(&backend->lock);
	return 0;
}
