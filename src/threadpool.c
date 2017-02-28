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
	pthread_mutex_t mtx;
	uint64_t refcount;	/* Protected by mtx */

	struct threadpool_dispatch_info info;
	pthread_t thread;
	struct worker_thread_info *worker_info;	/* If NULL, handled by contractor instead of worker. Protected by worker_threads_mtx */
	int in_worker_queue;	/* used to know if worker has it in its task queue. Protected by worker_threads_mtx */

	int in_contractor_queue;	/* Used to know if in contractor queue. Protected by contractors_mtx */

	pthread_mutex_t finished_cond_mtx;
	pthread_cond_t finished_cond;
	int finished;		/* Protected by finished_cond_mtx */

	/* Used for task queues */
	struct threadpool_handle *prev;
	struct threadpool_handle *next;
};

struct worker_thread_info {
	int initialized;					/* Protected by worker_threads_mtx */
	int running;						/* Protected by worker_threads_mtx */
	pthread_t thread;
};
/* END STRUCT DEFINITIONS */

/* PROTOTYPES */
static void unlock_finished_cond_mtx(void *arg);

static void notify_handle_finished(struct threadpool_handle *handle);

static void contractor_finished(void);
static void contractor_cleanup(void *arg);
static void *contractor_thread(void *arg);

static void worker_handle_cleanup(void *arg);
static void worker_cleanup(void *arg);
static void *worker_thread(void *arg);

static int push_worker_task_locked(struct threadpool_handle *handle);
static int pull_worker_task(struct worker_thread_info *worker_info, struct threadpool_handle **handlep);

static int dispatch_contractor(struct threadpool_handle *handle);
static int dispatch_worker(struct threadpool_handle *handle);

static int init_threadpool_handle(struct threadpool_handle *handle, struct threadpool_dispatch_info *task);
static void cleanup_threadpool_handle(struct threadpool_handle *handle);

static void init_worker_info(struct worker_thread_info *info);
/* END PROTOTYPES */

/* GLOBALS */
pthread_mutex_t worker_threads_mtx = PTHREAD_MUTEX_INITIALIZER;
static struct worker_thread_info worker_threads[MAX_WORKER_THREADS];	/* Protected by worker_threads_mtx */
pthread_cond_t workers_newtask_cond = PTHREAD_COND_INITIALIZER;		/* Protected by worker_threads_mtx */
decl_queue(struct threadpool_handle, workers_task_queue);		/* Protected by worker_threads_mtx */
static int workers_initialized = 0;					/* Protected by worker_threads_mtx */

pthread_mutex_t contractors_mtx = PTHREAD_MUTEX_INITIALIZER;
uint64_t contractors_count = 0;						/* Protected by contractors_mtx */
decl_queue(struct threadpool_handle, contractors_task_queue);		/* Protected by contractors_mtx */
static int contractors_initialized = 0;					/* Protected by contractors_mtx */
/* END GLOBALS */

static void unlock_finished_cond_mtx(void *arg)
{
	struct threadpool_handle *handle;
	int rc;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", arg));

	handle = (struct threadpool_handle *)arg;

	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_unlock) ARG("%p", &handle->finished_cond_mtx));
	if ((rc = pthread_mutex_unlock(&handle->finished_cond_mtx)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_mutex_unlock");
	}

	ASYNCIO_DEBUG_RETURN(VOIDRET);
}

static void notify_handle_finished(struct threadpool_handle *handle)
{
	int rc;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", handle));

	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_lock) ARG("%p", &handle->finished_cond_mtx));
	if ((rc = pthread_mutex_lock(&handle->finished_cond_mtx)) == 0) {
		handle->finished = 1;

		ASYNCIO_DEBUG_CALL(2 FUNC(pthread_cond_broadcast) ARG("%p", &handle->finished_cond));
		if ((rc = pthread_cond_broadcast(&handle->finished_cond)) != 0) {
			errno = rc;
			ASYNCIO_SYSERROR("pthread_cond_broadcast");
		}

		ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_unlock) ARG("%p", &handle->finished_cond_mtx));
		if ((rc = pthread_mutex_unlock(&handle->finished_cond_mtx)) != 0) {
			errno = rc;
			ASYNCIO_SYSERROR("pthread_mutex_unlock");
		}
	} else {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_mutex_lock");
	}

	ASYNCIO_DEBUG_RETURN(VOIDRET);
}

