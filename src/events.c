#include <stdint.h>
#include <limits.h>

#include "asyncio_threadpool.h"
#include "events.h"
#include "threading.h"
#include "queue.h"
#include "logging.h"

/* PROTOTYPES */
static void unlock_eventloop_mtx_cleanup(void *arg);

static void set_changed_events_locked(struct events_loop *eventloop);

static void notify_events_handle_finished(struct events_handle *handle);
static void fatal_error_on_handle(struct events_handle *handle);

static int events_dispatch_handle_to_eventloop(struct events_loop *eventloop, struct events_handle *handle);
static int events_handle_init(struct events_loop *eventloop, struct events_handle *handle, uint32_t threadpool_flags);
static void events_handle_cleanup_before_dispatch(struct events_handle *handle);

static void execute_events_callback(void *arg);
static void events_threadpool_completed(void *arg);
static void events_threadpool_cancelled(void *arg);
static int events_dispatch_handle_to_threadpool_locked(struct events_handle *handle);

static void eventloop_thread(void *arg);
static void stop_eventloop_thread_locked(struct events_loop *eventloop);

static int events_handle_wait(struct events_handle *handle, int old_cancelstate);
static int events_handle_cancel(struct events_handle *handle);
static int events_handle_acquire(struct events_handle *handle);
static void events_handle_release(struct events_handle *handle);

static int events_eventloop_acquire(struct events_loop *eventloop);
static void events_eventloop_release(struct events_loop *eventloop);
/* END PROTOTYPES */

static void unlock_eventloop_mtx_cleanup(void *arg)
{
	struct events_loop *eventloop;

	eventloop = arg;
	ASYNCIO_MUTEX_UNLOCK(&eventloop->mtx);
}

static void set_changed_events_locked(struct events_loop *eventloop)
{
	int wake;

	if (!(eventloop->changed))
		wake = 1;
	else
		wake = 0;

	eventloop->changed = 1;

	if (wake)
		eventloop->backend_wakeup_eventloop_locked(eventloop->instance);
}

static void notify_events_handle_finished(struct events_handle *handle)
{
	/* This function is responsible for ending the handle life cycle through the events module.
	 * A reference may still be held by users, but it stops existing as far as this module is concerned. */

	if (ASYNCIO_MUTEX_LOCK(&handle->eventloop->mtx) == 0) {
		queue_remove(&handle->eventloop->all_handles, handle);
		handle->finished = 1;

		/* Yes, other threads may still try to call events_handle_cancel, but that function
		 * will know not to touch the threadpool handle since we clear the has_threadpool_handle flag. */
		if (handle->has_threadpool_handle) {
			asyncio_threadpool_release_handle(handle->threadpool_handle);
			handle->has_threadpool_handle = 0;
		}

		ASYNCIO_COND_BROADCAST(&handle->finished_cond);
		ASYNCIO_MUTEX_UNLOCK(&handle->eventloop->mtx);
	} else {
		ASYNCIO_ERROR("Failed to lock handle's events mtx.\n");
	}

	events_handle_release(handle);
}

static void fatal_error_on_handle(struct events_handle *handle)
{
	/* TODO: add error conditions? */
	notify_events_handle_finished(handle);
}

static int events_dispatch_handle_to_eventloop(struct events_loop *eventloop, struct events_handle *handle)
{
	if (ASYNCIO_MUTEX_LOCK(&eventloop->mtx) != 0) {
		ASYNCIO_ERROR("Failed to lock eventloop mtx.\n");
		return -1;
	}

	if (eventloop->backend_insert_events_handle_locked(eventloop->instance, handle->instance) != 0) {
		ASYNCIO_ERROR("Failed to insert events handle.\n");
		ASYNCIO_MUTEX_UNLOCK(&eventloop->mtx);
		return -1;
	}

	handle->in_eventloop_database = 1;

	set_changed_events_locked(eventloop);
	queue_push(&eventloop->all_handles, handle);

	ASYNCIO_MUTEX_UNLOCK(&eventloop->mtx);
	return 0;
}

