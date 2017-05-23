#include <stdlib.h>
#include <stdint.h>

#include <unistd.h>
#include <pthread.h>
#include <signal.h>

#include <sys/errno.h>

#include "threadpool.h"
#include "cancellations.h"
#include "queue.h"
#include "safe_malloc.h"
#include "logging.h"

#define MAX_WORKER_THREADS		5
#define MAX_CONTRACTORS			1024

#define UINT64T_MAX			((uint64_t)(-1)) /* Get rid of compiler warning about 'use of C99 long long integer constant' for UINT64_MAX */

/* STRUCT DEFINITIONS */
struct threadpool_handle {
	uint64_t refcount;

	struct threadpool_dispatch_info info;

	pthread_t thread;			/* Thread (contractor or worker) executing this task. */
	int in_worker_thread;			/* Whether handle was pulled from worker task queue and is processed by worker thread. */
	int in_worker_queue;			/* Used to know if in worker task queue. Should never be 1 if in_worker_thread is 1. */

	pthread_cond_t finished_cond;
	int finished;				/* Whether the task has finished (set by thread that did pthread_join). */
	int need_to_join;			/* Whether we need to join on the pthread_t (with pthread_join). This is 0 if thread was taken out of worker queue or someone already did. */
	int reached_cleanup;			/* Whether handle's thread reached cleanup handler. Used to prevent pthread_cancel after certain point. */
	int completed_normally;			/* Handle's thread returned from dispatch function, did not get cancelled. */

	/* Used for task queues */
	struct threadpool_handle *prev;
	struct threadpool_handle *next;
};

struct worker_thread_info {
	int running;
	pthread_t thread;
};
/* END STRUCT DEFINITIONS */

/* PROTOTYPES */
static int lock_threadpool_mtx();
static void unlock_threadpool_mtx();
static void unlock_threadpool_mtx_cleanup(void *arg);

static int create_thread(pthread_t *thread, void *(*start_routine)(void *), void *arg);

static void notify_handle_finished(struct threadpool_handle *handle);

static void contractor_cleanup(void *arg);
static void *contractor_thread(void *arg);

static void worker_handle_cleanup(void *arg);
static void worker_cleanup_with_respawn(void *arg);
static void *worker_thread(void *arg);

static int stop_all_worker_tasks_locked(void);
static int push_worker_task_locked(struct threadpool_handle *handle);
static int pull_worker_task(struct worker_thread_info *worker_info, struct threadpool_handle **handlep, int *stopped);

static int dispatch_contractor(struct threadpool_handle *handle);
static int dispatch_worker(struct threadpool_handle *handle);

static int init_threadpool_handle(struct threadpool_handle *handle, struct threadpool_dispatch_info *task);
static void cleanup_threadpool_handle(struct threadpool_handle *handle);

static void init_worker_info(struct worker_thread_info *info);
/* END PROTOTYPES */

/* GLOBALS */
pthread_mutex_t threadpool_mtx = PTHREAD_MUTEX_INITIALIZER;

static struct worker_thread_info worker_threads[MAX_WORKER_THREADS];
pthread_cond_t workers_newtask_cond = PTHREAD_COND_INITIALIZER;
decl_queue(struct threadpool_handle, workers_task_queue);
static int workers_initialized = 0;

uint64_t contractors_count = 0;
/* END GLOBALS */

static int lock_threadpool_mtx()
{
	int rc;

	ASYNCIO_DEBUG_ENTER(VOIDARG);

	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_lock) ARG("%p", &threadpool_mtx));
	if ((rc = pthread_mutex_lock(&threadpool_mtx)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_mutex_lock");
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return - 1;
	}

	ASYNCIO_DEBUG_RETURN(RET("%d", 0));
	return 0;
}

static void unlock_threadpool_mtx()
{
	int rc;

	ASYNCIO_DEBUG_ENTER(VOIDARG);

	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_unlock) ARG("%p", &threadpool_mtx));
	if ((rc = pthread_mutex_unlock(&threadpool_mtx)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_mutex_unlock");
	}

	ASYNCIO_DEBUG_RETURN(VOIDRET);
}

