#include <limits.h>
#include <stdint.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

#include "timevents.h"
#include "threadpool.h"
#include "cancellations.h"
#include "queue.h"
#include "priority_queue.h"
#include "monotonic_gettime.h"
#include "safe_malloc.h"
#include "logging.h"
#include "compile_time_assert.h"

#define MAX_DEADLINES		10000

#define UINT64T_MAX		((uint64_t)(-1)) /* Get rid of compiler warning about 'use of C99 long long integer constant' for UINT64_MAX */

/* STRUCT DEFINITIONS */
struct timevent_handle {
	int timeout;
	uint64_t deadline;
	uint32_t flags;
	timevent_callback callback_fn;
	void *callback_arg;

	/* Things that may hold references to handle */
	struct timevents_worker_info *worker_info;	/* worker_info refcount must take into account the handles that contain it */
	int has_threadpool_handle;			/* Protected by timevents_worker_mtx */
	threadpool_handle_t threadpool_handle;		/* threadpool_handle is acquired per handle */

	int finished;					/* Protected by finished_cond_mtx */
	pthread_cond_t finished_cond;
	pthread_mutex_t finished_cond_mtx;

	unsigned int refcount;				/* Protected by mtx */
	pthread_mutex_t mtx;

	/* Used for timevent callback queues */
	struct timevent_handle *prev;
	struct timevent_handle *next;
};

struct timevents_worker_info {
	decl_queue(struct timevent_handle, callbacks)[MAX_DEADLINES];
	struct priority_queue deadlines;
	size_t ndeadlines;
	int wakefd;
	int clearwakefd;

	unsigned int refcount;
};
/* END STRUCT DEFINITIONS */

/* PROTOTYPES */
static int set_nonblocking(int fd);

static int init_timevent_handle(struct timevent_handle *handle, const struct timevent_info *evinfo);
static int lock_timevent_handle(struct timevent_handle *handle);
static void unlock_timevent_handle(struct timevent_handle *handle);
static void cleanup_timevent_handle(struct timevent_handle *handle);
static int reference_timevent_handle_locked(struct timevent_handle *handle);
static void dereference_timevent_handle_locked(struct timevent_handle *handle);

static void unlock_finished_cond_mtx(void *arg);
static int notify_timevent_handle_finished(struct timevent_handle *handle);

static int init_timevents_worker(struct timevents_worker_info *worker_info);
static int acquire_timevents_worker_locked(struct timevents_worker_info *worker_info);
static void release_timevents_worker(struct timevents_worker_info *worker_info);
static void destroy_timevents_worker(struct timevents_worker_info *worker_info);

static int lock_timevents_worker_mutex(void);
static void unlock_timevents_worker_mutex(void);
static int insert_timevents_locked(struct timevents_worker_info *worker_info, struct timevent_handle *handle);
static void remove_timevents_locked(struct timevents_worker_info *worker_info, struct timevent_handle *handle);

static int wake_timevents_worker_locked(struct timevents_worker_info *worker_info);
static int clearwake_timevents_worker_locked(struct timevents_worker_info *worker_info);

static uint32_t get_threadpool_flags(uint32_t timevents_flags);

static int convert_to_deadline(int timeout, uint64_t *deadlinep);
static int convert_to_timeout(uint64_t deadline, int *timeoutp);
static int calculate_timeout_locked(struct timevents_worker_info *worker_info, int *timeoutp);

static void timevent_threadpool_cancelled(void *arg);
static void execute_timevent_callback(void *arg);

static void timevents_loop(void *arg);

static int dispatch_handle_to_loop(struct timevent_handle *handle);
/* END PROTOTYPES */

/* GLOBALS */
/* For now use only one worker... */
static threadpool_handle_t timevents_worker_threadpool_handle;
static struct timevents_worker_info timevents_global_worker_info;
static pthread_mutex_t timevents_worker_mtx = PTHREAD_MUTEX_INITIALIZER;
static int worker_initialized = 0;
/* END GLOBALS */

static int set_nonblocking(int fd)
{
	int oldflags;

	oldflags = fcntl(fd, F_GETFL);

	if (fcntl(fd, F_SETFL, oldflags | O_NONBLOCK) == -1) {
		ASYNCIO_SYSERROR("fcntl");
		return -1;
	}

	return 0;
}