static int events_handle_init(struct events_loop *eventloop, struct events_handle *handle, uint32_t threadpool_flags)
{
	handle->eventloop = eventloop;

	handle->wait = events_handle_wait;
	handle->cancel = events_handle_cancel;
	handle->acquire = events_handle_acquire;
	handle->release = events_handle_release;

	/* The caller must have a reference by default to prevent race conditions
	 * where the event completes before the client had a chance to acquire the
	 * handle. Also the events loop thread awakened must have a reference to
	 * prevent the case where the client releases its handle before the loop
	 * manages to acquire its handle */
	handle->refcount = 2;
	handle->threadpool_flags = threadpool_flags;
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

static void events_handle_cleanup_before_dispatch(struct events_handle *handle)
{
	/* Only used if initialization in backend failed before dispatch to loop. */
	ASYNCIO_COND_DESTROY(&handle->finished_cond);
}

static void execute_events_callback(void *arg)
{
	struct events_handle *handle;
	int oldstate;

	/* Disable cancellations if they were set. They will get set again in backend callback. */
	ASYNCIO_DISABLE_CANCELLATIONS(&oldstate);

	handle = arg;
	handle->continued = 0;

	/* Execute the backend callback */
	handle->eventloop->backend_callback(handle->instance, handle->threadpool_flags, &handle->continued);
}

static void events_threadpool_completed(void *arg)
{
	struct events_handle *handle;

	handle = arg;

	if (ASYNCIO_MUTEX_LOCK(&handle->eventloop->mtx) != 0) {
		ASYNCIO_ERROR("Failed to lock handle's eventloop mtx.\n");
		fatal_error_on_handle(handle);
		return;
	}

	/* No danger of double release of handle (in notify_events_handle_finished)
	 * because it checks whether there actually is a threadpool handle.
	 * But the point is that we want to release the old threadpool handle even if
	 * the events handle's lifecycle is not done, in case it is continued. */
	asyncio_threadpool_release_handle(handle->threadpool_handle);
	handle->has_threadpool_handle = 0;

	if ((!(handle->eventloop->stopped)) && handle->continued) {
		handle->continued = 0;

		if (handle->eventloop->backend_insert_events_handle_locked(handle->eventloop->instance, handle->instance) != 0) {
			ASYNCIO_ERROR("Failed to re-dispatch handle to loop.\n");
			ASYNCIO_MUTEX_UNLOCK(&handle->eventloop->mtx);
			fatal_error_on_handle(handle);
			return;
		}

		handle->in_eventloop_database = 1;
		set_changed_events_locked(handle->eventloop);
		ASYNCIO_MUTEX_UNLOCK(&handle->eventloop->mtx);
	} else {
		ASYNCIO_MUTEX_UNLOCK(&handle->eventloop->mtx);
		notify_events_handle_finished(handle);
	}
}

static void events_threadpool_cancelled(void *arg)
{
	struct events_handle *handle;

	handle = arg;

	if (ASYNCIO_MUTEX_LOCK(&handle->eventloop->mtx) != 0) {
		ASYNCIO_ERROR("Failed to lock handle's eventloop mtx.\n");
		fatal_error_on_handle(handle);
		return;
	}

	/* No danger of double release of handle (in notify_events_handle_finished)
	 * because it checks whether there actually is a threadpool handle.
	 * But the point is that we want to release the old threadpool handle even if
	 * the events handle's lifecycle is not done, in case it is continued. */
	asyncio_threadpool_release_handle(handle->threadpool_handle);
	handle->has_threadpool_handle = 0;

	/* Call the backend's cancelled callback. */
	handle->eventloop->backend_cancelled_callback(handle->instance);

	ASYNCIO_MUTEX_UNLOCK(&handle->eventloop->mtx);
	notify_events_handle_finished(handle);
}

static int events_dispatch_handle_to_threadpool_locked(struct events_handle *handle)
{
	struct asyncio_threadpool_dispatch_info info;
	struct asyncio_threadpool_handle *threadpool_handle;

	info.flags = handle->threadpool_flags;
	info.dispatch_info.fn = execute_events_callback;
	info.dispatch_info.arg = handle;
	info.completed_info.cb = events_threadpool_completed;
	info.completed_info.arg = handle;
	info.cancelled_info.cb = events_threadpool_cancelled;
	info.cancelled_info.arg = handle;

	if (asyncio_threadpool_dispatch(&info, &threadpool_handle) != 0) {
		ASYNCIO_ERROR("Failed to dispatch events handle to threadpool.\n");
		/* Don't remove, maybe it will work next time? */
		return -1;
	}

	/* At this point the handle refcount is at least 2:
	 * - the client thread that got the handle
	 * - the events_loop worker */
	handle->eventloop->backend_remove_events_handle_locked(handle->eventloop->instance, handle->instance);
	handle->in_eventloop_database = 0;
	handle->threadpool_handle = threadpool_handle;
	handle->has_threadpool_handle = 1;

	return 0;
}

static void eventloop_thread(void *arg)
{
	struct events_loop *eventloop;

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

		if (ASYNCIO_MUTEX_LOCK(&eventloop->mtx) != 0) {
			ASYNCIO_ERROR("Failed to lock eventloop mtx.\n");
			/* XXX Have backend error callback? */
			break;
		}

		/* Stop condition for eventloop. */
		if (eventloop->stopped) {
			ASYNCIO_MUTEX_UNLOCK(&eventloop->mtx);
			break;
		}

		/* Check if events database has changed. */
		if (eventloop->changed) {
			eventloop->backend_process_changed_events_locked(eventloop->instance);
			eventloop->backend_clearwake_eventloop_locked(eventloop->instance);
			eventloop->changed = 0;
			ASYNCIO_MUTEX_UNLOCK(&eventloop->mtx);
			continue;
		}

		/* Scan for events */
		/* XXX Maybe make an iterator instead */
		eventloop->backend_scan_for_events_locked(eventloop->instance);

		eventloop->backend_continue_eventloop_thread_locked(eventloop->instance);
		ASYNCIO_MUTEX_UNLOCK(&eventloop->mtx);
	}

	eventloop->backend_cleanup_eventloop_thread(eventloop->instance);
}