static void contractor_finished(void)
{
	struct threadpool_handle *handle;
	pthread_t thread;
	int rc;

	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_lock) ARG("%p", &contractors_mtx));
	if ((rc = pthread_mutex_lock(&contractors_mtx)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_mutex_lock");
		ASYNCIO_DEBUG_RETURN(VOIDRET);
		return;
	}

	if (contractors_count == 0) {
		ASYNCIO_ERROR("contractor count already 0.\n");

		ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_unlock) ARG("%p", &contractors_mtx));
		if ((rc = pthread_mutex_unlock(&contractors_mtx)) != 0) {
			errno = rc;
			ASYNCIO_SYSERROR("pthread_mutex_unlock");
		}

		ASYNCIO_DEBUG_RETURN(VOIDRET);
		return;
	}

	--contractors_count;

	/* Dispatch new contractor for tasks in contractors task queue */
	if (!queue_empty(&contractors_task_queue)) {
		queue_pop(&contractors_task_queue, &handle);

		ASYNCIO_DEBUG_CALL(5 FUNC(pthread_create) ARG("%p", &thread) ARG("%p", NULL) ARG("%p", contractor_thread) ARG("%p", handle));
		if ((rc = pthread_create(&thread, NULL, contractor_thread, handle)) != 0) {
			errno = rc;
			ASYNCIO_SYSERROR("pthread_create");

			queue_push(&contractors_task_queue, handle);	/* XXX Should push back to where it was? */

			ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_unlock) ARG("%p", &contractors_mtx));
			if ((rc = pthread_mutex_unlock(&contractors_mtx)) != 0) {
				errno = rc;
				ASYNCIO_SYSERROR("pthread_mutex_unlock");
			}

			ASYNCIO_DEBUG_RETURN(VOIDRET);
			return;
		}

		handle->thread = thread;
		handle->in_contractor_queue = 0;
		++contractors_count;
	}

	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_unlock) ARG("%p", &contractors_mtx));
	if ((rc = pthread_mutex_unlock(&contractors_mtx)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_mutex_unlock");
	}

	ASYNCIO_DEBUG_RETURN(VOIDRET);
}

static void contractor_cleanup(void *arg)
{
	struct threadpool_handle *handle;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", arg));

	/* handle was acquired in contractor_thread */
	handle = (struct threadpool_handle *)arg;

	notify_handle_finished(handle);

	/* Call cancelled callback */
	if (handle->info.cancelled_info.cb != NULL)
		handle->info.cancelled_info.cb(handle->info.cancelled_info.arg);

	/* Release our handle */
	threadpool_release_handle(handle);

	contractor_finished();

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

	pthread_cleanup_pop(0);

	/* Broadcast condition that thread finished */
	notify_handle_finished(handle);

	/* Call completed callback */
	if (handle->info.completed_info.cb != NULL)
		handle->info.completed_info.cb(handle->info.completed_info.arg);

	threadpool_release_handle(handle); /* It was acquired implicitly by threadpool_dispatch when it dispatched it to us */

	contractor_finished();

	restore_cancelstate(oldstate);
	ASYNCIO_DEBUG_RETURN(RET("%p", NULL));
	return NULL;
}

static void worker_handle_cleanup(void *arg)
{
	struct threadpool_handle *handle;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", arg));

	/* handle was acquired in contractor_thread */
	handle = (struct threadpool_handle *)arg;

	notify_handle_finished(handle);

	/* Call cancelled callback */
	if (handle->info.cancelled_info.cb != NULL)
		handle->info.cancelled_info.cb(handle->info.cancelled_info.arg);

	/* Release our handle */
	threadpool_release_handle(handle);

	ASYNCIO_DEBUG_RETURN(VOIDRET);
}