static int init_timevent_handle(struct timevent_handle *handle, const struct timevent_info *evinfo)
{
	uint64_t deadline;

	if (convert_to_deadline(evinfo->timeout, &deadline) != 0) {
		ASYNCIO_ERROR("Failed to convert to deadline.\n");
		goto error;
	}

	handle->timeout = evinfo->timeout;
	handle->deadline = deadline;
	handle->flags = evinfo->flags;
	handle->callback_fn = evinfo->cb;
	handle->callback_arg = evinfo->arg;

	handle->worker_info = NULL;
	handle->has_threadpool_handle = 0;

	handle->refcount = 0;

	if (pthread_mutex_init(&handle->mtx, NULL) != 0) {
		ASYNCIO_SYSERROR("pthread_mutex_init");
		goto error;
	}

	handle->finished = 0;

	if (pthread_mutex_init(&handle->finished_cond_mtx, NULL) != 0) {
		ASYNCIO_SYSERROR("pthread_mutex_init");
		goto error_mtx;
	}

	if (pthread_cond_init(&handle->finished_cond, NULL) != 0) {
		ASYNCIO_SYSERROR("pthread_cond_init");
		goto error_cond_mtx;
	}

	handle->prev = NULL;
	handle->next = NULL;

	return 0;

error_cond_mtx:
	if (pthread_mutex_destroy(&handle->finished_cond_mtx) != 0)
		ASYNCIO_SYSERROR("pthread_mutex_destroy");

error_mtx:
	if (pthread_mutex_destroy(&handle->mtx) != 0)
		ASYNCIO_SYSERROR("pthread_mutex_destroy");

error:
	return -1;
}

static int lock_timevent_handle(struct timevent_handle *handle)
{
	if (pthread_mutex_lock(&handle->mtx) != 0) {
		ASYNCIO_SYSERROR("pthread_mutex_lock");
		return -1;
	}

	return 0;
}

static void unlock_timevent_handle(struct timevent_handle *handle)
{
	if (pthread_mutex_unlock(&handle->mtx) != 0)
		ASYNCIO_SYSERROR("pthread_mutex_unlock");
}

static void cleanup_timevent_handle(struct timevent_handle *handle)
{
	if (handle->has_threadpool_handle)
		threadpool_release_handle(handle->threadpool_handle);

	if (handle->worker_info != NULL)
		release_timevents_worker(handle->worker_info);

	if (pthread_cond_destroy(&handle->finished_cond) != 0)
		ASYNCIO_SYSERROR("pthread_cond_destroy");

	if (pthread_mutex_destroy(&handle->finished_cond_mtx) != 0)
		ASYNCIO_SYSERROR("pthread_mutex_destroy");

	if (pthread_mutex_destroy(&handle->mtx) != 0)
		ASYNCIO_SYSERROR("pthread_mutex_destroy");
}

static int reference_timevent_handle_locked(struct timevent_handle *handle)
{
	if (handle->refcount == UINT_MAX)
		return -1;

	++handle->refcount;
	return 0;
}

static void dereference_timevent_handle_locked(struct timevent_handle *handle)
{
	if (handle->refcount == 0) {
		/* Shouldn't happen */
		ASYNCIO_ERROR("timevent_handle refcount was 0 before dereference.\n");
		return;
	}

	--handle->refcount;
}

static void unlock_finished_cond_mtx(void *arg)
{
	struct timevent_handle *handle;

	handle = (struct timevent_handle *)arg;

	if (pthread_mutex_unlock(&handle->finished_cond_mtx) != 0)
		ASYNCIO_SYSERROR("pthread_mutex_unlock");
}

static int notify_timevent_handle_finished(struct timevent_handle *handle)
{
	if (pthread_mutex_lock(&handle->finished_cond_mtx) != 0) {
		ASYNCIO_SYSERROR("pthread_mutex_lock");
		return -1;
	}

	handle->finished = 1;

	if (pthread_cond_broadcast(&handle->finished_cond) != 0) {
		ASYNCIO_SYSERROR("pthread_cond_broadcast");

		if (pthread_mutex_unlock(&handle->finished_cond_mtx) != 0)
			ASYNCIO_SYSERROR("pthread_mutex_unlock");

		return -1;
	}

	if (pthread_mutex_unlock(&handle->finished_cond_mtx) != 0)
		ASYNCIO_SYSERROR("pthread_mutex_unlock");

	return 0;
}