static void stop_eventloop_thread_locked(struct events_loop *eventloop)
{
	eventloop->stopped = 1;
	eventloop->backend_wakeup_eventloop_locked(eventloop->instance);
}

static int events_handle_wait(struct events_handle *handle, int old_cancelstate)
{
	int oldstate;
	int oldtype;
	int success;

	if (ASYNCIO_MUTEX_LOCK(&handle->eventloop->mtx) != 0) {
		ASYNCIO_ERROR("Failed to lock eventloop mtx.\n");
		return -1;
	}

	/* Unlock the eventloop mtx in cleanup handler if cancelled here */
	ASYNCIO_CLEANUP_PUSH(unlock_eventloop_mtx_cleanup, handle->eventloop);

	/* Restore cancelstate while waiting for condition variable
	 * to allow cancellation in this case. But set cancellation type to DEFERRED
	 * in order to make sure we cancel during ASYNCIO_COND_WAIT, which should guarantee
	 * that the eventloop mtx is locked during the cleanup handler. */
	ASYNCIO_SET_CANCELTYPE(ASYNCIO_CANCEL_DEFERRED, &oldtype);
	ASYNCIO_RESTORE_CANCELSTATE(old_cancelstate);

	success = 1;

	while (!(handle->finished)) {
		if (ASYNCIO_COND_WAIT(&handle->finished_cond, &handle->eventloop->mtx) != 0) {
			ASYNCIO_ERROR("Failed to wait on handle finished_cond.\n");
			success = 0;
			break;
		}
	}

	ASYNCIO_DISABLE_CANCELLATIONS(&oldstate);
	ASYNCIO_RESTORE_CANCELTYPE(oldtype);

	/* Unlock eventloop mtx. */
	ASYNCIO_CLEANUP_POP(1);

	if (success)
		return 0;
	else
		return -1;
}

