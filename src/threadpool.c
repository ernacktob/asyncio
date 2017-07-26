#include <stdlib.h>
#include <stdint.h>
#include <limits.h>

#include <unistd.h>
#include <signal.h>

#include "asyncio_threadpool.h"
#include "queue.h"
#include "safe_malloc.h"
#include "logging.h"
#include "threading.h"

#define MAX_WORKER_THREADS		5
#define MAX_CONTRACTORS			1024

#define WORKERS_TASK_QUEUE_ID		0
#define REAPER_IMMEDIATE_TASK_QUEUE_ID	1
#define REAPER_TASK_QUEUE_ID		2
#define NUM_THREADPOOL_QUEUES		3

/* STRUCT DEFINITIONS */
struct asyncio_threadpool_handle {
	unsigned long refcount;

	struct asyncio_threadpool_dispatch_info info;

	ASYNCIO_THREAD_T thread;		/* Thread (contractor or worker) executing this task. */
	struct worker_thread_info *worker_info;	/* Info on worker thread (if in a worker, used for cleanup). */
	int in_worker_thread;			/* Whether handle was pulled from worker task queue and is processed by worker thread. */
	int in_worker_queue;			/* Used to know if in worker task queue. Should never be 1 if in_worker_thread is 1. */

	ASYNCIO_COND_T finished_cond;
	int finished;				/* Whether the task has finished (set by thread that did pthread_join i.e. reaper, or worker who returned from function). */
	int completed_normally;			/* Whether the task finished normally (not cancelled). */
	int reached_cleanup;			/* Whether handle's thread reached cleanup handler. Used to prevent pthread_cancel after certain point. */

	/* Used for task queues */
	/* We need to be able to put handle in multiple queues */
	struct asyncio_threadpool_handle *prev[NUM_THREADPOOL_QUEUES];
	struct asyncio_threadpool_handle *next[NUM_THREADPOOL_QUEUES];
};

struct worker_thread_info {
	int running;
	ASYNCIO_THREAD_T thread;
};
/* END STRUCT DEFINITIONS */

/* PROTOTYPES */
static void unlock_threadpool_mtx_cleanup(void *arg);

static void notify_handle_finished(struct asyncio_threadpool_handle *handle);
static void notify_handle_thread_died(struct asyncio_threadpool_handle *handle);
static void notify_worker_died(struct worker_thread_info *worker_info);

static void contractor_cleanup(void *arg);
static void *contractor_thread(void *arg);

static void worker_handle_cleanup(void *arg);
static void *worker_thread(void *arg);

static void *manager_thread(void *arg);
static void *reaper_thread(void *arg);

static int push_worker_task_locked(struct asyncio_threadpool_handle *handle);
static int pull_worker_task(struct worker_thread_info *worker_info, struct asyncio_threadpool_handle **handlep, int *stopped);

static void push_reaper_task_locked(struct asyncio_threadpool_handle *handle);
static int pull_reaper_task(struct asyncio_threadpool_handle **handlep, int *stopped);

static int dispatch_contractor(struct asyncio_threadpool_handle *handle);
static int dispatch_worker(struct asyncio_threadpool_handle *handle);

static int init_threadpool_handle(struct asyncio_threadpool_handle *handle, const struct asyncio_threadpool_dispatch_info *task);
static void cleanup_threadpool_handle(struct asyncio_threadpool_handle *handle);

static void stop_manager_thread(void);
/* END PROTOTYPES */

/* GLOBALS */
static ASYNCIO_RWLOCK_T initialization_lock = ASYNCIO_RWLOCK_INITIALIZER;
static int threadpool_initialized = 0;
static unsigned long initialization_count = 0;

static ASYNCIO_MUTEX_T threadpool_mtx = ASYNCIO_MUTEX_INITIALIZER;
static int threadpool_stopped = 0;

static struct worker_thread_info worker_threads[MAX_WORKER_THREADS];
static ASYNCIO_COND_T workers_newtask_cond = ASYNCIO_COND_INITIALIZER;
static decl_queue(struct asyncio_threadpool_handle, workers_task_queue);

static unsigned long contractors_count = 0;

static ASYNCIO_THREAD_T manager_thread_threadt;
static ASYNCIO_COND_T respawn_cond = ASYNCIO_COND_INITIALIZER;

static ASYNCIO_THREAD_T reaper_thread_threadt;
static ASYNCIO_COND_T reaper_cond = ASYNCIO_COND_INITIALIZER;
static decl_queue(struct asyncio_threadpool_handle, reaper_immediate_task_queue); /* Handles that will terminate very soon */
static decl_queue(struct asyncio_threadpool_handle, reaper_task_queue); /* All existing handles for ultimate cleanup */
/* END GLOBALS */

/* NOTE: Whenever there is a threadpool_release_handle in this module, handle must also be removed from the reaper_task_queue.
 * This doesn't apply to handles still in the workers_task_queue because they are not yet put in the reaper task queue.
 * We do this in the notify_handle_finished, which basically ends the handle's lifecycle through the threadpool module. */

/* NOTE: The reaper's purpose is to "reap" dead threads, i.e. cancelled workers/contractors as well as completed contractors.
 * A handle that has been completed by a worker ends its lifecycle by the worker, and handles taken out of the worker task
 * queue are "unborn", they never had a thread so there is nothing to reap. Their lifecycle is ended by whoever took them
 * out of the queue (for example threadpool_cancel). */