static int init_timevents_worker(struct timevents_worker_info *worker_info)
{
	int pipefds[2];

	if (priority_queue_init(&worker_info->deadlines, MAX_DEADLINES) != 0) {
		ASYNCIO_ERROR("Failed to initialize deadlines priority queue.\n");
		goto error;
	}

	if (pipe(pipefds) != 0) {
		ASYNCIO_SYSERROR("pipe");
		goto error_deadlines;
	}

	if (set_nonblocking(pipefds[0]) != 0 || set_nonblocking(pipefds[1]) != 0)
		goto error_pipefds;

	/* Add wake event for poll interruption when a change occured in deadlines */
	worker_info->clearwakefd = pipefds[0];
	worker_info->wakefd = pipefds[1];
	worker_info->refcount = 0;

	return 0;

error_pipefds:
	if (close(pipefds[0]) != 0)
		ASYNCIO_SYSERROR("close");

	if (close(pipefds[1]) != 0)
		ASYNCIO_SYSERROR("close");

error_deadlines:
	priority_queue_destroy(&worker_info->deadlines);

error:
	return -1;
}

static int acquire_timevents_worker_locked(struct timevents_worker_info *worker_info)
{
	if (worker_info->refcount == UINT_MAX) {
		ASYNCIO_ERROR("worker_info refcount reached maximal value.\n");
		return -1;
	}

	++worker_info->refcount;
	return 0;
}

static void release_timevents_worker(struct timevents_worker_info *worker_info)
{
	if (lock_timevents_worker_mutex() != 0)
		return;

	if (worker_info->refcount == 0) {
		ASYNCIO_ERROR("worker_info refcount 0 before decrement.\n");
		unlock_timevents_worker_mutex();
		return;
	}

	--worker_info->refcount;

	if (worker_info->refcount == 0) {
		unlock_timevents_worker_mutex();
		destroy_timevents_worker(worker_info);
		/* safe_free(worker_info); // don't do it because it's in the stack */
		return;
	}

	unlock_timevents_worker_mutex();
}

static void destroy_timevents_worker(struct timevents_worker_info *worker_info)
{
	if (close(worker_info->wakefd) != 0)
		ASYNCIO_SYSERROR("close");

	if (close(worker_info->clearwakefd) != 0)
		ASYNCIO_SYSERROR("close");

	priority_queue_destroy(&worker_info->deadlines);
}

static int lock_timevents_worker_mutex()
{
	if (pthread_mutex_lock(&timevents_worker_mtx) != 0) {
		ASYNCIO_SYSERROR("pthread_mutex_lock");
		return -1;
	}

	return 0;
}

static void unlock_timevents_worker_mutex()
{
	if (pthread_mutex_unlock(&timevents_worker_mtx) != 0)
		ASYNCIO_SYSERROR("pthread_mutex_unlock");
}

static int insert_timevents_locked(struct timevents_worker_info *worker_info, struct timevent_handle *handle)
{
	decl_queue(struct timevent_handle, *cbqueue);

	/* The deadline is already in the database, just add handle to callback queue for that deadline */
	if (priority_queue_lookup(&worker_info->deadlines, handle->deadline, (const void **)&cbqueue)) {
		queue_push(cbqueue, handle);
	} else {
		if (worker_info->ndeadlines == MAX_DEADLINES)
			return -1;

		cbqueue = (void *)&worker_info->callbacks[worker_info->ndeadlines];

		if (priority_queue_insert(&worker_info->deadlines, handle->deadline, cbqueue) != 0) {
			ASYNCIO_ERROR("Failed to insert deadline in priority queue.\n");
			return -1;
		}

		queue_init(cbqueue);
		queue_push(cbqueue, handle);

		++worker_info->ndeadlines;
	}

	return 0;
}