static void unlock_threadpool_mtx_cleanup(void *arg)
{
	/* Used as a wrapper to match the pthread_cleanup_push prototype */
	(void)arg;
	unlock_threadpool_mtx();
}

static int create_thread(pthread_t *thread, void *(*start_routine)(void *), void *arg)
{
	int rc;

	ASYNCIO_DEBUG_ENTER(3 ARG("%p", thread) ARG("%p", start_routine) ARG("%p", arg));

	ASYNCIO_DEBUG_CALL(5 FUNC(pthread_create) ARG("%p", thread) ARG("%p", NULL) ARG("%p", start_routine) ARG("%p", arg));
	if ((rc = pthread_create(thread, NULL, start_routine, arg)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_create");
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	/* When pthreads are created there is some allocated ressources that remain even after thread terminates, unless the thread was
	 * joined or detached. This is what allows keeping the pthread_t valid after termination, for example for multiple pthread_cancels.
	 * But we want to release these ressources, so we detach the threads here, and we keep track of terminated threads with the finished
	 * flag in the threadpool handles. */
/*	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_detach) ARG("%016llx", *thread));
	if ((rc = pthread_detach(*thread)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_detach");
*/		/* Too bad, but we still return success because no point in killing thread for that... */
/*	}
*/

	ASYNCIO_DEBUG_RETURN(RET("%d", 0));
	return 0;
}

static void notify_handle_finished(struct threadpool_handle *handle)
{
	int rc;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", handle));

	if (lock_threadpool_mtx() == 0) {
		handle->finished = 1;

		ASYNCIO_DEBUG_CALL(2 FUNC(pthread_cond_broadcast) ARG("%p", &handle->finished_cond));
		if ((rc = pthread_cond_broadcast(&handle->finished_cond)) != 0) {
			errno = rc;
			ASYNCIO_SYSERROR("pthread_cond_broadcast");
		}

		unlock_threadpool_mtx();
	} else {
		ASYNCIO_ERROR("Failed to lock threadpool mtx.\n");
	}

	ASYNCIO_DEBUG_RETURN(VOIDRET);
}

static void contractor_cleanup(void *arg)
{
	struct threadpool_handle *handle;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", arg));

	/* handle was acquired in contractor_thread */
	handle = (struct threadpool_handle *)arg;

	/* Indicate that we reached cleanup handler.
	 * Do not pthread_cancel after this point. */
	if (lock_threadpool_mtx() != 0) {
		ASYNCIO_ERROR("Failed to lock threadpool mtx.\n");
		ASYNCIO_DEBUG_RETURN(VOIDRET);
		return;
	}

	handle->reached_cleanup = 1;
	unlock_threadpool_mtx();

	if (handle->completed_normally) {
		/* Call completed callback */
		if (handle->info.completed_info.cb != NULL)
			handle->info.completed_info.cb(handle->info.completed_info.arg);
	} else {
		/* Call cancelled callback */
		if (handle->info.cancelled_info.cb != NULL)
			handle->info.cancelled_info.cb(handle->info.cancelled_info.arg);
	}

	/* Release our handle */
	threadpool_release_handle(handle);

	/* Decrement contractors count */
	if (lock_threadpool_mtx() != 0) {
		ASYNCIO_ERROR("Failed to lock threadpool mtx.\n");
		ASYNCIO_DEBUG_RETURN(VOIDRET);
		return;
	}

	if (contractors_count == 0) {
		ASYNCIO_ERROR("contractor count already 0.\n");
		unlock_threadpool_mtx();
		ASYNCIO_DEBUG_RETURN(VOIDRET);
		return;
	}

	--contractors_count;
	unlock_threadpool_mtx();

	ASYNCIO_DEBUG_RETURN(VOIDRET);
}

static void *contractor_thread(void *arg)
{
	struct threadpool_handle *handle;
	int oldstate, oldstate1;
	int oldtype;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", arg));
	disable_cancellations(&oldstate);

	handle = (struct threadpool_handle *)arg;
	pthread_cleanup_push(contractor_cleanup, handle);

	if (handle->info.flags & THREADPOOL_FLAG_ASYNCCANCEL)
		set_canceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &oldtype);
	else
		set_canceltype(PTHREAD_CANCEL_DEFERRED, &oldtype);

	if (handle->info.flags & THREADPOOL_FLAG_CANCELLABLE)
		set_cancelstate(PTHREAD_CANCEL_ENABLE, &oldstate1);
	else
		set_cancelstate(PTHREAD_CANCEL_DISABLE, &oldstate1);

	/* Execute the dispatched function */
	handle->info.dispatch_info.fn(handle->info.dispatch_info.arg);

	disable_cancellations(&oldstate1);
	restore_canceltype(oldtype);

	handle->completed_normally = 1;

	/* Pop and execute cleanup handler */
	pthread_cleanup_pop(1);

	restore_cancelstate(oldstate);
	ASYNCIO_DEBUG_RETURN(RET("%p", NULL));
	return NULL;
}

static void worker_handle_cleanup(void *arg)
{
	struct threadpool_handle *handle;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", arg));

	/* handle was acquired in worker_thread */
	handle = (struct threadpool_handle *)arg;

	/* Indicate that we reached cleanup handler.
	 * Do not pthread_cancel after this point. */
	if (lock_threadpool_mtx() != 0) {
		ASYNCIO_ERROR("Failed to lock threadpool mtx.\n");
		ASYNCIO_DEBUG_RETURN(VOIDRET);
		return;
	}

	handle->reached_cleanup = 1;

	/* We only join worker thread with handle if it was cancelled,
	 * otherwise worker thread will keep going about its other tasks anyway. */
	if (handle->completed_normally)
		handle->need_to_join = 0;
	else
		handle->need_to_join = 1;

	unlock_threadpool_mtx();

	/* Need to notify handle finished because threads joining on worker threadpool handle
	 * don't pthread_join by default, so they must know we finished through the
	 * pthread_cond variable. They will then see that we set need_to_join again
	 * and one of them will do the actual pthread_join. */
	notify_handle_finished(handle);

	if (handle->completed_normally) {
		/* Call completed callback */
		if (handle->info.completed_info.cb != NULL)
			handle->info.completed_info.cb(handle->info.completed_info.arg);
	} else {
		/* Call cancelled callback */
		if (handle->info.cancelled_info.cb != NULL)
			handle->info.cancelled_info.cb(handle->info.cancelled_info.arg);
	}

	/* Release our handle */
	threadpool_release_handle(handle);

	ASYNCIO_DEBUG_RETURN(VOIDRET);
}

static void worker_cleanup_with_respawn(void *arg)
{
	struct worker_thread_info *worker_info;
	pthread_t thread;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", arg));

	if (lock_threadpool_mtx() != 0) {
		ASYNCIO_ERROR("Failed to lock threadpool mtx.\n");
		ASYNCIO_DEBUG_RETURN(VOIDRET);
		return;
	}

	worker_info = (struct worker_thread_info *)arg;
	worker_info->running = 0;

	if (create_thread(&thread, worker_thread, worker_info) == 0) {
		worker_info->running = 1;
		worker_info->thread = thread;
	} else {
		ASYNCIO_ERROR("Failed to create thread.\n");
	}

	unlock_threadpool_mtx();
	ASYNCIO_DEBUG_RETURN(VOIDRET);
}

static void *worker_thread(void *arg)
{
	struct worker_thread_info *worker_info;
	struct threadpool_handle *handle;
	int oldstate, oldstate1;
	int oldtype;
	int stopped = 0;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", arg));
	disable_cancellations(&oldstate);

	worker_info = (struct worker_thread_info *)arg;

	/* Respawn worker thread if we get cancelled */
	pthread_cleanup_push(worker_cleanup_with_respawn, worker_info);

	for (;;) {
		if (pull_worker_task(worker_info, &handle, &stopped) != 0) {
			ASYNCIO_ERROR("Failed to pull worker task.\n");
			ASYNCIO_DEBUG_CALL(2 FUNC(usleep) ARG("%d", 10000));
			usleep(10000);
			continue;
		}

		if (stopped)
			break;

		pthread_cleanup_push(worker_handle_cleanup, handle);

		if (handle->info.flags & THREADPOOL_FLAG_ASYNCCANCEL)
			set_canceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &oldtype);
		else
			set_canceltype(PTHREAD_CANCEL_DEFERRED, &oldtype);

		if (handle->info.flags & THREADPOOL_FLAG_CANCELLABLE)
			set_cancelstate(PTHREAD_CANCEL_ENABLE, &oldstate1);
		else
			set_cancelstate(PTHREAD_CANCEL_DISABLE, &oldstate1);

		/* Execute dispatched function */
		handle->info.dispatch_info.fn(handle->info.dispatch_info.arg);

		disable_cancellations(&oldstate1);
		restore_canceltype(oldtype);

		handle->completed_normally = 1;

		/* Pop and execute cleanup handler */
		pthread_cleanup_pop(1);
	}

	/* We should only get here if we stopped, so no respawn. */
	pthread_cleanup_pop(0);
	restore_cancelstate(oldstate);

	ASYNCIO_DEBUG_RETURN(RET("%p", NULL));
	return NULL;
}