static void worker_cleanup(void *arg)
{
	struct worker_thread_info *worker_info;
	pthread_t thread;
	int rc;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", arg));

	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_lock) ARG("%p", &worker_threads_mtx));
	if ((rc = pthread_mutex_lock(&worker_threads_mtx)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_mutex_lock");
		ASYNCIO_DEBUG_RETURN(VOIDRET);
		return;
	}

	worker_info = (struct worker_thread_info *)arg;
	worker_info->running = 0;

	ASYNCIO_DEBUG_CALL(5 FUNC(pthread_create) ARG("%p", &thread) ARG("%p", NULL) ARG("%p", worker_thread) ARG("%p", worker_info));
	if ((rc = pthread_create(&thread, NULL, worker_thread, worker_info)) == 0) {
		worker_info->running = 1;
		worker_info->thread = thread;
	} else {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_create\n");
	}

	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_unlock) ARG("%p", &worker_threads_mtx));
	if ((rc = pthread_mutex_unlock(&worker_threads_mtx)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_mutex_unlock");
	}

	ASYNCIO_DEBUG_RETURN(VOIDRET);
}

static void *worker_thread(void *arg)
{
	struct worker_thread_info *worker_info;
	struct threadpool_handle *handle;
	int oldstate, oldstate1;
	int oldtype;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", arg));
	disable_cancellations(&oldstate);

	worker_info = (struct worker_thread_info *)arg;

	pthread_cleanup_push(worker_cleanup, worker_info);

	for (;;) {
		if (pull_worker_task(worker_info, &handle) != 0) {
			ASYNCIO_ERROR("Failed to pull worker task.\n");
			ASYNCIO_DEBUG_CALL(2 FUNC(usleep) ARG("%d", 10000));
			usleep(10000);
			continue;
		}

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

		pthread_cleanup_pop(0);

		/* Broadcast condition that thread finished */
		notify_handle_finished(handle);

		/* Call completed callback */
		if (handle->info.completed_info.cb != NULL)
			handle->info.completed_info.cb(handle->info.completed_info.arg);

		threadpool_release_handle(handle);
	}

	pthread_cleanup_pop(0);
	worker_info->running = 0;

	restore_cancelstate(oldstate);

	ASYNCIO_DEBUG_RETURN(RET("%p", NULL));
	return NULL;
}