static int events_handle_cancel(struct events_handle *handle)
{
	int success = 1;
	int notify = 0;

	/* NOTE: THIS SHALL NOT BE CALLED BEFORE events_dispatch_handle_to_eventloop! */

	if (ASYNCIO_MUTEX_LOCK(&handle->eventloop->mtx) != 0) {
		ASYNCIO_ERROR("Failed to lock handle's eventloop mtx.\n");
		return -1;
	}

	if (!(handle->threadpool_flags & ASYNCIO_THREADPOOL_FLAG_CANCELLABLE)) {
		ASYNCIO_ERROR("Tried to cancel handle without the ASYNCIO_THREADPOOL_FLAG_CANCELLABLE flag.\n");
		return -1;
	}

	if (handle->has_threadpool_handle) {
		notify = 0;
		success = 1;

		if (asyncio_threadpool_cancel(handle->threadpool_handle) != 0) {
			ASYNCIO_ERROR("Failed to cancel threadpool handle.\n");
			success = 0;
		}
	} else if (handle->in_eventloop_database) {
		/* Call the cancelled callback since we never got to threadpool. */
		handle->eventloop->backend_cancelled_callback(handle->instance);

		/* Means it has not yet been dispatched to threadpool, so eventloop won't have access to handle anymore.
		 * Note that if it was in a thread, it will have access due to the cancelled callback. */
		handle->eventloop->backend_remove_events_handle_locked(handle->eventloop->instance, handle->instance);
		handle->in_eventloop_database = 0;
		set_changed_events_locked(handle->eventloop);
		notify = 1;
		success = 1;
	} else {
		notify = 0;
		success = 1;
		/* Means the handle has finished, so do nothing. */
	}

	ASYNCIO_MUTEX_UNLOCK(&handle->eventloop->mtx);

	if (notify)
		notify_events_handle_finished(handle);

	if (success)
		return 0;
	else
		return -1;
}

static int events_handle_acquire(struct events_handle *handle)
{
	/* Note: In general, only one who owns a reference to handle can acquire it, for someone else.
	 * This is to prevent race-conditions where last owner releases reference while someone else
	 * acquires it (if the acquirer loses the lock race, they get dangling pointer) */
	if (ASYNCIO_MUTEX_LOCK(&handle->eventloop->mtx) != 0) {
		ASYNCIO_ERROR("Failed to lock handle's eventloop mtx.\n");
		return -1;
	}

	if (handle->refcount == ULONG_MAX) {
		ASYNCIO_ERROR("Reached maximum refcount for handle.");
		ASYNCIO_MUTEX_UNLOCK(&handle->eventloop->mtx);
		return -1;
	}

	++(handle->refcount);
	ASYNCIO_MUTEX_UNLOCK(&handle->eventloop->mtx);

	return 0;
}

static void events_handle_release(struct events_handle *handle)
{
	if (ASYNCIO_MUTEX_LOCK(&handle->eventloop->mtx) != 0) {
		ASYNCIO_ERROR("Failed to lock handle's eventloop mtx.\n");
		return;
	}

	if (handle->refcount == 0) {
		/* Shouldn't happen */
		ASYNCIO_ERROR("events_handle refcount was 0 before dereference.\n");
		return;
	}

	--(handle->refcount);

	if (handle->refcount > 0) {
		ASYNCIO_MUTEX_UNLOCK(&handle->eventloop->mtx);
		return;
	}

	ASYNCIO_MUTEX_UNLOCK(&handle->eventloop->mtx);

	ASYNCIO_COND_DESTROY(&handle->finished_cond);
	handle->eventloop->backend_cleanup_events_handle(handle->eventloop->instance, handle->instance);
}

static int events_eventloop_acquire(struct events_loop *eventloop)
{
	if (ASYNCIO_MUTEX_LOCK(&eventloop->mtx) != 0) {
		ASYNCIO_ERROR("Failed to lock eventloop mtx.\n");
		return -1;
	}

	if (eventloop->refcount == ULONG_MAX) {
		ASYNCIO_ERROR("Refcount for eventloop reached maximum.\n");
		ASYNCIO_MUTEX_UNLOCK(&eventloop->mtx);
		return -1;
	}

	++(eventloop->refcount);
	ASYNCIO_MUTEX_UNLOCK(&eventloop->mtx);
	return 0;
}

