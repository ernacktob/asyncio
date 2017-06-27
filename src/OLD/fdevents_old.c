#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <stdio.h>

#include "fdevents.h"
#include "threadpool.h"
#include "cancellations.h"
#include "queue.h"
#include "hashtable.h"
#include "safe_malloc.h"
#include "logging.h"
#include "compile_time_assert.h"

#define MAX_POLLFDS		10000
#define POLLFD_EVENT_NBITS	(CHAR_BIT * sizeof (short))

/* STRUCT DEFINITIONS */
struct fdevent_handle {
	int fd;
	short events;
	short revents;					/* Set by the fdevents_loop to received events */
	uint32_t flags;
	fdevent_callback callback_fn;
	void *callback_arg;

	int in_worker_database;
	int has_threadpool_handle;
	threadpool_handle_t threadpool_handle;		/* threadpool_handle is acquired per handle */

	int continued;				/* Redispatch to event loop */

	int finished;				/* XXX Should be set through threadpool completed/cancelled callback */
	pthread_cond_t finished_cond;

	unsigned int refcount;

	/* Used for fdevent callback queues */
	struct fdevent_handle *prev;
	struct fdevent_handle *next;
};

struct fdevent_refcount {
	unsigned int bitcounts[POLLFD_EVENT_NBITS];
};

struct fdevents_worker_info {
	nfds_t nfds;
	struct pollfd fds[MAX_POLLFDS];
	decl_queue(struct fdevent_handle, callbacks)[MAX_POLLFDS];
	struct fdevent_refcount fdevent_refcounts[MAX_POLLFDS];
	struct hashtable fd_map;
	int changed;
	int wakefd;
	int clearwakefd;
};
/* END STRUCT DEFINITIONS */

/* PROTOTYPES */
static int set_nonblocking(int fd);

static int lock_fdevents_mtx(void);
static void unlock_fdevents_mtx(void);
static void unlock_fdevents_mtx_cleanup(void *arg);

static int init_fdevent_handle(struct fdevent_handle *handle, const struct fdevent_info *evinfo);
static void cleanup_fdevent_handle(struct fdevent_handle *handle);
static int reference_fdevent_handle_locked(struct fdevent_handle *handle);
static void dereference_fdevent_handle_locked(struct fdevent_handle *handle);

static int notify_fdevent_handle_finished(struct fdevent_handle *handle);

static void init_fdevent_refcount_locked(struct fdevent_refcount *refcounts);
static int increment_fdevent_refcount_locked(struct fdevent_refcount *refcounts, short events);
static void decrement_fdevent_refcount_locked(struct fdevent_refcount *refcounts, short events);
static short get_eventsmask_from_fdevent_refcount_locked(const struct fdevent_refcount *refcounts);

static int init_fdevents_worker(struct fdevents_worker_info *worker_info);
static void destroy_fdevents_worker(struct fdevents_worker_info *worker_info);

static int insert_fdevents_locked(struct fdevents_worker_info *worker_info, struct fdevent_handle *handle);
static void remove_fdevents_locked(struct fdevents_worker_info *worker_info, struct fdevent_handle *handle);

static void copy_fdevents_locked(const struct fdevents_worker_info *worker_info, struct pollfd *fds, nfds_t *nfdsp);
static void clear_changed_fdevents_locked(struct fdevents_worker_info *worker_info);
static int set_changed_fdevents_locked(struct fdevents_worker_info *worker_info);
static int has_changed_fdevents_locked(struct fdevents_worker_info *worker_info);

static int wake_fdevents_worker_locked(struct fdevents_worker_info *worker_info);
static int clearwake_fdevents_worker_locked(struct fdevents_worker_info *worker_info);

static uint32_t get_threadpool_flags(uint32_t fdevents_flags);
static uint16_t to_fdevents_events(short poll_events);
static short to_poll_events(uint16_t events);

static void fdevent_threadpool_cancelled(void *arg);
static void execute_fdevent_callback(void *arg);

static void fdevents_loop(void *arg);

static int dispatch_handle_to_loop(struct fdevent_handle *handle);
static int redispatch_handle_to_loop(struct fdevent_handle *handle);
static int launch_redispatcher_thread(struct fdevent_handle *handle);
/* END PROTOTYPES */