static int push_worker_task_locked(struct threadpool_handle *handle)
{
	int rc;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", handle));

	/* Push new task into worker task queue */
	queue_push(&workers_task_queue, handle);

	/* Wake up a worker */
	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_cond_signal) ARG("%p", &workers_newtask_cond));
	if ((rc = pthread_cond_signal(&workers_newtask_cond)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_cond_signal\n");

		queue_remove(&workers_task_queue, handle);
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	handle->in_worker_queue = 1;
	ASYNCIO_DEBUG_RETURN(RET("%d", 0));
	return 0;
}

static int pull_worker_task(struct worker_thread_info *worker_info, struct threadpool_handle **handlep)
{
	struct threadpool_handle *handle;
	int rc;

	ASYNCIO_DEBUG_ENTER(2 ARG("%p", worker_info) ARG("%p", handlep));

	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_lock) ARG("%p", &worker_threads_mtx));
	if ((rc = pthread_mutex_lock(&worker_threads_mtx)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_mutex_lock");
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	while (queue_empty(&workers_task_queue)) {
		ASYNCIO_DEBUG_CALL(3 FUNC(pthread_cond_wait) ARG("%p", &workers_newtask_cond) ARG("%p", &worker_threads_mtx));
		if ((rc = pthread_cond_wait(&workers_newtask_cond, &worker_threads_mtx)) != 0) {
			errno = rc;
			ASYNCIO_SYSERROR("pthread_cond_wait");

			ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_unlock) ARG("%p", &worker_threads_mtx));
			if ((rc = pthread_mutex_unlock(&worker_threads_mtx)) != 0) {
				errno = rc;
				ASYNCIO_SYSERROR("pthread_mutex_unlock");
			}

			ASYNCIO_DEBUG_RETURN(RET("%d", -1));
			return -1;
		}
	}

	/* Pop new task from queue (this is a misnomer... it's not pop-ing the task that was pushed like a stack) */
	queue_pop(&workers_task_queue, &handle);
	handle->thread = worker_info->thread;
	handle->worker_info = worker_info;
	handle->in_worker_queue = 0; /* Protected by the worker_threads_mtx */

	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_unlock) ARG("%p", &worker_threads_mtx));
	if ((rc = pthread_mutex_unlock(&worker_threads_mtx)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_mutex_unlock");
		/* Return anyways since we already got the handle anyways... */
	}

	*handlep = handle;
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
	handle->worker_info = NULL;
	handle->in_worker_queue = 0;
	handle->in_contractor_queue = 0;

	ASYNCIO_DEBUG_CALL(3 FUNC(pthread_mutex_init) ARG("%p", &handle->mtx) ARG("%p", NULL));
	if ((rc = pthread_mutex_init(&handle->mtx, NULL)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_mutex_init");
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	ASYNCIO_DEBUG_CALL(3 FUNC(pthread_mutex_init) ARG("%p", &handle->finished_cond_mtx) ARG("%p", NULL));
	if ((rc = pthread_mutex_init(&handle->finished_cond_mtx, NULL)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_mutex_init");

		ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_destroy) ARG("%p", &handle->mtx));
		if ((rc = pthread_mutex_destroy(&handle->mtx)) != 0) {
			errno = rc;
			ASYNCIO_SYSERROR("pthread_mutex_destroy");
		}

		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	ASYNCIO_DEBUG_CALL(3 FUNC(pthread_cond_init) ARG("%p", &handle->finished_cond) ARG("%p", NULL));
	if ((rc = pthread_cond_init(&handle->finished_cond, NULL)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_cond_init");

		ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_destroy) ARG("%p", &handle->finished_cond_mtx));
		if ((rc = pthread_mutex_destroy(&handle->finished_cond_mtx)) != 0) {
			errno = rc;
			ASYNCIO_SYSERROR("pthread_mutex_destroy");
		}

		ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_destroy) ARG("%p", &handle->mtx));
		if ((rc = pthread_mutex_destroy(&handle->mtx)) != 0) {
			errno = rc;
			ASYNCIO_SYSERROR("pthread_mutex_destroy");
		}

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

	/* Should only detach if handle was with contractor. Worker threads are detached in worker cleanup. */
	if (handle->worker_info == NULL) {
		ASYNCIO_DEBUG_CALL(2 FUNC(pthread_detach) ARG("%016llx", handle->thread));
		if ((rc = pthread_detach(handle->thread)) != 0) {	/* Release pthread_t ressources for that thread (this is what allows multiple pthread_cancels()) */
			errno = rc;
			ASYNCIO_SYSERROR("pthread_detach");
		}
	}

	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_cond_destroy) ARG("%p", &handle->finished_cond));
	if ((rc = pthread_cond_destroy(&handle->finished_cond)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_cond_destroy");
	}

	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_destroy) ARG("%p", &handle->finished_cond_mtx));
	if ((rc = pthread_mutex_destroy(&handle->finished_cond_mtx)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_mutex_destroy");
	}

	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_destroy) ARG("%p", &handle->mtx));
	if ((rc = pthread_mutex_destroy(&handle->mtx)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_mutex_destroy");
	}

	ASYNCIO_DEBUG_RETURN(VOIDRET);
}

static void init_worker_info(struct worker_thread_info *info)
{
	ASYNCIO_DEBUG_ENTER(1 ARG("%p", info));

	info->running = 0;
	info->initialized = 1;

	ASYNCIO_DEBUG_RETURN(VOIDRET);;
}