static void unlock_threadpool_mtx_cleanup(void *arg)
{
	/* Used as a wrapper to match the pthread_cleanup_push prototype */
	(void)arg;
	ASYNCIO_DEBUG_ENTER(1 ARG("%p", arg));
	ASYNCIO_MUTEX_UNLOCK(&threadpool_mtx);
	ASYNCIO_DEBUG_RETURN(VOIDRET);
}

static void notify_handle_finished(struct asyncio_threadpool_handle *handle)
{
	/* This function is responsible for ending the handle life cycle through the threadpool module.
	 * A reference may still be held by users, but it stops existing as far as this module is concerned. */
	ASYNCIO_DEBUG_ENTER(1 ARG("%p", handle));

	if (ASYNCIO_MUTEX_LOCK(&threadpool_mtx) == 0) {
		queue_remove(&reaper_task_queue, handle);
		handle->finished = 1;

		ASYNCIO_COND_BROADCAST(&handle->finished_cond);
		ASYNCIO_MUTEX_UNLOCK(&threadpool_mtx);
	} else {
		ASYNCIO_ERROR("Failed to lock threadpool mtx.\n");
	}

	asyncio_threadpool_release_handle(handle);
	ASYNCIO_DEBUG_RETURN(VOIDRET);
}

static void notify_handle_thread_died(struct asyncio_threadpool_handle *handle)
{
	ASYNCIO_DEBUG_ENTER(1 ARG("%p", handle));

	if (ASYNCIO_MUTEX_LOCK(&threadpool_mtx) != 0) {
		ASYNCIO_ERROR("Failed to lock threadpool mtx.\n");
		ASYNCIO_DEBUG_RETURN(VOIDRET);
		return;
	}

	handle->reached_cleanup = 1;

	/* If threadpool stopped, don't add ourselves to reaper task queue anymore
	 * because reaper needs to know that the queue is frozen at that point. We
	 * will be reaped by iterating over the reaper (born threads) and worker (unborn) task queues. */
	/* This check is not strictly necessary because reaper ignores the immediate task queue
	 * after the threadpool is stopped anyway. */
	if (!threadpool_stopped) {
		push_reaper_task_locked(handle);

		/* Do not notify handle finished, we maintain the invariant
		 * that notify_handle_finished is called only after the handle's
		 * thread has actually been joined (or there was no thread
		 * to begin with). As a result, this handle will slip by
		 * the immediate reaper queue, and will exist as a "zombie"
		 * until the final cleanup where the reaper will end its lifecycle
		 * while iterating over the reaper task queue.
		 *
		 * We're just settling for a delayed release of ressources in the
		 * very unlikely event that the push function above returns an error,
		 * which is really not even supposed to happen anyway... */
	}

	ASYNCIO_MUTEX_UNLOCK(&threadpool_mtx);
	ASYNCIO_DEBUG_RETURN(VOIDRET);
}

static void notify_worker_died(struct worker_thread_info *worker_info)
{
	ASYNCIO_DEBUG_ENTER(1 ARG("%p", worker_info));

	if (ASYNCIO_MUTEX_LOCK(&threadpool_mtx) == 0) {
		/* We get here only if worker was working on a handle and got cancelled.
		 * Then notify_handle_thread_died gets called in the handle cleanup,
		 * which wakes the reaper and the reaper joins on the thread. The reaper
		 * then lets the manager know one of his workers has died so he can replace them.
		 */
		worker_info->running = 0;
		ASYNCIO_COND_SIGNAL(&respawn_cond);
		ASYNCIO_MUTEX_UNLOCK(&threadpool_mtx);
	} else {
		ASYNCIO_ERROR("Failed to lock threadpool mtx.\n");
	}

	ASYNCIO_DEBUG_RETURN(VOIDRET);
}

static void contractor_cleanup(void *arg)
{
	struct asyncio_threadpool_handle *handle;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", arg));

	/* handle was acquired in contractor_thread */
	handle = arg;

	/* Decrement contractors count */
	if (ASYNCIO_MUTEX_LOCK(&threadpool_mtx) != 0) {
		ASYNCIO_ERROR("Failed to lock threadpool mtx.\n");
		ASYNCIO_DEBUG_RETURN(VOIDRET);
		return;
	}

	if (contractors_count == 0) {
		ASYNCIO_ERROR("contractor count already 0.\n");
		ASYNCIO_MUTEX_UNLOCK(&threadpool_mtx);
		ASYNCIO_DEBUG_RETURN(VOIDRET);
		return;
	}

	--contractors_count;
	ASYNCIO_MUTEX_UNLOCK(&threadpool_mtx);

	/* Do not release handle, it will be done by the reaper thread. */
	notify_handle_thread_died(handle);

	ASYNCIO_DEBUG_RETURN(VOIDRET);
}