static int stop_all_worker_tasks_locked(void)
{
	int rc;

	ASYNCIO_DEBUG_ENTER(VOIDARG);
	workers_initialized = 0;

	/* Wake up all workers with broadcast */
	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_cond_broadcast) ARG("%p", &workers_newtask_cond));
	if ((rc = pthread_cond_broadcast(&workers_newtask_cond)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_cond_broadcast");
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	ASYNCIO_DEBUG_RETURN(RET("%d", 0));
	return 0;
}

static int push_worker_task_locked(struct threadpool_handle *handle)
{
	int rc;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", handle));

	if (handle->in_worker_thread) {
		ASYNCIO_ERROR("Handle cannot be both in worker task queue and in worker thread.\n");
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	/* Push new task into worker task queue */
	queue_push(&workers_task_queue, handle);

	/* Wake up a worker */
	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_cond_signal) ARG("%p", &workers_newtask_cond));
	if ((rc = pthread_cond_signal(&workers_newtask_cond)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_cond_signal");

		queue_remove(&workers_task_queue, handle);
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	handle->in_worker_queue = 1;
	ASYNCIO_DEBUG_RETURN(RET("%d", 0));
	return 0;
}

static int pull_worker_task(struct worker_thread_info *worker_info, struct threadpool_handle **handlep, int *stopped)
{
	struct threadpool_handle *handle;
	int rc;

	ASYNCIO_DEBUG_ENTER(3 ARG("%p", worker_info) ARG("%p", handlep) ARG("%p", stopped));

	if (lock_threadpool_mtx() != 0) {
		ASYNCIO_ERROR("Failed to lock threadpool mtx.\n");
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	while (queue_empty(&workers_task_queue) && workers_initialized) {
		ASYNCIO_DEBUG_CALL(3 FUNC(pthread_cond_wait) ARG("%p", &workers_newtask_cond) ARG("%p", &threadpool_mtx));
		if ((rc = pthread_cond_wait(&workers_newtask_cond, &threadpool_mtx)) != 0) {
			errno = rc;
			ASYNCIO_SYSERROR("pthread_cond_wait");

			unlock_threadpool_mtx();
			ASYNCIO_DEBUG_RETURN(RET("%d", -1));
			return -1;
		}
	}

	if (!workers_initialized) {
		unlock_threadpool_mtx();
		*stopped = 1;
		ASYNCIO_DEBUG_RETURN(RET("%d", 0));
		return 0;
	}

	/* Pop new task from queue (this is a misnomer... it's not pop-ing the task that was pushed like a stack) */
	queue_pop(&workers_task_queue, &handle);
	handle->thread = worker_info->thread;
	handle->need_to_join = 0;	/* Don't pthread_join it, instead worker handle cleanup will notify handle finished if worker cancelled (else it will continue). */
	handle->in_worker_thread = 1;
	handle->in_worker_queue = 0;

	unlock_threadpool_mtx();

	*handlep = handle;
	*stopped = 0;
	ASYNCIO_DEBUG_RETURN(RET("%d", 0));
	return 0;
}

static int init_threadpool_handle(struct threadpool_handle *handle, struct threadpool_dispatch_info *task)
{
	int rc;

	ASYNCIO_DEBUG_ENTER(2 ARG("%p", handle) ARG("%p", task));
	handle->info = *task;
	handle->refcount = 0;
	handle->finished = 0;
	handle->need_to_join = 0;
	handle->reached_cleanup = 0;
	handle->completed_normally = 0;
	handle->in_worker_thread = 0;
	handle->in_worker_queue = 0;

	ASYNCIO_DEBUG_CALL(3 FUNC(pthread_cond_init) ARG("%p", &handle->finished_cond) ARG("%p", NULL));
	if ((rc = pthread_cond_init(&handle->finished_cond, NULL)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_cond_init");
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	ASYNCIO_DEBUG_RETURN(RET("%d", 0));
	return 0;
}

static void cleanup_threadpool_handle(struct threadpool_handle *handle)
{
	int rc;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", handle));

	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_cond_destroy) ARG("%p", &handle->finished_cond));
	if ((rc = pthread_cond_destroy(&handle->finished_cond)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_cond_destroy");
	}

	ASYNCIO_DEBUG_RETURN(VOIDRET);
}

static void init_worker_info(struct worker_thread_info *info)
{
	ASYNCIO_DEBUG_ENTER(1 ARG("%p", info));
	info->running = 0;
	ASYNCIO_DEBUG_RETURN(VOIDRET);
}

static int dispatch_contractor(struct threadpool_handle *handle)
{
	pthread_t thread;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", handle));

	if (lock_threadpool_mtx() != 0) {
		ASYNCIO_ERROR("Failed to lock threadpool mtx.\n");
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	if (contractors_count < MAX_CONTRACTORS) {
		if (create_thread(&thread, contractor_thread, handle) == 0) {
			handle->thread = thread;
			handle->need_to_join = 1;
			++contractors_count;
			unlock_threadpool_mtx();

			ASYNCIO_DEBUG_RETURN(RET("%d", 0));
			return 0;
		} else {
			ASYNCIO_ERROR("Failed to create thread.\n");
		}
	} else {
		ASYNCIO_ERROR("Max contractors count limit reached.\n");
	}

	unlock_threadpool_mtx();

	ASYNCIO_DEBUG_RETURN(RET("%d", -1));
	return -1;
}

static int dispatch_worker(struct threadpool_handle *handle)
{
	struct worker_thread_info *worker_info;
	pthread_t thread;
	size_t i;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", handle));

	if (lock_threadpool_mtx() != 0) {
		ASYNCIO_ERROR("Failed to lock threadpool mtx.\n");
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	if (!workers_initialized) {
		for (i = 0; i < MAX_WORKER_THREADS; i++)
			init_worker_info(&worker_threads[i]);

		queue_init(&workers_task_queue);
		workers_initialized = 1;
	}

	/* XXX Is this really necessary to do more than once, after first initialization?
	 * Could also be used in case pthread_create fails during worker cleanup,
	 * so that we keep trying to respawn a worker after it got cancelled.
	 * TODO: should we have a worker thread manager? */
	for (i = 0; i < MAX_WORKER_THREADS; i++) {
		worker_info = &worker_threads[i];

		/* Start worker thread if not running */
		if (!(worker_info->running)) {
			if (create_thread(&thread, worker_thread, worker_info) != 0) {
				ASYNCIO_ERROR("Failed to create thread.\n");
				continue;
			}

			worker_info->thread = thread;
			worker_info->running = 1;
		}
	}

	if (push_worker_task_locked(handle) != 0) {
		ASYNCIO_ERROR("Failed to push worker task.\n");
		unlock_threadpool_mtx();
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	unlock_threadpool_mtx();
	ASYNCIO_DEBUG_RETURN(RET("%d", 0));
	return 0;
}

int threadpool_dispatch(struct threadpool_dispatch_info *task, threadpool_handle_t *handlep)
{
	struct threadpool_handle *handle;
	int oldstate;

	ASYNCIO_DEBUG_ENTER(2 ARG("%p", task) ARG("%p", handlep));
	disable_cancellations(&oldstate);

	if (task->dispatch_info.fn == NULL) {
		restore_cancelstate(oldstate);
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	handle = safe_malloc(sizeof *handle);

	if (handle == NULL) {
		ASYNCIO_ERROR("safe_malloc failed.\n");
		restore_cancelstate(oldstate);
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	if (init_threadpool_handle(handle, task) != 0) {
		safe_free(handle);

		restore_cancelstate(oldstate);
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	/* The caller must have a reference by default to prevent race conditions
	 * where the task completes before the client had a chance to acquire the
	 * handle. Also the contractor or worker thread must have a reference to
	 * prevent the case where the client releases its handle before the worker
	 * or contractor manages to acquire its handle */
	handle->refcount = 2;

	/* XXX Creating contractor threads seems to be much slower than dispatching to
	 * the fixed number of worker threads, so only use them for tasks that are gonna
	 * take a long time, to avoid slowing down batch-dispatching. */
	if (task->flags & THREADPOOL_FLAG_CONTRACTOR) {
		/* Give preference to contractor threads */
		if (dispatch_contractor(handle) == 0) {
			*handlep = (threadpool_handle_t)handle;
			restore_cancelstate(oldstate);
			ASYNCIO_DEBUG_RETURN(RET("%d", 0));
			return 0;
		}

		/* Try with worker thread instead */
		if (dispatch_worker(handle) == 0) {
			*handlep = (threadpool_handle_t)handle;
			restore_cancelstate(oldstate);
			ASYNCIO_DEBUG_RETURN(RET("%d", 0));
			return 0;
		}
	} else {
		/* Give preference to worker threads */
		if (dispatch_worker(handle) == 0) {
			*handlep = (threadpool_handle_t)handle;
			restore_cancelstate(oldstate);
			ASYNCIO_DEBUG_RETURN(RET("%d", 0));
			return 0;
		}

		/* Try with contractor thread instead */
		if (dispatch_contractor(handle) == 0) {
			*handlep = (threadpool_handle_t)handle;
			restore_cancelstate(oldstate);
			ASYNCIO_DEBUG_RETURN(RET("%d", 0));
			return 0;
		}
	}

	cleanup_threadpool_handle(handle);

	safe_free(handle);

	restore_cancelstate(oldstate);
	ASYNCIO_DEBUG_RETURN(RET("%d", -1));
	return -1;
}

int threadpool_cancel(threadpool_handle_t thandle)
{
	struct threadpool_handle *handle;
	int oldstate;
	int rc;
	int success, removed_from_queue;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", thandle));
	disable_cancellations(&oldstate);

	handle = (struct threadpool_handle *)thandle;

	/* Only cancel cancellable threads */
	if (!(handle->info.flags & THREADPOOL_FLAG_CANCELLABLE))
		return -1;

	/* Don't let thread cancel itself. */
	if (pthread_equal(handle->thread, pthread_self()))
		return -1;

	if (lock_threadpool_mtx() != 0) {
		ASYNCIO_ERROR("Failed to lock threadpool mtx.\n");
		restore_cancelstate(oldstate);
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
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
		ASYNCIO_DEBUG_CALL(2 FUNC(pthread_cancel) ARG("%016llx", handle->thread));
		if ((rc = pthread_cancel(handle->thread)) == 0) {
			success = 1;
		} else {
			errno = rc;
			ASYNCIO_SYSERROR("pthread_cancel");
		}
	}

	unlock_threadpool_mtx();

	if (removed_from_queue) {
		/* There's no pthread_join that can return, it's just taken out of queue. */
		notify_handle_finished(handle);

		/* Call cancelled callback */
		if (handle->info.cancelled_info.cb != NULL)
			handle->info.cancelled_info.cb(handle->info.cancelled_info.arg);

		threadpool_release_handle(handle);	/* Release worker's reference to handle */
	}

	restore_cancelstate(oldstate);

	if (success) {
		ASYNCIO_DEBUG_RETURN(RET("%d", 0));
		return 0;
	} else {
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}
}

int threadpool_join(threadpool_handle_t thandle)
{
	struct threadpool_handle *handle;
	int oldstate;
	int oldtype;
	int rc;
	int done, success;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", thandle));
	disable_cancellations(&oldstate);

	handle = (struct threadpool_handle *)thandle;

	if (lock_threadpool_mtx() != 0) {
		ASYNCIO_ERROR("Failed to lock threadpool mtx.\n");
		restore_cancelstate(oldstate);
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	done = 0;
	success = 1;

	/* This whole thing here is pretty complicated and confusing, I am not sure if there's a better way... */
	while (!done) {
		/* Check if someone's already pthread_joining() (or if there's nothing to join with because it was taken out of queue).
		 * In this case we wait on the finished condition variable. Otherwise (if we're the first here) and we need to join, do so. */
		if (handle->need_to_join) {
			handle->need_to_join = 0;
			unlock_threadpool_mtx();

			ASYNCIO_DEBUG_CALL(3 FUNC(pthread_join) ARG("%016llx", handle->thread) ARG("%p", NULL));
			if ((rc = pthread_join(handle->thread, NULL)) != 0) {
				errno = rc;
				ASYNCIO_SYSERROR("pthread_join");
				success = 0;
			}

			/* Let the others know thread has terminated. */
			notify_handle_finished(handle);
			done = 1;
		} else {
			/* Unlock the threadpool_mtx in cleanup handler if cancelled here */
			pthread_cleanup_push(unlock_threadpool_mtx_cleanup, NULL);

			/* Restore cancelstate while waiting for condition variable
			 * to allow cancellation in this case. But set cancellation type to DEFERRED
			 * in order to make sure we cancel during pthread_cond_wait, which should guarantee
			 * that the mtx is locked during the cleanup handler. */
			set_canceltype(PTHREAD_CANCEL_DEFERRED, &oldtype);
			restore_cancelstate(oldstate);

			while (!(handle->finished)) {
				ASYNCIO_DEBUG_CALL(3 FUNC(pthread_cond_wait) ARG("%p", &handle->finished_cond) ARG("%p", &threadpool_mtx));
				if ((rc = pthread_cond_wait(&handle->finished_cond, &threadpool_mtx)) != 0) {
					errno = rc;
					ASYNCIO_SYSERROR("pthread_cond_wait");
					success = 0;
					break;
				}
			}

			disable_cancellations(&oldstate);
			restore_canceltype(oldtype);

			/* This only happens if we were joining a worker thread that got cancelled.
			 * If it completes normally we would not want to pthread_join since it will continue doing other stuff.
			 * But if it cancelled, we need to pthread_join on the thread, but we only learn about cancelled after
			 * waiting on the condition variable. It gets signaled in the worker handle cleanup handler. This is why
			 * we have that while loop here: we might need to repeat the process one more time after waking up here
			 * to wait until the actual thread terminated (we were first waiting until thread reaches cleanup handler). */
			if (handle->in_worker_thread && !(handle->completed_normally) && handle->need_to_join) {
				/* Remember that pthread_cond_wait reacquires the mutex after returning. Only one thread will do the pthread_join again
				 * in second loop iteration, and will then clear the need_to join flag again. The other threads that return from the
				 * pthread_cond_wait will block until this is done, and thus everyone blocks until the thread really has terminated.
				 * So, DO NOT RELEASE MUTEX HERE. */
				done = 0;
			} else {
				/* If we're here then we are actually done so release the lock. */
				unlock_threadpool_mtx();
				done = 1;
			}

			pthread_cleanup_pop(0);
		}
	}

	restore_cancelstate(oldstate);

	if (!success) {
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	ASYNCIO_DEBUG_RETURN(RET("%d", 0));
	return 0;
}

int threadpool_acquire_handle(threadpool_handle_t thandle)
{
	struct threadpool_handle *handle;
	int oldstate;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", thandle));
	disable_cancellations(&oldstate);

	handle = (struct threadpool_handle *)thandle;

	if (lock_threadpool_mtx() != 0) {
		ASYNCIO_ERROR("Failed to lock threadpool mtx.\n");
		restore_cancelstate(oldstate);
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	/* Check for overflow */
	if (handle->refcount >= UINT64T_MAX) {
		ASYNCIO_ERROR("handle refcount overflow.\n");
		unlock_threadpool_mtx();
		restore_cancelstate(oldstate);
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	++(handle->refcount);

	unlock_threadpool_mtx();
	restore_cancelstate(oldstate);
	ASYNCIO_DEBUG_RETURN(RET("%d", 0));
	return 0;
}

void threadpool_release_handle(threadpool_handle_t thandle)
{
	struct threadpool_handle *handle;
	int oldstate;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", thandle));
	disable_cancellations(&oldstate);

	handle = (struct threadpool_handle *)thandle;

	if (lock_threadpool_mtx() != 0) {
		ASYNCIO_ERROR("Failed to lock threadpool mtx.\n");
		restore_cancelstate(oldstate);
		ASYNCIO_DEBUG_RETURN(VOIDRET);
		return;
	}

	/* Check for underflow */
	if (handle->refcount == 0) {
		ASYNCIO_ERROR("Handle refcount already 0 before release.\n");
		unlock_threadpool_mtx();
		restore_cancelstate(oldstate);
		ASYNCIO_DEBUG_RETURN(VOIDRET);
		return;
	}

	--(handle->refcount);

	if (handle->refcount == 0) {
		unlock_threadpool_mtx();
		cleanup_threadpool_handle(handle);

		safe_free(handle);

		restore_cancelstate(oldstate);
		ASYNCIO_DEBUG_RETURN(VOIDRET);
		return;
	}

	unlock_threadpool_mtx();
	restore_cancelstate(oldstate);
	ASYNCIO_DEBUG_RETURN(VOIDRET);
}

void threadpool_cleanup(void)
{
	/* Assumptions: this function must be called only after every other thread has been joined.
	 * There must not be any active threadpool handles when calling threadpool_cleanup().
	 * Only a thread not created by the threadpool module can call this. */
	int rc;
	size_t i;

	ASYNCIO_DEBUG_ENTER(VOIDARG);

	if (lock_threadpool_mtx() != 0) {
		ASYNCIO_ERROR("Failed to lock threadpool mtx.\n");
		ASYNCIO_DEBUG_RETURN(VOIDRET);
		return;
	}

	if (!workers_initialized) {
		unlock_threadpool_mtx();
		ASYNCIO_DEBUG_RETURN(VOIDRET);
		return;
	}

	if (stop_all_worker_tasks_locked() != 0)
		ASYNCIO_ERROR("Failed to stop worker tasks.\n");

	unlock_threadpool_mtx();

	/* Safe to not use mutex here since no other threads are active at this point.
	 * Also the worker threads that are currently in the process of stopping will
	 * not touch the running field since they don't call worker_cleanup_with_respawn. */
	for (i = 0; i < MAX_WORKER_THREADS; i++) {
		/* This can only be false if a worker thread failed to respawn after terminating / cancelling.
		 * In both cases the user had a threadpool_handle with that thread, so he must have joined the
		 * thread already (by assumption above) and we don't need to do it here. */
		if (worker_threads[i].running) {
			ASYNCIO_DEBUG_CALL(3 FUNC(pthread_join) ARG("%016llx", worker_threads[i].thread) ARG("%p", NULL));
			if ((rc = pthread_join(worker_threads[i].thread, NULL)) != 0) {
				errno = rc;
				ASYNCIO_SYSERROR("pthread_join");
			}
		}
	}

	ASYNCIO_DEBUG_RETURN(VOIDRET);
}