static void events_eventloop_release(struct events_loop *eventloop)
{
	struct events_handle *handle;

	if (ASYNCIO_MUTEX_LOCK(&eventloop->mtx) != 0) {
		ASYNCIO_ERROR("Failed to lock eventloop mtx.\n");
		return;
	}

	if (eventloop->refcount == 0) {
		ASYNCIO_ERROR("Refcount for eventloop already 0.\n");
		ASYNCIO_MUTEX_UNLOCK(&eventloop->mtx);
		return;
	}

	--(eventloop->refcount);

	if (eventloop->refcount > 0) {
		ASYNCIO_MUTEX_UNLOCK(&eventloop->mtx);
		return;
	}

	stop_eventloop_thread_locked(eventloop);

	ASYNCIO_MUTEX_UNLOCK(&eventloop->mtx);

	if (asyncio_threadpool_join(eventloop->threadpool_handle) != 0)
		ASYNCIO_ERROR("Failed to join on eventloop threadpool handle.\n");

	asyncio_threadpool_release_handle(eventloop->threadpool_handle);

	if (ASYNCIO_MUTEX_LOCK(&eventloop->mtx) != 0) {
		ASYNCIO_ERROR("Failed to lock eventloop mtx. This shouldn't happen, now we can't release ressources.\n");
		return;
	}

	while (!queue_empty(&eventloop->all_handles)) {
		handle = queue_first(&eventloop->all_handles);

		if (handle->in_eventloop_database) {
			handle->eventloop->backend_remove_events_handle_locked(handle->eventloop->instance, handle->instance);
			handle->in_eventloop_database = 0;
		}

		if (handle->has_threadpool_handle) {
			ASYNCIO_MUTEX_UNLOCK(&eventloop->mtx);

			if (asyncio_threadpool_join(handle->threadpool_handle) != 0)
				ASYNCIO_ERROR("Failed to join event handle's threadpool handle.\n");
		} else {
			/* Call the cancelled callback since threadpool won't do it. */
			handle->eventloop->backend_cancelled_callback(handle->instance);

			/* Only notify if not in threadpool, because otherwise it gets done in completed callback. */
			ASYNCIO_MUTEX_UNLOCK(&eventloop->mtx);
			notify_events_handle_finished(handle);
		}

		if (ASYNCIO_MUTEX_LOCK(&eventloop->mtx) != 0) {
			ASYNCIO_ERROR("Failed to lock eventloop mtx. This shouldn't happen, now we can't release ressources.\n");
			break;
		}
	}

	ASYNCIO_MUTEX_UNLOCK(&eventloop->mtx);
	asyncio_threadpool_cleanup();

	/* Destroy only once all handles have finished, but before backend cleanup. */
	ASYNCIO_MUTEX_DESTROY(&eventloop->mtx);
	eventloop->backend_cleanup_eventloop(eventloop->instance);
}

int asyncio_events_eventloop(struct events_loop *eventloop)
{
	struct asyncio_threadpool_dispatch_info eventloop_task;

	if (asyncio_threadpool_init() != 0) {
		ASYNCIO_ERROR("Failed to init threadpool.\n");
		return -1;
	}

	/* One refcount for the caller. The eventloop_thread itself doesn't get a reference
	 * even though it has access to pointer, because when eventloop is released, there
	 * will be a stop signal on the thread and release will wait until it has finished. */
	eventloop->refcount = 1;
	eventloop->acquire = events_eventloop_acquire;
	eventloop->release = events_eventloop_release;
	eventloop->handle_init = events_handle_init;
	eventloop->handle_cleanup_before_dispatch = events_handle_cleanup_before_dispatch;
	eventloop->dispatch_handle_to_eventloop = events_dispatch_handle_to_eventloop;
	eventloop->dispatch_handle_to_threadpool_locked = events_dispatch_handle_to_threadpool_locked;

	queue_init(&eventloop->all_handles, EVENTS_HANDLE_QUEUE_ID);
	eventloop->stopped = 0;
	eventloop->changed = 0;

	if (ASYNCIO_MUTEX_INIT(&eventloop->mtx) != 0) {
		ASYNCIO_ERROR("Failed to init eventloop mtx.\n");
		asyncio_threadpool_cleanup();
		return -1;
	}

	eventloop_task.flags = ASYNCIO_THREADPOOL_FLAG_CONTRACTOR;
	eventloop_task.dispatch_info.fn = eventloop_thread;
	eventloop_task.dispatch_info.arg = eventloop;
	eventloop_task.completed_info.cb = NULL;
	eventloop_task.completed_info.arg = NULL;
	eventloop_task.cancelled_info.cb = NULL;
	eventloop_task.cancelled_info.arg = NULL;

	if (asyncio_threadpool_dispatch(&eventloop_task, &eventloop->threadpool_handle) != 0) {
		ASYNCIO_ERROR("Failed to dispatch eventloop task.\n");
		ASYNCIO_MUTEX_DESTROY(&eventloop->mtx);
		asyncio_threadpool_cleanup();
		return -1;
	}

	return 0;
}