static int dispatch_contractor(struct threadpool_handle *handle)
{
	pthread_t thread;
	int success;
	int rc;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", handle));

	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_lock) ARG("%p", &contractors_mtx));
	if ((rc = pthread_mutex_lock(&contractors_mtx)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_mutex_lock");
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	if (!contractors_initialized) {
		queue_init(&contractors_task_queue);
		contractors_initialized = 1;
	}

	success = 1;

	if (contractors_count < MAX_CONTRACTORS) {
		ASYNCIO_DEBUG_CALL(5 FUNC(pthread_create) ARG("%p", &thread) ARG("%p", NULL) ARG("%p", contractor_thread) ARG("%p", handle));
		if ((rc = pthread_create(&thread, NULL, contractor_thread, handle)) != 0) {
			errno = rc;
			ASYNCIO_SYSERROR("pthread_create");
			success = 0;
		}

		handle->thread = thread;
		handle->in_contractor_queue = 0;
		++contractors_count;
	} else {
		queue_push(&contractors_task_queue, handle);
		handle->in_contractor_queue = 1;
	}

	if ((rc = pthread_mutex_unlock(&contractors_mtx)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_mutex_unlock");
	}

	if (!success) {
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	ASYNCIO_DEBUG_RETURN(RET("%d", 0));
	return 0;
}

static int dispatch_worker(struct threadpool_handle *handle)
{
	struct worker_thread_info *worker_info;
	pthread_t thread;
	size_t i;
	int rc;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", handle));

	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_lock) ARG("%p", &worker_threads_mtx));
	if ((rc = pthread_mutex_lock(&worker_threads_mtx)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_mutex_lock");
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	if (!workers_initialized) {
		for (i = 0; i < MAX_WORKER_THREADS; i++)
			init_worker_info(&worker_threads[i]);

		queue_init(&workers_task_queue);
		workers_initialized = 1;
	}

	/* XXX Is this really necessary? Used in case pthread_create fails during worker cleanup,
	 * so that we keep trying to respawn a worker after it got cancelled. */
	for (i = 0; i < MAX_WORKER_THREADS; i++) {
		worker_info = &worker_threads[i];

		/* Start worker thread if not running */
		if (!(worker_info->running)) {
			ASYNCIO_DEBUG_CALL(5 FUNC(pthread_create) ARG("%p", &thread) ARG("%p", NULL) ARG("%p", worker_thread) ARG("%p", worker_info));
			if ((rc = pthread_create(&thread, NULL, worker_thread, worker_info)) != 0) {
				errno = rc;
				ASYNCIO_SYSERROR("pthread_create");
				continue;
			}

			worker_info->thread = thread;
			worker_info->running = 1;
		}
	}

	if (push_worker_task_locked(handle) != 0) {
		ASYNCIO_ERROR("Failed to push worker task.\n");

		ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_unlock) ARG("%p", &worker_threads_mtx));
		if ((rc = pthread_mutex_unlock(&worker_threads_mtx)) != 0) {
			errno = rc;
			ASYNCIO_SYSERROR("pthread_mutex_unlock");
		}

		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_unlock) ARG("%p", &worker_threads_mtx));
	if ((rc = pthread_mutex_unlock(&worker_threads_mtx)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_mutex_unlock");
	}

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
		ASYNCIO_ERROR("safe_malloc");
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

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", thandle));
	disable_cancellations(&oldstate);

	handle = (struct threadpool_handle *)thandle;

	/* Only cancel cancellable threads */
	if (!(handle->info.flags & THREADPOOL_FLAG_CANCELLABLE))
		return -1;

	/* Don't let thread cancel itself. */
	if (pthread_equal(handle->thread, pthread_self()))
		return -1;

	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_lock) ARG("%p", &worker_threads_mtx));
	if ((rc = pthread_mutex_lock(&worker_threads_mtx)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_mutex_lock");

		restore_cancelstate(oldstate);
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	/* Remove handle from worker queue (if in worker) */
	if (handle->worker_info != NULL) {
		if (handle->in_worker_queue) {
			queue_remove(&workers_task_queue, handle);
			handle->in_worker_queue = 0;
			notify_handle_finished(handle);
		} else {
			ASYNCIO_DEBUG_CALL(2 FUNC(pthread_cancel) ARG("%016llx", handle->thread));
			if ((rc = pthread_cancel(handle->thread)) != 0) {
				/* Some implementations return ESRCH if pthread_cancel is called
				 * on a thread that has already terminated normally by returning
				 * from the pthread_create callback function. This is not an error,
				 * just ignore it. */
				if (rc != ESRCH) {
					errno = rc;
					ASYNCIO_SYSERROR("pthread_cancel");

					ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_unlock) ARG("%p", &worker_threads_mtx));
					if ((rc = pthread_mutex_unlock(&worker_threads_mtx)) != 0) {
						errno = rc;
						ASYNCIO_SYSERROR("pthread_mutex_unlock");
					}

					restore_cancelstate(oldstate);
					ASYNCIO_DEBUG_RETURN(RET("%d", -1));
					return -1;
				}
			}
		}

		ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_unlock) ARG("%p", &worker_threads_mtx));
		if ((rc = pthread_mutex_unlock(&worker_threads_mtx)) != 0) {
			errno = rc;
			ASYNCIO_SYSERROR("pthread_mutex_unlock");
		}

		restore_cancelstate(oldstate);
		ASYNCIO_DEBUG_RETURN(RET("%d", 0));
		return 0;
	}

	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_unlock) ARG("%p", &worker_threads_mtx));
	if ((rc = pthread_mutex_unlock(&worker_threads_mtx)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_mutex_unlock");
	}

	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_lock) ARG("%p", &contractors_mtx));
	if ((rc = pthread_mutex_lock(&contractors_mtx)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_mutex_lock");

		restore_cancelstate(oldstate);
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	if (handle->in_contractor_queue) {
		queue_remove(&contractors_task_queue, handle);
		notify_handle_finished(handle);
	} else {
		ASYNCIO_DEBUG_CALL(2 FUNC(pthread_cancel) ARG("%016llx", handle->thread));
		if ((rc = pthread_cancel(handle->thread)) != 0) {
			/* Some implementations return ESRCH if pthread_cancel is called
			 * on a thread that has already terminated normally by returning
			 * from the pthread_create callback function. This is not an error,
			 * just ignore it. */
			if (rc != ESRCH) {
				errno = rc;
				ASYNCIO_SYSERROR("pthread_cancel");

				ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_unlock) ARG("%p", &contractors_mtx));
				if ((rc = pthread_mutex_unlock(&contractors_mtx)) != 0) {
					errno = rc;
					ASYNCIO_SYSERROR("pthread_mutex_unlock");
				}

				restore_cancelstate(oldstate);
				ASYNCIO_DEBUG_RETURN(RET("%d", -1));
				return -1;
			}
		}
	}

	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_unlock) ARG("%p", &contractors_mtx));
	if ((rc = pthread_mutex_unlock(&contractors_mtx)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_mutex_unlock");
	}

	restore_cancelstate(oldstate);
	ASYNCIO_DEBUG_RETURN(RET("%d", 0));
	return 0;
}