/* GLOBALS */
static pthread_mutex_t fdevents_mtx = PTHREAD_MUTEX_INITIALIZER;
static volatile int fdevents_stopped = 0;

static pthread_rwlock_t initialization_lock = PTHREAD_RWLOCK_INITIALIZER;
static volatile int fdevents_initialized = 0;

/* For now use only one worker... */
static threadpool_handle_t fdevents_worker_threadpool_handle;
static struct fdevents_worker_info fdevents_global_worker_info;
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

static int lock_fdevents_mtx()
{
	int rc;

	if ((rc = pthread_mutex_lock(&fdevents_mtx)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_mutex_lock");
		return -1;
	}

	return 0;
}

static void unlock_fdevents_mtx()
{
	int rc;

	if ((rc = pthread_mutex_unlock(&fdevents_mtx)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_mutex_unlock");
	}
}

static void unlock_fdevents_mtx_cleanup(void *arg)
{
	(void)arg;
	unlock_fdevents_mtx();
}

static int init_fdevent_handle(struct fdevent_handle *handle, const struct fdevent_info *evinfo)
{
	int rc;

	handle->fd = evinfo->fd;
	handle->events = to_poll_events(evinfo->events);
	handle->revents = 0;
	handle->flags = evinfo->flags;
	handle->callback_fn = evinfo->cb;
	handle->callback_arg = evinfo->arg;

	handle->in_worker_database = 0;
	handle->has_threadpool_handle = 0;

	handle->refcount = 0;
	handle->finished = 0;

	if ((rc = pthread_cond_init(&handle->finished_cond, NULL)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_cond_init");
		return -1;
	}

	handle->prev = NULL;
	handle->next = NULL;

	return 0;
}

static void cleanup_fdevent_handle(struct fdevent_handle *handle)
{
	int rc;

	if (handle->has_threadpool_handle)
		threadpool_release_handle(handle->threadpool_handle);

	if ((rc = pthread_cond_destroy(&handle->finished_cond)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_cond_destroy");
	}
}

static int reference_fdevent_handle_locked(struct fdevent_handle *handle)
{
	if (handle->refcount == UINT_MAX) {
		ASYNCIO_ERROR("Reached maximum refcount for handle.");
		return -1;
	}

	++(handle->refcount);
	return 0;
}

static void dereference_fdevent_handle_locked(struct fdevent_handle *handle)
{
	if (handle->refcount == 0) {
		/* Shouldn't happen */
		ASYNCIO_ERROR("fdevent_handle refcount was 0 before dereference.\n");
		return;
	}

	--(handle->refcount);
}

static int notify_fdevent_handle_finished(struct fdevent_handle *handle)
{
	int rc;

	printf("Notifying.\n");
	if (lock_fdevents_mtx() != 0) {
		ASYNCIO_ERROR("Failed to lock fdevents mtx.\n");
		return -1;
	}

	handle->finished = 1;

	if ((rc = pthread_cond_broadcast(&handle->finished_cond)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_cond_broadcast");

		unlock_fdevents_mtx();
		return -1;
	}

	unlock_fdevents_mtx();
	return 0;
}

static void init_fdevent_refcount_locked(struct fdevent_refcount *refcounts)
{
	unsigned int i;

	COMPILE_TIME_ASSERT(UINT_MAX > POLLFD_EVENT_NBITS);

	for (i = 0; i < POLLFD_EVENT_NBITS; i++)
		refcounts->bitcounts[i] = 0;
}

static int increment_fdevent_refcount_locked(struct fdevent_refcount *refcounts, short events)
{
	unsigned int i;

	COMPILE_TIME_ASSERT(UINT_MAX > POLLFD_EVENT_NBITS);

	/* First check that we can increment without overflows */
	for (i = 0; i < POLLFD_EVENT_NBITS; i++) {
		if ((events & (1 << i)) && (refcounts->bitcounts[i] == UINT_MAX)) {
			ASYNCIO_ERROR("Reached maximum count for fdevent refcounts.\n");
			return -1;
		}
	}

	for (i = 0; i < POLLFD_EVENT_NBITS; i++) {
		if (events & (1 << i))
			++refcounts->bitcounts[i];
	}

	return 0;
}

static void decrement_fdevent_refcount_locked(struct fdevent_refcount *refcounts, short events)
{
	unsigned int i;

	COMPILE_TIME_ASSERT(UINT_MAX > POLLFD_EVENT_NBITS);

	/* First check that we can decrement without underflows */
	for (i = 0; i < POLLFD_EVENT_NBITS; i++) {
		if ((events & (1 << i)) && (refcounts->bitcounts[i] == 0)) {
			ASYNCIO_ERROR("fdevent_refcount bitcounts is zero before decrement.\n");
			return;
		}
	}

	for (i = 0; i < POLLFD_EVENT_NBITS; i++) {
		if (events & (1 << i))
			--refcounts->bitcounts[i];
	}
}

static short get_eventsmask_from_fdevent_refcount_locked(const struct fdevent_refcount *refcounts)
{
	short events;
	unsigned int i;

	COMPILE_TIME_ASSERT(UINT_MAX > POLLFD_EVENT_NBITS);

	events = 0;

	for (i = 0; i < POLLFD_EVENT_NBITS; i++) {
		if (refcounts->bitcounts[i] > 0)
			events |= (1 << i);
	}

	return events;
}

static int init_fdevents_worker(struct fdevents_worker_info *worker_info)
{
	int pipefds[2];

	/* No need to insert wake pipefd[0] in hashtable, we always know it's at index 0 */
	if (hashtable_init(&worker_info->fd_map, MAX_POLLFDS) != 0) {
		ASYNCIO_ERROR("Failed to initialize fdevents hashtable.\n");
		goto error;
	}

	if (pipe(pipefds) != 0) {
		ASYNCIO_SYSERROR("pipe");
		goto error_fd_map;
	}

	if (set_nonblocking(pipefds[0]) != 0 || set_nonblocking(pipefds[1]) != 0)
		goto error_pipefds;

	/* These are unused for the wake event */
	queue_init(&worker_info->callbacks[0]);
	init_fdevent_refcount_locked(&worker_info->fdevent_refcounts[0]);

	/* Add wake event for poll interruption when a change occured in pollfds */
	worker_info->fds[0].fd = pipefds[0];
	worker_info->fds[0].events = POLLIN;
	worker_info->nfds = 1;
	worker_info->changed = 0;
	worker_info->wakefd = pipefds[1];
	worker_info->clearwakefd = pipefds[0];

	return 0;

error_pipefds:
	if (close(pipefds[0]) != 0)
		ASYNCIO_SYSERROR("close");

	if (close(pipefds[1]) != 0)
		ASYNCIO_SYSERROR("close");

error_fd_map:
	hashtable_destroy(&worker_info->fd_map);

error:
	return -1;
}

static void destroy_fdevents_worker(struct fdevents_worker_info *worker_info)
{
	if (close(worker_info->wakefd) != 0)
		ASYNCIO_SYSERROR("close");

	if (close(worker_info->clearwakefd) != 0)
		ASYNCIO_SYSERROR("close");

	hashtable_destroy(&worker_info->fd_map);
}

static int insert_fdevents_locked(struct fdevents_worker_info *worker_info, struct fdevent_handle *handle)
{
	decl_queue(struct fdevent_handle, *cbqueue);
	size_t index;

	/* The fd is already in the database, just add handle to queue and increment event refcounts */
	if (hashtable_lookup(&worker_info->fd_map, sizeof handle->fd, &handle->fd, (const void **)&cbqueue)) {
		index = ((unsigned char *)cbqueue - (unsigned char *)&worker_info->callbacks[0]) / sizeof *cbqueue;

		/* Too many events in this fdevent callback queue */
		if (increment_fdevent_refcount_locked(&worker_info->fdevent_refcounts[index], handle->events) != 0)
			return -1;

		queue_push(cbqueue, handle);
		worker_info->fds[index].events |= handle->events;
	} else {
		if (worker_info->nfds == MAX_POLLFDS) {
			ASYNCIO_ERROR("Worker nfds reached MAX_POLLFDS.\n");
			return -1;
		}

		cbqueue = (void *)&worker_info->callbacks[worker_info->nfds];

		if (hashtable_insert(&worker_info->fd_map, sizeof handle->fd, &handle->fd, cbqueue) != 0) {
			ASYNCIO_ERROR("Failed to insert fd in hashtable.\n");
			return -1;
		}

		init_fdevent_refcount_locked(&worker_info->fdevent_refcounts[worker_info->nfds]);

		if (increment_fdevent_refcount_locked(&worker_info->fdevent_refcounts[worker_info->nfds], handle->events) != 0) {
			ASYNCIO_ERROR("Failed to increment fdevent refcount.\n");
			hashtable_delete(&worker_info->fd_map, sizeof handle->fd, &handle->fd);
			return -1;
		}

		queue_init(cbqueue);
		queue_push(cbqueue, handle);

		worker_info->fds[worker_info->nfds].fd = handle->fd;
		worker_info->fds[worker_info->nfds].events = handle->events;
		++worker_info->nfds;
	}

	return 0;
}

static void remove_fdevents_locked(struct fdevents_worker_info *worker_info, struct fdevent_handle *handle)
{
	decl_queue(struct fdevent_handle, *cbqueue);
	size_t index;
	nfds_t last;

	if (!hashtable_lookup(&worker_info->fd_map, sizeof handle->fd, &handle->fd, (const void **)&cbqueue)) {
		ASYNCIO_ERROR("Tried to remove handle not in the worker_info queue.\n");
		return;
	}

	last = worker_info->nfds - 1;
	index = ((unsigned char *)cbqueue - (unsigned char *)&worker_info->callbacks[0]) / sizeof *cbqueue;
	decrement_fdevent_refcount_locked(&worker_info->fdevent_refcounts[index], handle->events);
	queue_remove(cbqueue, handle);

	if (queue_empty(cbqueue)) {
		hashtable_delete(&worker_info->fd_map, sizeof handle->fd, &handle->fd);
		worker_info->fds[index] = worker_info->fds[last];
		worker_info->callbacks[index] = worker_info->callbacks[last];
		worker_info->fdevent_refcounts[index] = worker_info->fdevent_refcounts[last];
		hashtable_modify(&worker_info->fd_map, sizeof worker_info->fds[last].fd, &worker_info->fds[last].fd, cbqueue); /* This can't fail */
		--worker_info->nfds;
	} else {
		worker_info->fds[index].events = get_eventsmask_from_fdevent_refcount_locked(&worker_info->fdevent_refcounts[index]);
	}
}

static void copy_fdevents_locked(const struct fdevents_worker_info *worker_info, struct pollfd *fds, nfds_t *nfdsp)
{
	nfds_t i;

	for (i = 0; i < worker_info->nfds; i++)
		fds[i] = worker_info->fds[i];

	*nfdsp = worker_info->nfds;
}

static void clear_changed_fdevents_locked(struct fdevents_worker_info *worker_info)
{
	worker_info->changed = 0;
}

static int set_changed_fdevents_locked(struct fdevents_worker_info *worker_info)
{
	int wake;

	if (!has_changed_fdevents_locked(worker_info))
		wake = 1;
	else
		wake = 0;

	worker_info->changed = 1;

	/* Avoid possible && side-effects */
	if (wake) {
		if (wake_fdevents_worker_locked(worker_info) != 0)
			return -1;
	}

	return 0;
}

static int has_changed_fdevents_locked(struct fdevents_worker_info *worker_info)
{
	return worker_info->changed;
}

static int wake_fdevents_worker_locked(struct fdevents_worker_info *worker_info)
{
	unsigned char dummy;

	dummy = 0;

	if (write(worker_info->wakefd, &dummy, sizeof dummy) != sizeof dummy) {
		ASYNCIO_SYSERROR("write");
		return -1;
	}

	return 0;
}

static int clearwake_fdevents_worker_locked(struct fdevents_worker_info *worker_info)
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

static uint32_t get_threadpool_flags(uint32_t fdevents_flags)
{
	uint32_t flags;

	/* Do not use ASYNCHRONOUS CANCELLATIONS here, because we don't want
	 * execute_fdevent_callback to be async-cancellable. It will set the user
	 * desired cancellation state/type there. */
	flags = THREADPOOL_FLAG_NONE;

	if (fdevents_flags & FDEVENT_FLAG_CONTRACTOR)
		flags |= THREADPOOL_FLAG_CONTRACTOR;
	if (fdevents_flags & FDEVENT_FLAG_CANCELLABLE)
		flags |= THREADPOOL_FLAG_CANCELLABLE;

	return flags;
}

#define SET_IF_SET(e1, f1, e2, f2) do {if ((e2) & (f2)) { (e1) |= (f1); }} while (0)

static uint16_t to_fdevents_events(short poll_events)
{
	uint16_t events;

	events = 0;
	SET_IF_SET(events, FDEVENT_EVENT_READ, poll_events, POLLIN | POLLPRI);
	SET_IF_SET(events, FDEVENT_EVENT_WRITE, poll_events, POLLOUT);
	SET_IF_SET(events, FDEVENT_EVENT_ERROR, poll_events, POLLERR | POLLHUP | POLLNVAL);

	return events;
}

static short to_poll_events(uint16_t events)
{
	short poll_events;

	poll_events = 0;
	SET_IF_SET(poll_events, POLLIN | POLLPRI, events, FDEVENT_EVENT_READ);
	SET_IF_SET(poll_events, POLLOUT, events, FDEVENT_EVENT_WRITE);
	SET_IF_SET(poll_events, POLLERR | POLLHUP | POLLNVAL, events, FDEVENT_EVENT_ERROR);

	return poll_events;
}

static void fdevent_threadpool_cancelled(void *arg)
{
	struct fdevent_handle *handle;

	handle = (struct fdevent_handle *)arg;

	printf("Cancelled.\n");

	/* Release threadpool's reference to handle */
	fdevent_release_handle(handle);
	/* XXX But threadpool doesn't call cancelled callback if it never left threadpool queue? */
}

static void execute_fdevent_callback(void *arg)
{
	struct fdevent_handle *handle;
	int continued;
	int oldstate;
	int oldtype;

	continued = 0;
	handle = (struct fdevent_handle *)arg;

	/* Set user-defined cancellation settings */
	if (handle->flags & FDEVENT_FLAG_ASYNCCANCEL)
		set_canceltype(CANCEL_ASYNCHRONOUS, &oldtype);
	else
		set_canceltype(CANCEL_DEFERRED, &oldtype);

	if (handle->flags & FDEVENT_FLAG_CANCELLABLE)
		set_cancelstate(CANCEL_ENABLE, &oldstate);
	else
		set_cancelstate(CANCEL_DISABLE, &oldstate);

	/* Execute the callback */
	handle->callback_fn(handle->fd, to_fdevents_events(handle->revents), handle->callback_arg, &continued);

	restore_cancelstate(oldstate);
	restore_canceltype(oldtype);

	/* XXX BETTER IDEA!!! JUST DO THAT IN A WHILE LOOP. WE'RE IN A THREAD ALREADY, SO WHY NOT JUST KEEP POLLING? */
	/* No, better: If it's a worker thread, just launch redispacher in completed_callback (since worker thread keeps going,
	 * and completed callback gets called right before it goes back in the loop). If it's a contractor thread, then
	 * keep looping in the thread since it's there already... But, that gives issue if many many continued contractors, all of them waiting.
	 * We would have 100000 threads doing nothing but waiting, instead of 1 thread (the event loop) */
	if (continued) {
		printf("continued\n");
		/* Release threadpool handle? Set has_threadpool_handle to 0. Will need a lock... */
		/* XXX Need to join on old threadpool handle, release old threadpool handle, set has_threadpool_handle to 0.
		 * Do this by launching a redispatcher thread (we can't do it ourselves since we are the thread we want to join...) */
		/* Acquire reference for next worker dispatch inside launch function... */
		if (launch_redispatcher_thread(handle) != 0)
			ASYNCIO_ERROR("Failed to launch redispatcher thread for continue.\n");
	} else {
		printf("not continued\n");
	}

	/* Release threadpool's reference to handle */
	fdevent_release_handle(handle);
}

static void fdevents_loop(void *arg)
{
	threadpool_handle_t threadpool_handle, next;
	struct threadpool_dispatch_info threadpool_info;
	struct fdevents_worker_info *worker_info;
	struct fdevent_handle *handle;
	struct pollfd fds[MAX_POLLFDS];
	nfds_t nfds;
	nfds_t i;

	worker_info = (struct fdevents_worker_info *)arg;

	if (lock_fdevents_mtx() != 0) {
		ASYNCIO_ERROR("Failed to lock fdevents mtx.\n");
		return;
	}

	copy_fdevents_locked(worker_info, fds, &nfds);
	unlock_fdevents_mtx();

	for (;;) {
		if (poll(fds, nfds, -1) < 0) {
			ASYNCIO_SYSERROR("poll");
			break;
		}

		if (lock_fdevents_mtx() != 0) {
			ASYNCIO_ERROR("Failed to lock fdevents mtx.\n");
			break;
		}

		/* Stop condition for fdevents loop. */
		if (fdevents_stopped) {
			unlock_fdevents_mtx();
			break;
		}

		if (has_changed_fdevents_locked(worker_info)) {
			copy_fdevents_locked(worker_info, fds, &nfds);

			if (clearwake_fdevents_worker_locked(worker_info) != 0) {
				ASYNCIO_ERROR("Failed to clear wake event.\n");
				unlock_fdevents_mtx();
				break;
			}

			clear_changed_fdevents_locked(worker_info);
			unlock_fdevents_mtx();
			continue;
		}

		/* Scan for events */
		for (i = 0; i < nfds; i++) {
			if (fds[i].revents & (fds[i].events | POLLERR | POLLHUP | POLLNVAL)) {
				queue_foreach(&worker_info->callbacks[i], handle, next) {
					next = handle->next; /* The handle might get removed and the next pointer overwritten otherwise */

					if (fds[i].revents & (handle->events | POLLERR | POLLHUP | POLLNVAL)) {
						/* ADD TO DISPATCHER QUEUE */

						/* At this point the handle refcount is at least 2:
						 * - the client thread that got the handle
						 * - the fdevents_loop worker */

						/* The callback queues are owned by the fdevents_loop, this is the only
						 * thread that is allowed to remove handles from queues. It should not
						 * be done during an fdevent_release_handle */

						/* XXX Ideally do not call threadpool_dispatch here because it calls malloc.
						 * Instead, put the handle in a list and dispatch all of them after the loop,
						 * and after unlocking the mutex. Or maybe use a lock-free queue?
						 * First pass (locked): just make a copy of the callback queues.
						 * Second pass (unlocked): with the copies, actually process and dispatch threadpool, etc.
						 * Could use different links for next and prev in the copy, to avoid overwriting ->next and ->prev,
						 * allowing us to only store those that are actually fired off events */
						threadpool_info.flags = get_threadpool_flags(handle->flags);
						threadpool_info.dispatch_info.fn = execute_fdevent_callback;
						threadpool_info.dispatch_info.arg = handle;
						threadpool_info.completed_info.cb = fdevent_threadpool_completed;
						threadpool_info.completed_info.arg = handle;
						threadpool_info.cancelled_info.cb = fdevent_threadpool_cancelled;
						threadpool_info.cancelled_info.arg = handle;

						if (threadpool_dispatch(&threadpool_info, &threadpool_handle) != 0) {
							ASYNCIO_ERROR("Failed to dispatch fdevent handle to threadpool.\n");
							fdevent_release_handle(handle); /* threadpool won't have the handle after all... */
							continue;
						}

						handle->revents = fds[i].revents;
						handle->threadpool_handle = threadpool_handle;
						handle->has_threadpool_handle = 1;

						remove_fdevents_locked(worker_info, handle);
						handle->in_worker_database = 0;
					}
				}
			}
		}

		/* The masked fds must be removed from the local fds array because poll
		 * will return on exceptions even when events is 0. */
		copy_fdevents_locked(worker_info, fds, &nfds);
		unlock_fdevents_mtx();
	}
}

static int dispatch_handle_to_loop(struct fdevent_handle *handle)
{
	if (lock_fdevents_mtx() != 0) {
		ASYNCIO_ERROR("Failed to lock fdevents mtx.\n");
		return -1;
	}

	if (insert_fdevents_locked(&fdevents_global_worker_info, handle) != 0) {
		unlock_fdevents_mtx();
		return -1;
	}

	handle->in_worker_database = 1;
	set_changed_fdevents_locked(&fdevents_global_worker_info);

	unlock_fdevents_mtx();
	return 0;
}

static int redispatch_handle_to_loop(struct fdevent_handle *handle)
{
	/*struct threadpool_dispatch_info fdevents_task; */

	/* XXX PROBLEM: WHO'S GONNA JOIN ON THIS THREAD??? */
	/* Need a dedicated redispatcher thread? */
	/* I mean, past threadpool handle would have gotten overwritten othrewise!!???!? */
	if (lock_fdevents_mtx() != 0) {
		ASYNCIO_ERROR("Failed to lock fdevents mtx.\n");
		return -1;
	}

	if (insert_fdevents_locked(&fdevents_global_worker_info, handle) != 0) {
		unlock_fdevents_mtx();
		return -1;
	}

	handle->in_worker_database = 1;

	set_changed_fdevents_locked(&fdevents_global_worker_info);

	unlock_fdevents_mtx();
	return 0;
}

int fdevent_init()
{
	struct threadpool_dispatch_info fdevents_task;
	int oldstate;

	disable_cancellations(&oldstate);

	if (lock_initializer_wrlock() != 0) {
		ASYNCIO_ERROR("Failed to lock initializer wrlock.\n");
		restore_cancelstate(oldstate);
		return -1;
	}

	if (lock_fdevents_mtx() != 0) {
		ASYNCIO_ERROR("Failed to lock fdevents mtx.\n");
		unlock_initializer_lock();
		restore_cancelstate(oldstate);
		return -1;
	}

	if (fdevents_initialized) {
		unlock_fdevents_mtx();
		unlock_initializer_wrlock();
		restore_cancelstate(oldstate);
		return 0;
	}

	if (init_fdevents_worker(&fdevents_global_worker_info) != 0) {
		unlock_fdevents_mtx();
		unlock_initializer_wrlock();
		restore_cancelstate(oldstate);
		return -1;
	}

	fdevents_task.flags = THREADPOOL_FLAG_CONTRACTOR;
	fdevents_task.dispatch_info.fn = fdevents_loop;
	fdevents_task.dispatch_info.arg = &fdevents_global_worker_info;
	fdevents_task.completed_info.cb = NULL;
	fdevents_task.completed_info.arg = NULL;
	fdevents_task.cancelled_info.cb = NULL;
	fdevents_task.cancelled_info.arg = NULL;

	if (threadpool_dispatch(&fdevents_task, &fdevents_worker_threadpool_handle) != 0) {
		ASYNCIO_ERROR("Failed to dispatch fdevents loop.\n");
		destroy_fdevents_worker(&fdevents_global_worker_info);
		unlock_fdevents_mtx();
		unlock_initializer_lock();
		restore_cancelstate(oldstate);
		return -1;
	}

	fdevents_initialized = 1;
	unlock_fdevents_mtx();
	unlock_initializer_lock();
	restore_cancelstate(oldstate);
	return 0;
}

int fdevent_register(struct fdevent_info *evinfo, fdevent_handle_t *fdhandle)
{
	struct fdevent_handle *handle;
	int oldstate;

	disable_cancellations(&oldstate);

	if (evinfo->cb == NULL) {
		restore_cancelstate(oldstate);
		return -1;
	}

	if (set_nonblocking(evinfo->fd) != 0) {
		restore_cancelstate(oldstate);
		return -1;
	}

	handle = safe_malloc(sizeof *handle);

	if (handle == NULL) {
		restore_cancelstate(oldstate);
		return -1;
	}

	if (init_fdevent_handle(handle, evinfo) != 0) {
		safe_free(handle);
		restore_cancelstate(oldstate);
		return -1;
	}

	/* The caller must have a reference by default to prevent race conditions
	 * where the event completes before the client had a chance to acquire the
	 * handle. Also the fdevents loop thread awakened must have a reference to
	 * prevent the case where the client releases its handle before the loop
	 * manages to acquire its handle */
	handle->refcount = 2;

	if (dispatch_handle_to_loop(handle) != 0) {
		cleanup_fdevent_handle(handle);
		safe_free(handle);
		restore_cancelstate(oldstate);
		return -1;
	}

	*fdhandle = handle;
	restore_cancelstate(oldstate);
	return 0;
}

/* XXX Do something like in threadpool_join, need to wait until thread really has finished,
 * so call threadpool_join and for continued events, do that in a while loop? */
int fdevent_join(fdevent_handle_t fdhandle)
{
	struct fdevent_handle *handle;
	int oldstate;
	int oldtype;
	int success = 1;
	int rc;

	disable_cancellations(&oldstate);

	if (lock_fdevents_mtx() != 0) {
		ASYNCIO_ERROR("Failed to lock fdevents mtx.\n");
		restore_cancelstate(oldstate);
		return -1;
	}

	handle = (struct fdevent_handle *)fdhandle;

	/* Unlock the fdevents_mtx in cleanup handler if cancelled here */
	pthread_cleanup_push(unlock_fdevents_mtx_cleanup, NULL);

	/* Restore cancelstate while waiting for condition variable
	 * to allow cancellation in this case. But set cancellation type to DEFERRED
	 * in order to make sure we cancel during pthread_cond_wait, which should guarantee
	 * that the fdevents_mtx is locked during the cleanup handler. */
	set_canceltype(PTHREAD_CANCEL_DEFERRED, &oldtype);
	restore_cancelstate(oldstate);

	while (!(handle->finished)) {
		if ((rc = pthread_cond_wait(&handle->finished_cond, &fdevents_mtx)) != 0) {
			errno = rc;
			ASYNCIO_SYSERROR("pthread_cond_wait");
			success = 0;
			break;
		}
	}

	disable_cancellations(&oldstate);
	restore_canceltype(oldtype);

	/* Unlock fdevents mutex. */
	pthread_cleanup_pop(1);

	restore_cancelstate(oldstate);

	if (success)
		return 0;
	else
		return -1;
}

int fdevent_cancel(fdevent_handle_t fdhandle)
{
	struct fdevent_handle *handle;
	int oldstate;

	disable_cancellations(&oldstate);

	handle = (struct fdevent_handle *)fdhandle;

	if (lock_fdevents_mtx() != 0) {
		ASYNCIO_ERROR("Failed to lock fdevents mtx.\n");
		restore_cancelstate(oldstate);
		return -1;
	}

	if (handle->in_worker_database) {
		/* Means it has not yet been dispatched to threadpool, so worker won't have access to handle anymore.
		 * Note that if it was in a thread, it will have access due to the cancelled callback. */
		remove_fdevents_locked(&fdevents_global_worker_info, handle);
		set_changed_fdevents_locked(&fdevents_global_worker_info);
		printf("kartel\n");
	}

	unlock_fdevents_mtx();

	/* XXX If we cancel an event that is continued, both in_worker_database and has_threadpool_handle = 1...
	 * This also prevents us from going in the notify (else), and so handle is never indicated as finished, because
	 * the cancelled threadpool handle refers to an old task that already finished. */
	/* XXX Need to lock this!!! */
	if (handle->has_threadpool_handle) {
		if (threadpool_cancel(handle->threadpool_handle) != 0) {
			ASYNCIO_ERROR("Failed to cancel threadpool handle.\n");
			restore_cancelstate(oldstate);
			return -1;
		}
		printf("Pfouaregom\n");
	} else {
		printf("cancelled notify.\n");
		if (notify_fdevent_handle_finished(handle) != 0) {
			ASYNCIO_ERROR("Failed to notify fdevent handle finished.\n");
			restore_cancelstate(oldstate);
			return -1;
		}
	}

	restore_cancelstate(oldstate);
	return 0;
}

int fdevent_acquire_handle(fdevent_handle_t fdhandle)
{
	struct fdevent_handle *handle;
	int oldstate;

	disable_cancellations(&oldstate);

	handle = (struct fdevent_handle *)fdhandle;

	if (lock_fdevents_mtx() != 0) {
		ASYNCIO_ERROR("Failed to lock fdevents mtx.\n");
		restore_cancelstate(oldstate);
		return -1;
	}

	if (reference_fdevent_handle_locked(handle) != 0) {
		unlock_fdevents_mtx();
		restore_cancelstate(oldstate);
		return -1;
	}

	unlock_fdevents_mtx();
	restore_cancelstate(oldstate);
	return 0;
}

void fdevent_release_handle(fdevent_handle_t fdhandle)
{
	struct fdevent_handle *handle;
	int oldstate;

	disable_cancellations(&oldstate);

	handle = (struct fdevent_handle *)fdhandle;

	if (lock_fdevents_mtx() != 0) {
		ASYNCIO_ERROR("Failed to lock fdevents mtx.\n");
		restore_cancelstate(oldstate);
		return;
	}

	dereference_fdevent_handle_locked(handle);

	if (handle->refcount == 0) {
		unlock_fdevents_mtx();
		cleanup_fdevent_handle(handle);
		safe_free(handle);
	} else {
		unlock_fdevents_mtx();
	}

	restore_cancelstate(oldstate);
}