static void remove_timevents_locked(struct timevents_worker_info *worker_info, struct timevent_handle *handle)
{
	decl_queue(struct timevent_handle, *cbqueue);
	size_t index;
	size_t last;

	if (!priority_queue_lookup(&worker_info->deadlines, handle->deadline, (const void **)&cbqueue)) {
		ASYNCIO_ERROR("Tried to remove handle not in the worker_info queue.\n");
		return;
	}

	last = worker_info->ndeadlines - 1;
	index = ((unsigned char *)cbqueue - (unsigned char *)&worker_info->callbacks[0]) / sizeof *cbqueue;
	queue_remove(cbqueue, handle);

	if (queue_empty(cbqueue)) {
		priority_queue_delete(&worker_info->deadlines, handle->deadline);

		if (index != last) {
			worker_info->callbacks[index] = worker_info->callbacks[last];
			priority_queue_modify(&worker_info->deadlines, queue_first(&worker_info->callbacks[last])->deadline, cbqueue); /* This can't fail */
		}

		--worker_info->ndeadlines;
	}
}

static int wake_timevents_worker_locked(struct timevents_worker_info *worker_info)
{
	unsigned char dummy;

	dummy = 0;

	if (write(worker_info->wakefd, &dummy, sizeof dummy) != sizeof dummy) {
		ASYNCIO_SYSERROR("write");
		return -1;
	}

	return 0;
}

static int clearwake_timevents_worker_locked(struct timevents_worker_info *worker_info)
{
	unsigned char dummy;

	if (read(worker_info->clearwakefd, &dummy, sizeof dummy) != sizeof dummy) {
		ASYNCIO_SYSERROR("read");
		return -1;
	}

	if (dummy != 0) {
		ASYNCIO_ERROR("Read invalid dummy value.\n");
		return -1;
	}

	return 0;
}

static uint32_t get_threadpool_flags(uint32_t timevents_flags)
{
	uint32_t flags;

	/* Do not use ASYNCHRONOUS CANCELLATIONS here, because we don't want
	 * execute_fdevent_callback to be async-cancellable. It will set the user
	 * desired cancellation state/type there. */
	flags = THREADPOOL_FLAG_NONE;

	if (timevents_flags & TIMEVENT_FLAG_CONTRACTOR)
		flags |= THREADPOOL_FLAG_CONTRACTOR;
	if (timevents_flags & TIMEVENT_FLAG_CANCELLABLE)
		flags |= THREADPOOL_FLAG_CANCELLABLE;

	return flags;
}

static int convert_to_deadline(int timeout, uint64_t *deadlinep)
{
	struct timespec current_time;
	uint64_t deadline;
	uint64_t now;

	if (monotonic_gettime(&current_time) != 0)
		return -1;

	/* Check for overflows */
	if ((uint64_t)current_time.tv_sec > UINT64T_MAX / 1000)
		return -1;

	now = (uint64_t)current_time.tv_sec * 1000 + (uint64_t)current_time.tv_nsec / 1000000;	/* Convert to milliseconds */

	/* Check for overflows */
	if (now > UINT64T_MAX - (uint64_t)timeout)
		return -1;

	deadline = now + (uint64_t)timeout;

	*deadlinep = deadline;
	return 0;
}

static int convert_to_timeout(uint64_t deadline, int *timeoutp)
{
	struct timespec current_time;
	uint64_t now;
	int timeout;

	if (monotonic_gettime(&current_time) != 0)
		return -1;

	/* Check for overflows */
	if ((uint64_t)current_time.tv_sec > UINT64T_MAX / 1000)
		return -1;

	now = (uint64_t)current_time.tv_sec * 1000 + (uint64_t)current_time.tv_nsec / 1000000;	/* Convert to milliseconds */

	if (now > deadline)
		timeout = 0;
	else
		timeout = deadline - now;

	*timeoutp = timeout;
	return 0;
}

static int calculate_timeout_locked(struct timevents_worker_info *worker_info, int *timeoutp)
{
	uint64_t deadline;
	decl_queue(struct timevent_handle, *cbqueue);

	if (priority_queue_empty(&worker_info->deadlines)) {
		*timeoutp = -1;
		return 0;
	}

	if (priority_queue_peek(&worker_info->deadlines, &deadline, (void *)&cbqueue) != 0)
		return -1;

	if (convert_to_timeout(deadline, timeoutp) != 0)
		return -1;

	return 0;
}