int threadpool_join(threadpool_handle_t thandle)
{
	struct threadpool_handle *handle;
	int oldstate;
	int oldtype;
	int rc;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", thandle));
	disable_cancellations(&oldstate);

	handle = (struct threadpool_handle *)thandle;

	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_lock) ARG("%p", &handle->finished_cond_mtx));
	if ((rc = pthread_mutex_lock(&handle->finished_cond_mtx)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_mutex_lock");

		restore_cancelstate(oldstate);
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	/* Unlock the finished_cond_mtx in cleanup handler if cancelled here */
	pthread_cleanup_push(unlock_finished_cond_mtx, handle);

	/* Restore cancelstate while waiting for condition variable
	 * to allow cancellation in this case. But set cancellation type to DEFERRED
	 * in order to make sure we cancel during pthread_cond_wait, which should guarantee
	 * that the finished_cond_mtx is locked during the cleanup handler. */
	set_canceltype(PTHREAD_CANCEL_DEFERRED, &oldtype);
	restore_cancelstate(oldstate);

	while (!(handle->finished)) {
		ASYNCIO_DEBUG_CALL(3 FUNC(pthread_cond_wait) ARG("%p", &handle->finished_cond) ARG("%p", &handle->finished_cond_mtx));
		if ((rc = pthread_cond_wait(&handle->finished_cond, &handle->finished_cond_mtx)) != 0) {
			errno = rc;
			ASYNCIO_SYSERROR("pthread_cond_wait");
			break;
		}
	}

	disable_cancellations(&oldstate);
	restore_canceltype(oldtype);

	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_unlock) ARG("%p", &handle->finished_cond_mtx));
	if ((rc = pthread_mutex_unlock(&handle->finished_cond_mtx)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_mutex_unlock");
	}

	pthread_cleanup_pop(0);

	restore_cancelstate(oldstate);

	/* This should be only because of pthread_cond_wait failing, not pthread_mutex_unlock */
	if (rc != 0) {
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
	int rc;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", thandle));
	disable_cancellations(&oldstate);

	handle = (struct threadpool_handle *)thandle;

	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_lock) ARG("%p", &handle->mtx));
	if ((rc = pthread_mutex_lock(&handle->mtx)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_mutex_lock");

		restore_cancelstate(oldstate);
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	/* Check for overflow */
	if (handle->refcount >= UINT64T_MAX) {
		ASYNCIO_ERROR("handle refcount overflow\n");

		ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_unlock) ARG("%p", &handle->mtx));
		if ((rc = pthread_mutex_unlock(&handle->mtx)) != 0) {
			errno = rc;
			ASYNCIO_SYSERROR("pthread_mutex_unlock");
		}

		restore_cancelstate(oldstate);
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	++(handle->refcount);

	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_unlock) ARG("%p", &handle->mtx));
	if ((rc = pthread_mutex_unlock(&handle->mtx)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_mutex_unlock");
	}

	restore_cancelstate(oldstate);
	ASYNCIO_DEBUG_RETURN(RET("%d", 0));
	return 0;
}