static void *contractor_thread(void *arg)
{
	struct asyncio_threadpool_handle *handle;
	int oldstate, oldstate1;
	int oldtype;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", arg));
	ASYNCIO_DISABLE_CANCELLATIONS(&oldstate);

	handle = arg;
	ASYNCIO_CLEANUP_PUSH(contractor_cleanup, handle);

	if (handle->info.flags & ASYNCIO_THREADPOOL_FLAG_ASYNCCANCEL)
		ASYNCIO_SET_CANCELTYPE(ASYNCIO_CANCEL_ASYNCHRONOUS, &oldtype);
	else
		ASYNCIO_SET_CANCELTYPE(ASYNCIO_CANCEL_DEFERRED, &oldtype);

	if (handle->info.flags & ASYNCIO_THREADPOOL_FLAG_CANCELLABLE)
		ASYNCIO_SET_CANCELSTATE(ASYNCIO_CANCEL_ENABLE, &oldstate1);
	else
		ASYNCIO_SET_CANCELSTATE(ASYNCIO_CANCEL_DISABLE, &oldstate1);

	/* Execute the dispatched function */
	handle->info.dispatch_info.fn(handle->info.dispatch_info.arg);

	ASYNCIO_DISABLE_CANCELLATIONS(&oldstate1);
	ASYNCIO_RESTORE_CANCELTYPE(oldtype);

	handle->completed_normally = 1;

	/* Pop and execute cleanup handler */
	ASYNCIO_CLEANUP_POP(1);

	ASYNCIO_RESTORE_CANCELSTATE(oldstate);
	ASYNCIO_DEBUG_RETURN(RET("%p", NULL));
	return NULL;
}

static void worker_handle_cleanup(void *arg)
{
	struct asyncio_threadpool_handle *handle;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", arg));

	/* handle was acquired in worker_thread */
	handle = arg;

	/* Do not release handle, it will be done by the reaper thread. */
	notify_handle_thread_died(handle);

	ASYNCIO_DEBUG_RETURN(VOIDRET);
}

static void *worker_thread(void *arg)
{
	struct worker_thread_info *worker_info;
	struct asyncio_threadpool_handle *handle;
	int oldstate, oldstate1;
	int oldtype;
	int stopped = 0;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", arg));
	ASYNCIO_DISABLE_CANCELLATIONS(&oldstate);

	worker_info = arg;

	for (;;) {
		if (pull_worker_task(worker_info, &handle, &stopped) != 0) {
			ASYNCIO_ERROR("Failed to pull worker task.\n");
			ASYNCIO_DEBUG_CALL(2 FUNC(usleep) ARG("%d", 10000));
			usleep(10000);
			continue;
		}

		if (stopped)
			break;

		ASYNCIO_CLEANUP_PUSH(worker_handle_cleanup, handle);

		if (handle->info.flags & ASYNCIO_THREADPOOL_FLAG_ASYNCCANCEL)
			ASYNCIO_SET_CANCELTYPE(ASYNCIO_CANCEL_ASYNCHRONOUS, &oldtype);
		else
			ASYNCIO_SET_CANCELTYPE(ASYNCIO_CANCEL_DEFERRED, &oldtype);

		if (handle->info.flags & ASYNCIO_THREADPOOL_FLAG_CANCELLABLE)
			ASYNCIO_SET_CANCELSTATE(ASYNCIO_CANCEL_ENABLE, &oldstate1);
		else
			ASYNCIO_SET_CANCELSTATE(ASYNCIO_CANCEL_DISABLE, &oldstate1);

		/* Execute dispatched function */
		handle->info.dispatch_info.fn(handle->info.dispatch_info.arg);

		ASYNCIO_DISABLE_CANCELLATIONS(&oldstate1);
		ASYNCIO_RESTORE_CANCELTYPE(oldtype);

		/* Really this is unnecessary because it won't go to the reaper. */
		handle->completed_normally = 1;

		/* Call completed callback */
		if (handle->info.completed_info.cb != NULL)
			handle->info.completed_info.cb(handle->info.completed_info.arg);

		notify_handle_finished(handle);

		/* Pop but do not execute cleanup handler (we notify handle finished, not thread died). */
		ASYNCIO_CLEANUP_POP(0);
	}

	/* We should only get here if we stopped, so no notify. */
	ASYNCIO_RESTORE_CANCELSTATE(oldstate);

	ASYNCIO_DEBUG_RETURN(RET("%p", NULL));
	return NULL;
}

static void *manager_thread(void *arg)
{
	struct worker_thread_info *worker_info;
	ASYNCIO_THREAD_T thread;
	size_t i;
	(void)arg;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", arg));

	/* Initialize worker threads */
	for (i = 0; i < MAX_WORKER_THREADS; i++) {
		worker_info = &worker_threads[i];
		worker_info->running = 0;

		if (ASYNCIO_THREAD_CREATE(&thread, worker_thread, worker_info) != 0) {
			ASYNCIO_ERROR("Failed to create worker thread.\n");
			continue;
		}

		worker_info->thread = thread;
		worker_info->running = 1;
	}

	if (ASYNCIO_MUTEX_LOCK(&threadpool_mtx) != 0) {
		/* What are we supposed to do? */
		ASYNCIO_ERROR("Failed to lock threadpool mtx.\n");
		ASYNCIO_DEBUG_RETURN(RET("%p", NULL));
		return NULL;
	}

	/* Remain idle until some workers are dead and respawn them, or until we stopped. */
	while (!threadpool_stopped) {
		if (ASYNCIO_COND_WAIT(&respawn_cond, &threadpool_mtx) != 0) {
			ASYNCIO_MUTEX_UNLOCK(&threadpool_mtx);
			ASYNCIO_DEBUG_RETURN(RET("%p", NULL));
			return NULL;
		}

		/* Respawn worker threads */
		for (i = 0; i < MAX_WORKER_THREADS; i++) {
			worker_info = &worker_threads[i];

			if (!(worker_info->running)) {
				if (ASYNCIO_THREAD_CREATE(&thread, worker_thread, worker_info) != 0) {
					ASYNCIO_ERROR("Failed to create worker thread.\n");
					continue;
				}

				worker_info->thread = thread;
				worker_info->running = 1;
			}
		}
	}

	ASYNCIO_MUTEX_UNLOCK(&threadpool_mtx);
	ASYNCIO_DEBUG_RETURN(RET("%p", NULL));
	return NULL;
}