static void timevent_threadpool_cancelled(void *arg)
{
	struct timevent_handle *handle;

	handle = (struct timevent_handle *)arg;

	notify_timevent_handle_finished(handle);

	/* Release threadpool's reference to handle */
	timevent_release_handle(handle);
}

static void execute_timevent_callback(void *arg)
{
	struct timevent_handle *handle;
	uint64_t deadline;
	int continued;
	int oldstate;
	int oldtype;

	continued = 0;
	handle = (struct timevent_handle *)arg;

	/* Set user-defined cancellation settings */
	if (handle->flags & TIMEVENT_FLAG_ASYNCCANCEL)
		set_canceltype(CANCEL_ASYNCHRONOUS, &oldtype);
	else
		set_canceltype(CANCEL_DEFERRED, &oldtype);

	if (handle->flags & TIMEVENT_FLAG_CANCELLABLE)
		set_cancelstate(CANCEL_ENABLE, &oldstate);
	else
		set_cancelstate(CANCEL_DISABLE, &oldstate);

	/* Execute the callback */
	handle->callback_fn(handle->callback_arg, &continued);

	restore_cancelstate(oldstate);
	restore_canceltype(oldtype);

	if (continued) {
		/* Acquire reference for next worker dispatch. */
		if (timevent_acquire_handle(handle) == 0) {
			/* Recalculate deadline from timeout */
			if (convert_to_deadline(handle->timeout, &deadline) == 0) {
				handle->deadline = deadline;

				if (dispatch_handle_to_loop(handle) != 0) {
					ASYNCIO_ERROR("Failed to redispatch handle for continue.\n");
					timevent_release_handle(handle);
				}
			} else {
				ASYNCIO_ERROR("Failed to convert to deadline for continue.\n");
				timevent_release_handle(handle);
			}
		} else {
			ASYNCIO_ERROR("Failed to acquire handle for continue.\n");
		}
	} else {
		notify_timevent_handle_finished(handle);
	}

	/* Release threadpool's reference to handle */
	timevent_release_handle(handle);
}

static void timevents_loop(void *arg)
{
	threadpool_handle_t threadpool_handle;
	struct threadpool_dispatch_info threadpool_info;
	struct timevents_worker_info *worker_info;
	struct timevent_handle *handle, *next;
	struct pollfd fds[1];
	decl_queue(struct timevent_handle, *cbqueue);
	uint64_t deadline;
	int timeout;
	int rc;

	worker_info = (struct timevents_worker_info *)arg;

	fds[0].fd = worker_info->clearwakefd;
	fds[0].events = POLLIN;

	for (;;) {
		if (lock_timevents_worker_mutex() != 0) {
			release_timevents_worker(worker_info);
			break;
		}

		if (calculate_timeout_locked(worker_info, &timeout) != 0) {
			ASYNCIO_ERROR("Failed to calculate timeout.\n");
			unlock_timevents_worker_mutex();
			release_timevents_worker(worker_info);
			break;
		}

		unlock_timevents_worker_mutex();
		rc = poll(fds, 1, timeout);

		if (lock_timevents_worker_mutex() != 0) {
			release_timevents_worker(worker_info);
			break;
		}

		if (rc == 0) {
			/* timeout occured */
			if (priority_queue_peek(&worker_info->deadlines, &deadline, (const void **)&cbqueue) != 0) {
				ASYNCIO_ERROR("Failed to peek into priority queue.\n");
				unlock_timevents_worker_mutex();
				release_timevents_worker(worker_info);
				break;
			}

			/* Iterate through all callbacks for that deadline */
			queue_foreach(cbqueue, handle, next) {
				next = handle->next; /* The handle might get removed and the next pointer overwritten otherwise */

				if (timevent_acquire_handle(handle) != 0) {
					ASYNCIO_ERROR("Failed to reference timevent handle");
					continue;
				}

				/* At this point the handle refcount is at least 3:
				 * - the client thread that got the handle
				 * - the timevents_loop thread
				 * - the threadpool dispatched thread */

				/* XXX Ideally do not call threadpool_dispatch here because it calls malloc.
				 * Instead, put the handle in a list and dispatch all of them after the loop,
				 * and after unlocking the mutex. Or use a lock-free queue? */
				threadpool_info.flags = get_threadpool_flags(handle->flags);
				threadpool_info.dispatch_info.fn = execute_timevent_callback;
				threadpool_info.dispatch_info.arg = handle;
				threadpool_info.completed_info.cb = NULL;
				threadpool_info.completed_info.arg = NULL;
				threadpool_info.cancelled_info.cb = timevent_threadpool_cancelled;
				threadpool_info.cancelled_info.arg = handle;

				if (threadpool_dispatch(&threadpool_info, &threadpool_handle) != 0) {
					ASYNCIO_ERROR("Failed to dispatch timevent handle to threadpool.\n");
					timevent_release_handle(handle); /* threadpool won't have the handle after all... */
					continue;
				}

				handle->threadpool_handle = threadpool_handle;
				handle->has_threadpool_handle = 1;

				remove_timevents_locked(worker_info, handle);
				handle->worker_info = NULL;
			}
		} else if (rc == 1) {
			clearwake_timevents_worker_locked(worker_info);
		} else {
			ASYNCIO_SYSERROR("poll");
			unlock_timevents_worker_mutex();
			release_timevents_worker(worker_info);
			break;
		}

		unlock_timevents_worker_mutex();
	}
}