void threadpool_release_handle(threadpool_handle_t thandle)
{
	struct threadpool_handle *handle;
	int oldstate;
	int rc;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", thandle));
	disable_cancellations(&oldstate);

	handle = (struct threadpool_handle *)thandle;

	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_lock) ARG("%p", &handle->mtx));
	if ((rc = pthread_mutex_lock(&handle->mtx)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_mutex_lock");
		restore_cancelstate(oldstate);
		ASYNCIO_DEBUG_RETURN(VOIDRET);
		return;
	}

	/* Check for underflow */
	if (handle->refcount == 0) {
		ASYNCIO_ERROR("Handle refcount already 0 before release.\n");

		ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_unlock) ARG("%p", &handle->mtx));
		if ((rc = pthread_mutex_unlock(&handle->mtx)) != 0) {
			errno = rc;
			ASYNCIO_SYSERROR("pthread_mutex_unlock");
		}

		restore_cancelstate(oldstate);
		ASYNCIO_DEBUG_RETURN(VOIDRET);
		return;
	}

	--(handle->refcount);

	if (handle->refcount == 0) {
		ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_unlock) ARG("%p", &handle->mtx));
		if ((rc = pthread_mutex_unlock(&handle->mtx)) != 0) {
			errno = rc;
			ASYNCIO_SYSERROR("pthread_mutex_unlock");
		}

		cleanup_threadpool_handle(handle);

		safe_free(handle);

		restore_cancelstate(oldstate);
		ASYNCIO_DEBUG_RETURN(VOIDRET);
		return;
	}

	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_unlock) ARG("%p", &handle->mtx));
	if ((rc = pthread_mutex_unlock(&handle->mtx)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_mutex_unlock");
	}

	restore_cancelstate(oldstate);
	ASYNCIO_DEBUG_RETURN(VOIDRET);
}