static void *reaper_thread(void *arg)
{
	struct worker_thread_info *worker_info;
	struct asyncio_threadpool_handle *handle;
	int stopped;
	size_t i;
	(void)arg;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", arg));

	for (;;) {
		if (pull_reaper_task(&handle, &stopped) != 0) {
			ASYNCIO_ERROR("Failed to pull reaper task.\n");
			ASYNCIO_DEBUG_CALL(2 FUNC(usleep) ARG("%d", 10000));
			usleep(10000);
			continue;
		}

		if (stopped)
			break;

		ASYNCIO_THREAD_JOIN(handle->thread);

		/* No need for mutex, these only get touched when the handle is dispatched to a worker. */
		if (handle->in_worker_thread && handle->worker_info != NULL) {
			notify_worker_died(handle->worker_info); /* Tell manager to respawn worker */
			handle->in_worker_thread = 0;
			handle->worker_info = NULL;
		}

		/* That would be a contractor thread ending, since worker completing normally doesn't die
		 * the thread would keep working on other handles. */
		if (handle->completed_normally) {
			/* Call completed callback */
			if (handle->info.completed_info.cb != NULL)
				handle->info.completed_info.cb(handle->info.completed_info.arg);
		} else {
			/* Call cancelled callback */
			if (handle->info.cancelled_info.cb != NULL)
				handle->info.cancelled_info.cb(handle->info.cancelled_info.arg);
		}

		notify_handle_finished(handle);
	}

	/* Join on all tasks in the reaper queue. We don't need mutex anymore since threadpool module has stopped, i.e. no more threads can be added. */
	/* The reaper task queue only contains handles associated to a thread. The handles waiting in worker task queue aren't here. */
	while (!queue_empty(&reaper_task_queue)) {
		queue_pop(&reaper_task_queue, &handle);
		ASYNCIO_THREAD_JOIN(handle->thread);

		/* So that we don't double-join a worker in the next loop */
		if (handle->in_worker_thread && handle->worker_info != NULL)
			handle->worker_info->running = 0;

		if (handle->completed_normally) {
			/* Call completed callback */
			if (handle->info.completed_info.cb != NULL)
				handle->info.completed_info.cb(handle->info.completed_info.arg);
		} else {
			/* Call cancelled callback */
			if (handle->info.cancelled_info.cb != NULL)
				handle->info.cancelled_info.cb(handle->info.cancelled_info.arg);
		}

		notify_handle_finished(handle);
	}

	/* The worker threads that stopped but didn't have a handle to work on do not update their running fields, it will still be 1. */
	for (i = 0; i < MAX_WORKER_THREADS; i++) {
		worker_info = &worker_threads[i];

		if (worker_info->running) {
			ASYNCIO_THREAD_JOIN(worker_info->thread);
			worker_info->running = 0;
		}
	}

	/* Release all handles in the workers task queue */
	while (!queue_empty(&workers_task_queue)) {
		queue_pop(&workers_task_queue, &handle);

		/* Call cancelled callback */
		if (handle->info.cancelled_info.cb != NULL)
			handle->info.cancelled_info.cb(handle->info.cancelled_info.arg);

		notify_handle_finished(handle);
	}

	ASYNCIO_DEBUG_RETURN(RET("%p", NULL));
	return NULL;
}