static int dispatch_handle_to_loop(struct timevent_handle *handle)
{
	struct threadpool_dispatch_info timevents_task;

	if (lock_timevents_worker_mutex() != 0)
		return -1;

	if (!worker_initialized) {
		if (init_timevents_worker(&timevents_global_worker_info) != 0) {
			unlock_timevents_worker_mutex();
			return -1;
		}

		/* One reference held by the timevents_loop */
		timevents_global_worker_info.refcount = 1;

		timevents_task.flags = THREADPOOL_FLAG_CONTRACTOR;
		timevents_task.dispatch_info.fn = timevents_loop;
		timevents_task.dispatch_info.arg = &timevents_global_worker_info;
		timevents_task.completed_info.cb = NULL;
		timevents_task.completed_info.arg = NULL;
		timevents_task.cancelled_info.cb = NULL;
		timevents_task.cancelled_info.arg = NULL;

		if (threadpool_dispatch(&timevents_task, &timevents_worker_threadpool_handle) != 0) {
			ASYNCIO_ERROR("Failed to dispatch timevents loop.\n");
			destroy_timevents_worker(&timevents_global_worker_info);
			unlock_timevents_worker_mutex();
			return -1;
		}

		worker_initialized = 1;
	}

	if (insert_timevents_locked(&timevents_global_worker_info, handle) != 0) {
		unlock_timevents_worker_mutex();
		return -1;
	}

	/* Acquire reference to worker_info in handle */
	if (acquire_timevents_worker_locked(&timevents_global_worker_info) != 0) {
		remove_timevents_locked(&timevents_global_worker_info, handle);
		unlock_timevents_worker_mutex();
		return -1;
	}

	handle->worker_info = &timevents_global_worker_info;
	wake_timevents_worker_locked(&timevents_global_worker_info);

	unlock_timevents_worker_mutex();
	return 0;
}

int timevent_register(struct timevent_info *timinfo, timevent_handle_t *timhandle)
{
	struct timevent_handle *handle;
	int oldstate;

	disable_cancellations(&oldstate);

	if (timinfo->cb == NULL) {
		restore_cancelstate(oldstate);
		return -1;
	}

	handle = safe_malloc(sizeof *handle);

	if (handle == NULL) {
		restore_cancelstate(oldstate);
		return -1;
	}

	if (init_timevent_handle(handle, timinfo) != 0) {
		safe_free(handle);
		restore_cancelstate(oldstate);
		return -1;
	}

	/* The caller must have a reference by default to prevent race conditions
	 * where the event completes before the client had a chance to acquire the
	 * handle. Also the timevents loop thread awakened must have a reference to
	 * prevent the case where the client releases its handle before the loop
	 * manages to acquire its handle */
	handle->refcount = 2;

	if (dispatch_handle_to_loop(handle) != 0) {
		cleanup_timevent_handle(handle);
		safe_free(handle);
		restore_cancelstate(oldstate);
		return -1;
	}

	*timhandle = handle;
	restore_cancelstate(oldstate);
	return 0;
}

int timevent_join(timevent_handle_t timhandle)
{
	struct timevent_handle *handle;
	int oldstate;
	int oldtype;
	int rc;

	disable_cancellations(&oldstate);

	handle = (struct timevent_handle *)timhandle;

	if (pthread_mutex_lock(&handle->finished_cond_mtx) != 0) {
		ASYNCIO_SYSERROR("pthread_mutex_lock");
		restore_cancelstate(oldstate);
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

	rc = 0;

	while (!(handle->finished)) {
		if (pthread_cond_wait(&handle->finished_cond, &handle->finished_cond_mtx) != 0) {
			ASYNCIO_SYSERROR("pthread_cond_wait");
			rc = -1;
			break;
		}
	}

	disable_cancellations(&oldstate);
	restore_canceltype(oldtype);

	if (pthread_mutex_unlock(&handle->finished_cond_mtx) != 0)
		ASYNCIO_SYSERROR("pthread_mutex_unlock");

	pthread_cleanup_pop(0);
	restore_cancelstate(oldstate);
	return rc;
}

int timevent_cancel(timevent_handle_t timhandle)
{
	struct timevent_handle *handle;
	int oldstate;
	int release_worker_ref = 0;

	disable_cancellations(&oldstate);

	handle = (struct timevent_handle *)timhandle;

	if (lock_timevents_worker_mutex() != 0) {
		restore_cancelstate(oldstate);
		return -1;
	}

	if (handle->worker_info != NULL) {
		/* Means it has not yet been dispatched to threadpool, so worker won't have access to handle anymore.
		 * Note that if it was in a thread, it will have access due to the cancelled callback. */
		remove_timevents_locked(handle->worker_info, handle);
		wake_timevents_worker_locked(handle->worker_info);
		release_worker_ref = 1;
	}

	unlock_timevents_worker_mutex();

	/* When we took out the handle from worker's database, we need to decrement its refcount */
	if (release_worker_ref)
		timevent_release_handle(handle);

	if (handle->has_threadpool_handle) {
		if (threadpool_cancel(handle->threadpool_handle) != 0) {
			ASYNCIO_ERROR("Failed to cancel threadpool handle.\n");
			restore_cancelstate(oldstate);
			return -1;
		}
	} else {
		if (notify_timevent_handle_finished(handle) != 0) {
			ASYNCIO_ERROR("Failed to notify timevent handle finished.\n");
			restore_cancelstate(oldstate);
			return -1;
		}
	}

	restore_cancelstate(oldstate);
	return 0;
}

int timevent_acquire_handle(timevent_handle_t timhandle)
{
	struct timevent_handle *handle;
	int oldstate;

	disable_cancellations(&oldstate);

	handle = (struct timevent_handle *)timhandle;

	if (lock_timevent_handle(handle) != 0) {
		restore_cancelstate(oldstate);
		return -1;
	}

	if (reference_timevent_handle_locked(handle) != 0) {
		unlock_timevent_handle(handle);
		restore_cancelstate(oldstate);
		return -1;
	}

	unlock_timevent_handle(handle);
	restore_cancelstate(oldstate);
	return 0;
}

void timevent_release_handle(timevent_handle_t timhandle)
{
	struct timevent_handle *handle;
	int oldstate;

	disable_cancellations(&oldstate);

	handle = (struct timevent_handle *)timhandle;

	if (lock_timevent_handle(handle) != 0) {
		restore_cancelstate(oldstate);
		return;
	}

	dereference_timevent_handle_locked(handle);

	if (handle->refcount == 0) {
		unlock_timevent_handle(handle);
		cleanup_timevent_handle(handle);
		safe_free(handle);
		restore_cancelstate(oldstate);
		return;
	}

	unlock_timevent_handle(handle);
	restore_cancelstate(oldstate);
}