static int push_worker_task_locked(struct asyncio_threadpool_handle *handle)
{
	ASYNCIO_DEBUG_ENTER(1 ARG("%p", handle));

	if (handle->in_worker_thread) {
		ASYNCIO_ERROR("Handle cannot be both in worker task queue and in worker thread.\n");
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	/* Push new task into worker task queue */
	queue_push(&workers_task_queue, handle);

	/* Wake up a worker */
	ASYNCIO_COND_SIGNAL(&workers_newtask_cond);

	handle->in_worker_queue = 1;
	ASYNCIO_DEBUG_RETURN(RET("%d", 0));
	return 0;
}

static int pull_worker_task(struct worker_thread_info *worker_info, struct asyncio_threadpool_handle **handlep, int *stopped)
{
	struct asyncio_threadpool_handle *handle;

	ASYNCIO_DEBUG_ENTER(3 ARG("%p", worker_info) ARG("%p", handlep) ARG("%p", stopped));

	if (ASYNCIO_MUTEX_LOCK(&threadpool_mtx) != 0) {
		ASYNCIO_ERROR("Failed to lock threadpool mtx.\n");
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	while (queue_empty(&workers_task_queue) && !threadpool_stopped) {
		if (ASYNCIO_COND_WAIT(&workers_newtask_cond, &threadpool_mtx) != 0) {
			ASYNCIO_MUTEX_UNLOCK(&threadpool_mtx);
			ASYNCIO_DEBUG_RETURN(RET("%d", -1));
			return -1;
		}
	}

	if (threadpool_stopped) {
		ASYNCIO_MUTEX_UNLOCK(&threadpool_mtx);
		*stopped = 1;
		ASYNCIO_DEBUG_RETURN(RET("%d", 0));
		return 0;
	}

	/* Pop new task from queue (this is a misnomer... it's not pop-ing the task that was pushed like a stack) */
	queue_pop(&workers_task_queue, &handle);
	handle->thread = worker_info->thread;
	handle->worker_info = worker_info;
	handle->in_worker_thread = 1;
	handle->in_worker_queue = 0;

	queue_push(&reaper_task_queue, handle);
	ASYNCIO_MUTEX_UNLOCK(&threadpool_mtx);

	*handlep = handle;
	*stopped = 0;
	ASYNCIO_DEBUG_RETURN(RET("%d", 0));
	return 0;
}

static void push_reaper_task_locked(struct asyncio_threadpool_handle *handle)
{
	ASYNCIO_DEBUG_ENTER(1 ARG("%p", handle));

	/* Push new task into reaper immediate task queue */
	queue_push(&reaper_immediate_task_queue, handle);

	/* Wake up reaper */
	ASYNCIO_COND_SIGNAL(&reaper_cond);

	ASYNCIO_DEBUG_RETURN(VOIDRET);
}

static int pull_reaper_task(struct asyncio_threadpool_handle **handlep, int *stopped)
{
	struct asyncio_threadpool_handle *handle;

	ASYNCIO_DEBUG_ENTER(2 ARG("%p", handlep) ARG("%p", stopped));

	if (ASYNCIO_MUTEX_LOCK(&threadpool_mtx) != 0) {
		ASYNCIO_ERROR("Failed to lock threadpool mtx.\n");
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	while (queue_empty(&reaper_immediate_task_queue) && !threadpool_stopped) {
		if (ASYNCIO_COND_WAIT(&reaper_cond, &threadpool_mtx) != 0) {
			ASYNCIO_MUTEX_UNLOCK(&threadpool_mtx);
			ASYNCIO_DEBUG_RETURN(RET("%d", -1));
			return -1;
		}
	}

	if (threadpool_stopped) {
		ASYNCIO_MUTEX_UNLOCK(&threadpool_mtx);
		*stopped = 1;
		ASYNCIO_DEBUG_RETURN(RET("%d", 0));
		return 0;
	}

	/* Pop new task from queue (this is a misnomer... it's not pop-ing the task that was pushed like a stack) */
	queue_pop(&reaper_immediate_task_queue, &handle);
	ASYNCIO_MUTEX_UNLOCK(&threadpool_mtx);

	*handlep = handle;
	*stopped = 0;
	ASYNCIO_DEBUG_RETURN(RET("%d", 0));
	return 0;
}

static int init_threadpool_handle(struct asyncio_threadpool_handle *handle, const struct asyncio_threadpool_dispatch_info *task)
{
	ASYNCIO_DEBUG_ENTER(2 ARG("%p", handle) ARG("%p", task));
	handle->info = *task;
	handle->refcount = 0;
	handle->finished = 0;
	handle->reached_cleanup = 0;
	handle->completed_normally = 0;
	handle->worker_info = NULL;
	handle->in_worker_thread = 0;
	handle->in_worker_queue = 0;

	if (ASYNCIO_COND_INIT(&handle->finished_cond) != 0) {
		ASYNCIO_ERROR("Failed to initialize finished_cond.\n");
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	ASYNCIO_DEBUG_RETURN(RET("%d", 0));
	return 0;
}

static void cleanup_threadpool_handle(struct asyncio_threadpool_handle *handle)
{
	ASYNCIO_DEBUG_ENTER(1 ARG("%p", handle));
	ASYNCIO_COND_DESTROY(&handle->finished_cond);
	ASYNCIO_DEBUG_RETURN(VOIDRET);
}

static int dispatch_contractor(struct asyncio_threadpool_handle *handle)
{
	ASYNCIO_THREAD_T thread;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", handle));

	if (ASYNCIO_MUTEX_LOCK(&threadpool_mtx) != 0) {
		ASYNCIO_ERROR("Failed to lock threadpool mtx.\n");
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	if (contractors_count < MAX_CONTRACTORS) {
		if (ASYNCIO_THREAD_CREATE(&thread, contractor_thread, handle) == 0) {
			handle->thread = thread;
			++contractors_count;
			queue_push(&reaper_task_queue, handle);
			ASYNCIO_MUTEX_UNLOCK(&threadpool_mtx);

			ASYNCIO_DEBUG_RETURN(RET("%d", 0));
			return 0;
		} else {
			ASYNCIO_ERROR("Failed to create thread.\n");
		}
	} else {
		ASYNCIO_ERROR("Max contractors count limit reached.\n");
	}

	ASYNCIO_MUTEX_UNLOCK(&threadpool_mtx);

	ASYNCIO_DEBUG_RETURN(RET("%d", -1));
	return -1;
}

static int dispatch_worker(struct asyncio_threadpool_handle *handle)
{
	ASYNCIO_DEBUG_ENTER(1 ARG("%p", handle));

	if (ASYNCIO_MUTEX_LOCK(&threadpool_mtx) != 0) {
		ASYNCIO_ERROR("Failed to lock threadpool mtx.\n");
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	if (push_worker_task_locked(handle) != 0) {
		ASYNCIO_ERROR("Failed to push worker task.\n");
		ASYNCIO_MUTEX_UNLOCK(&threadpool_mtx);
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	ASYNCIO_MUTEX_UNLOCK(&threadpool_mtx);
	ASYNCIO_DEBUG_RETURN(RET("%d", 0));
	return 0;
}

static void stop_manager_thread()
{
	ASYNCIO_DEBUG_ENTER(VOIDARG);

	if (ASYNCIO_MUTEX_LOCK(&threadpool_mtx) != 0) {
		ASYNCIO_ERROR("Failed to lock threadpool mtx.\n");
		ASYNCIO_DEBUG_RETURN(VOIDRET);
		return;
	}

	threadpool_stopped = 1;

	/* Wake up manager */
	ASYNCIO_COND_SIGNAL(&respawn_cond);

	ASYNCIO_MUTEX_UNLOCK(&threadpool_mtx);

	/* Join manager thread */
	ASYNCIO_THREAD_JOIN(manager_thread_threadt);

	ASYNCIO_DEBUG_RETURN(VOIDRET);
}

int asyncio_threadpool_init()
{
	int oldstate;
	int rc;

	ASYNCIO_DEBUG_ENTER(VOIDARG);
	ASYNCIO_DISABLE_CANCELLATIONS(&oldstate);

	/* Used to avoid race condition if one thread calls threadpool_init while
	 * another thread is currently in threadpool_cleanup (joining on the reaper, for example).
	 * The cleanup doesn't hold the threadpool_mtx when joining because it would prevent the
	 * workers in acquiring the mutex, thus resulting in deadlock. */
	/* The wrlocks are used by init and cleanup because they will modify the threadpool_initialized state.
	 * The rdlocks are used by dispatch and cancel to check on the initialization status and prevent init/cleanup
	 * from occuring while they are still doing stuff. */
	if (ASYNCIO_RWLOCK_WRLOCK(&initialization_lock) != 0) {
		ASYNCIO_ERROR("Failed to wrlock initialization lock.\n");
		rc = -1;
		goto return_cancelstate;
	}

	if (initialization_count == ULONG_MAX) {
		ASYNCIO_ERROR("Reached maximal threadpool initialization count.\n");
		rc = -1;
		goto return_initialization_lock;
	}

	++initialization_count;

	if (threadpool_initialized) {
		rc = 0;
		goto return_initialization_lock;
	}

	queue_init(&workers_task_queue, WORKERS_TASK_QUEUE_ID);
	queue_init(&reaper_immediate_task_queue, REAPER_IMMEDIATE_TASK_QUEUE_ID);
	queue_init(&reaper_task_queue, REAPER_TASK_QUEUE_ID);
	contractors_count = 0;

	threadpool_stopped = 0;

	if (ASYNCIO_THREAD_CREATE(&manager_thread_threadt, manager_thread, NULL) != 0) {
		ASYNCIO_ERROR("Failed to create manager thread.\n");
		rc = -1;
		goto return_initialization_count;
	}

	if (ASYNCIO_THREAD_CREATE(&reaper_thread_threadt, reaper_thread, NULL) != 0) {
		ASYNCIO_ERROR("Failed to create reaper thread.\n");
		rc = -1;
		goto return_manager_thread;
	}

	threadpool_initialized = 1;

	rc = 0;
	goto return_initialization_lock;

return_manager_thread:
	stop_manager_thread();

return_initialization_count:
	--initialization_count;

return_initialization_lock:
	ASYNCIO_RWLOCK_UNLOCK(&initialization_lock);

return_cancelstate:
	ASYNCIO_RESTORE_CANCELSTATE(oldstate);
	ASYNCIO_DEBUG_RETURN(RET("%d", rc));
	return rc;
}

int asyncio_threadpool_dispatch(const struct asyncio_threadpool_dispatch_info *task, struct asyncio_threadpool_handle **handlep)
{
	struct asyncio_threadpool_handle *handle;
	int oldstate;
	int rc;

	ASYNCIO_DEBUG_ENTER(2 ARG("%p", task) ARG("%p", handlep));
	ASYNCIO_DISABLE_CANCELLATIONS(&oldstate);

	if (ASYNCIO_RWLOCK_RDLOCK(&initialization_lock) != 0) {
		ASYNCIO_ERROR("Failed to rdlock initialization lock.\n");
		rc = -1;
		goto return_cancelstate;
	}

	if (!threadpool_initialized) {
		rc = -1;
		goto return_initialization_lock;
	}

	if (task->dispatch_info.fn == NULL) {
		rc = -1;
		goto return_initialization_lock;
	}

	handle = asyncio_safe_malloc(1, sizeof *handle);

	if (handle == NULL) {
		ASYNCIO_ERROR("safe_malloc failed.\n");
		rc = -1;
		goto return_initialization_lock;
	}

	if (init_threadpool_handle(handle, task) != 0) {
		rc = -1;
		goto return_free_handle;
	}

	/* The caller must have a reference by default to prevent race conditions
	 * where the task completes before the client had a chance to acquire the
	 * handle. Also the contractor or worker thread must have a reference to
	 * prevent the case where the client releases its handle before the worker
	 * or contractor manages to acquire its handle */
	handle->refcount = 2;

	/* Creating contractor threads seems to be much slower than dispatching to
	 * the fixed number of worker threads, so only use them for tasks that are gonna
	 * take a long time, to avoid slowing down batch-dispatching. */
	if (task->flags & ASYNCIO_THREADPOOL_FLAG_CONTRACTOR) {
		/* Give preference to contractor threads */
		if (dispatch_contractor(handle) == 0) {
			*handlep = handle;
			rc = 0;
			goto return_initialization_lock;
		}

		/* Try with worker thread instead */
		if (dispatch_worker(handle) == 0) {
			*handlep = handle;
			rc = 0;
			goto return_initialization_lock;
		}
	} else {
		/* Give preference to worker threads */
		if (dispatch_worker(handle) == 0) {
			*handlep = handle;
			rc = 0;
			goto return_initialization_lock;
		}

		/* Try with contractor thread instead */
		if (dispatch_contractor(handle) == 0) {
			*handlep = handle;
			rc = 0;
			goto return_initialization_lock;
		}
	}

	cleanup_threadpool_handle(handle);
	rc = -1;

return_free_handle:
	asyncio_safe_free(handle);

return_initialization_lock:
	ASYNCIO_RWLOCK_UNLOCK(&initialization_lock);

return_cancelstate:
	ASYNCIO_RESTORE_CANCELSTATE(oldstate);
	ASYNCIO_DEBUG_RETURN(RET("%d", rc));
	return rc;
}

int asyncio_threadpool_cancel(struct asyncio_threadpool_handle *handle)
{
	int oldstate;
	int rc;
	int success, removed_from_queue;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", thandle));
	ASYNCIO_DISABLE_CANCELLATIONS(&oldstate);

	if (ASYNCIO_RWLOCK_RDLOCK(&initialization_lock) != 0) {
		ASYNCIO_ERROR("Failed to rdlock initialization lock.\n");
		rc = -1;
		goto return_cancelstate;
	}

	/* Only cancel cancellable threads */
	if (!(handle->info.flags & ASYNCIO_THREADPOOL_FLAG_CANCELLABLE)) {
		rc = -1;
		goto return_initialization_lock;
	}

	if (ASYNCIO_MUTEX_LOCK(&threadpool_mtx) != 0) {
		ASYNCIO_ERROR("Failed to lock threadpool mtx.\n");
		rc = -1;
		goto return_initialization_lock;
	}

	success = 0;
	removed_from_queue = 0;

	if (handle->reached_cleanup) {
		/* If it already reached cleanup, we're done. */
		success = 1;
	} else if (handle->in_worker_queue) {
		/* Remove handle from worker queue */
		queue_remove(&workers_task_queue, handle);
		handle->in_worker_queue = 0;
		removed_from_queue = 1;
		success = 1;
	} else {
		/* Two possibilities: it was pulled out of worker queue after which it is guaranteed to be in a worker thread,
		 * or it is in contractor thread. It can't be nowhere because that happens only if dispatch failed, but then
		 * we would never have reached this point. In both possible cases, we just cancel the thread.
		 *
		 * Also, it's impossible for the thread to be invalid here. For that to be the case, one must have called
		 * pthread_join on it and returned. But a return from pthread_join must mean the thread has terminated, i.e.
		 * its cleanup handlers must have returned as well. But the handle->reached_cleanup check above makes sure
		 * that it hasn't reached cleanup yet, and we're locking so this blocks the cleanup handler until we cancelled. */
		if (ASYNCIO_THREAD_CANCEL(handle->thread) == 0)
			success = 1;
		else
			ASYNCIO_ERROR("Failed to cancel handle thread.\n");
	}

	ASYNCIO_MUTEX_UNLOCK(&threadpool_mtx);

	if (removed_from_queue) {
		/* Call cancelled callback. */
		if (handle->info.cancelled_info.cb != NULL)
			handle->info.cancelled_info.cb(handle->info.cancelled_info.arg);

		/* There's no pthread_join that can return, it's just taken out of queue. */
		notify_handle_finished(handle);
	}

	if (success)
		rc = 0;
	else
		rc = -1;

return_initialization_lock:
	ASYNCIO_RWLOCK_UNLOCK(&initialization_lock);

return_cancelstate:
	ASYNCIO_RESTORE_CANCELSTATE(oldstate);
	ASYNCIO_DEBUG_RETURN(RET("%d", rc));
	return rc;
}

int asyncio_threadpool_join(struct asyncio_threadpool_handle *handle)
{
	int oldstate;
	int oldtype;
	int success = 1;
	int rc;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", thandle));
	ASYNCIO_DISABLE_CANCELLATIONS(&oldstate);

	if (ASYNCIO_MUTEX_LOCK(&threadpool_mtx) != 0) {
		ASYNCIO_ERROR("Failed to lock threadpool mtx.\n");
		rc = -1;
		goto return_cancelstate;
	}

	/* Unlock the threadpool_mtx in cleanup handler if cancelled here */
	ASYNCIO_CLEANUP_PUSH(unlock_threadpool_mtx_cleanup, NULL);

	/* Restore cancelstate while waiting for condition variable
	 * to allow cancellation in this case. But set cancellation type to DEFERRED
	 * in order to make sure we cancel during pthread_cond_wait, which should guarantee
	 * that the mtx is locked during the cleanup handler. */
	ASYNCIO_SET_CANCELTYPE(ASYNCIO_CANCEL_DEFERRED, &oldtype);
	ASYNCIO_RESTORE_CANCELSTATE(oldstate);

	while (!(handle->finished)) {
		if (ASYNCIO_COND_WAIT(&handle->finished_cond, &threadpool_mtx) != 0) {
			ASYNCIO_ERROR("Failed to wait on handle finished_cond.\n");
			success = 0;
			break;
		}
	}

	ASYNCIO_DISABLE_CANCELLATIONS(&oldstate);
	ASYNCIO_RESTORE_CANCELTYPE(oldtype);

	/* Unlock the threadpool mtx. */
	ASYNCIO_CLEANUP_POP(1);

	if (success)
		rc = 0;
	else
		rc = -1;

return_cancelstate:
	ASYNCIO_RESTORE_CANCELSTATE(oldstate);
	ASYNCIO_DEBUG_RETURN(RET("%d", rc));
	return rc;
}

int asyncio_threadpool_acquire_handle(struct asyncio_threadpool_handle *handle)
{
	int oldstate;
	int rc;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", thandle));
	ASYNCIO_DISABLE_CANCELLATIONS(&oldstate);

	if (ASYNCIO_MUTEX_LOCK(&threadpool_mtx) != 0) {
		ASYNCIO_ERROR("Failed to lock threadpool mtx.\n");
		rc = -1;
		goto return_cancelstate;
	}

	/* Check for overflow */
	if (handle->refcount >= ULONG_MAX) {
		ASYNCIO_ERROR("handle refcount overflow.\n");
		rc = -1;
		goto return_threadpool_mtx;
	}

	++(handle->refcount);
	rc = 0;

return_threadpool_mtx:
	ASYNCIO_MUTEX_UNLOCK(&threadpool_mtx);

return_cancelstate:
	ASYNCIO_RESTORE_CANCELSTATE(oldstate);
	ASYNCIO_DEBUG_RETURN(RET("%d", rc));
	return rc;
}

void asyncio_threadpool_release_handle(struct asyncio_threadpool_handle *handle)
{
	int oldstate;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", thandle));
	ASYNCIO_DISABLE_CANCELLATIONS(&oldstate);

	if (ASYNCIO_MUTEX_LOCK(&threadpool_mtx) != 0) {
		ASYNCIO_ERROR("Failed to lock threadpool mtx.\n");
		goto return_cancelstate;
	}

	/* Check for underflow */
	if (handle->refcount == 0) {
		ASYNCIO_ERROR("Handle refcount already 0 before release.\n");
		goto return_threadpool_mtx;
	}

	--(handle->refcount);

	if (handle->refcount == 0) {
		ASYNCIO_MUTEX_UNLOCK(&threadpool_mtx);
		cleanup_threadpool_handle(handle); /* Don't wanna do this while holding mtx */
		asyncio_safe_free(handle);
		goto return_cancelstate;
	}

return_threadpool_mtx:
	ASYNCIO_MUTEX_UNLOCK(&threadpool_mtx);

return_cancelstate:
	ASYNCIO_RESTORE_CANCELSTATE(oldstate);
	ASYNCIO_DEBUG_RETURN(VOIDRET);
}

void asyncio_threadpool_cleanup(void)
{
	int oldstate;

	ASYNCIO_DEBUG_ENTER(VOIDARG);
	ASYNCIO_DISABLE_CANCELLATIONS(&oldstate);

	/* Initialization lock used to signal the threadpool_initialized flag. */
	if (ASYNCIO_RWLOCK_WRLOCK(&initialization_lock) != 0) {
		ASYNCIO_ERROR("Failed to wrlock initialization lock.\n");
		goto return_cancelstate;
	}

	if (!threadpool_initialized)
		goto return_initialization_lock;

	if (initialization_count == 0) {
		ASYNCIO_ERROR("Threadpool initialization count already 0, should not happen.\n");
		goto return_initialization_lock;
	}

	--initialization_count;

	if (initialization_count > 0)
		goto return_initialization_lock;

	if (ASYNCIO_MUTEX_LOCK(&threadpool_mtx) != 0) {
		ASYNCIO_ERROR("Failed to lock threadpool mtx.\n");
		goto return_initialization_lock;
	}

	threadpool_stopped = 1;
	threadpool_initialized = 0;

	/* Wake up all workers with broadcast */
	ASYNCIO_COND_BROADCAST(&workers_newtask_cond);

	/* Wake up manager */
	ASYNCIO_COND_SIGNAL(&respawn_cond);

	/* Wake up reaper */
	ASYNCIO_COND_SIGNAL(&reaper_cond);

	/* Threadpool mtx used to signal the threapool_stopped flag. */
	ASYNCIO_MUTEX_UNLOCK(&threadpool_mtx);

	/* Join manager thread */
	ASYNCIO_THREAD_JOIN(manager_thread_threadt);

	/* Join reaper thread */
	ASYNCIO_THREAD_JOIN(reaper_thread_threadt);

return_initialization_lock:
	ASYNCIO_RWLOCK_UNLOCK(&initialization_lock);

return_cancelstate:
	ASYNCIO_RESTORE_CANCELSTATE(oldstate);
	ASYNCIO_DEBUG_RETURN(VOIDRET);
}
