#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "fdevents.h"
#include "threadpool.h"
#include "cancellations.h"
#include "queue.h"
#include "hashtable.h"
#include "safe_malloc.h"
#include "logging.h"
#include "compile_time_assert.h"

#define MAX_POLLFDS			10000
#define POLLFD_EVENT_NBITS		(CHAR_BIT * sizeof (short))

#define CALLBACK_QUEUE_ID		0
#define ALL_FDEVENT_HANDLES_QUEUE_ID	1
#define NUM_FDEVENT_QUEUES		2

/* STRUCT DEFINITIONS */
struct event_handle {
	int type;
	uint32_t flags;

	int in_worker_database;
	int has_threadpool_handle;
	threadpool_handle_t threadpool_handle;

	int continued;					/* Redispatch to event loop. Is like an argument to the cancelled/completed callback, so no mutex needed. */

	int finished;
	pthread_cond_t finished_cond;

	unsigned int refcount;

	/* Used for event callback queues */
	/* We need to be able to put handle in multiple queues */
	struct event_handle *prev[NUM_EVENT_QUEUES];
	struct event_handle *next[NUM_EVENT_QUEUES];
};

struct backend_ops {
	int (*init_eventloop)(void);
	int (*wait_for_events)(void);
};
/* END STRUCT DEFINITIONS */

/* PROTOTYPES */
static int lock_events_mtx(void);
static void unlock_events_mtx(void);
static void unlock_events_mtx_cleanup(void *arg);

static int lock_initialization_rdlock(void);
static int lock_initialization_wrlock(void);
static void unlock_initialization_lock(void);

static int init_fdevent_handle(struct fdevent_handle *handle, const struct fdevent_info *evinfo);
static void cleanup_fdevent_handle(struct fdevent_handle *handle);
static int reference_fdevent_handle_locked(struct fdevent_handle *handle);
static void dereference_fdevent_handle_locked(struct fdevent_handle *handle);

static void notify_fdevent_handle_finished(struct fdevent_handle *handle);
static void fatal_error_on_handle(struct fdevent_handle *handle);

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

static void fdevent_threadpool_completed(void *arg);
static void execute_fdevent_callback(void *arg);

static void fdevents_loop(void *arg);
static void signal_stop_everything(void);

static int dispatch_handle_to_loop(struct fdevent_handle *handle);
/* END PROTOTYPES */

/* GLOBALS */
static pthread_mutex_t events_mtx = PTHREAD_MUTEX_INITIALIZER;
static int fdevents_stopped = 0;

static pthread_rwlock_t initialization_lock = PTHREAD_RWLOCK_INITIALIZER;
static int events_initialized = 0;

/* A global list of all handles for cleanup */
static decl_queue(struct fdevent_handle, all_fdevent_handles_queue);

/* NOTE: Whenever there is a fdevent_release_handle in this module, handle must also be removed from the all_fdevent_handles_queue.
 * We do this in the notify_fdevent_handle_finished, which basically ends the handle's lifecycle through the fdevent module. */
static decl_queue(struct backend_ops, all_event_backends);
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

static int lock_initialization_rdlock()
{
	int rc;

	if ((rc = pthread_rwlock_rdlock(&initialization_lock)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_rwlock_rdlock");
		return -1;
	}

	return 0;
}

static int lock_initialization_wrlock()
{
	int rc;

	if ((rc = pthread_rwlock_wrlock(&initialization_lock)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_rwlock_wrlock");
		return -1;
	}

	return 0;
}

static void unlock_initialization_lock()
{
	int rc;

	if ((rc = pthread_rwlock_unlock(&initialization_lock)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_rwlock_unlock");
	}
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

	handle->do_not_dispatch_got_cancelled = 0;
	handle->continued = 0;

	handle->refcount = 0;
	handle->finished = 0;

	if ((rc = pthread_cond_init(&handle->finished_cond, NULL)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_cond_init");
		return -1;
	}

	return 0;
}

static void cleanup_fdevent_handle(struct fdevent_handle *handle)
{
	int rc;

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

static void notify_fdevent_handle_finished(struct fdevent_handle *handle)
{
	int rc;

	/* This function is responsible for ending the handle life cycle through the fdevent module.
	 * A reference may still be held by users, but it stops existing as far as this module is concerned. */

	if (lock_fdevents_mtx() == 0) {
		queue_remove(&all_fdevent_handles_queue, handle);
		handle->finished = 1;

		/* Yes, other threads may still try to call fdevent_cancel, but that function
		 * will know not to touch the threadpool handle since we clear the has_threadpool_handle flag. */
		if (handle->has_threadpool_handle) {
			threadpool_release_handle(handle->threadpool_handle);
			handle->has_threadpool_handle = 0;
		}

		if ((rc = pthread_cond_broadcast(&handle->finished_cond)) != 0) {
			errno = rc;
			ASYNCIO_SYSERROR("pthread_cond_broadcast");
		}

		unlock_fdevents_mtx();
	} else {
		ASYNCIO_ERROR("Failed to lock fdevents mtx.\n");
	}

	fdevent_release_handle(handle);
}

static void fatal_error_on_handle(struct fdevent_handle *handle)
{
	/* TODO: add error conditions? */
	notify_fdevent_handle_finished(handle);
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
	queue_init(&worker_info->callbacks[0], CALLBACK_QUEUE_ID);
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

		queue_init(cbqueue, CALLBACK_QUEUE_ID);
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

static void fdevent_threadpool_completed(void *arg)
{
	struct fdevent_handle *handle;

	handle = (struct fdevent_handle *)arg;

	/* TODO: More fine grained lock? */
	if (lock_fdevents_mtx() != 0) {
		ASYNCIO_ERROR("Failed to lock fdevents mtx.\n");
		fatal_error_on_handle(handle);
		return;
	}

	/* No danger of double release of handle (in notify_fdevent_handle_finished)
	 * because it checks whether there actually is a threadpool handle.
	 * But the point is that we want to release the old threadpool handle even if
	 * the fdevent handle's lifecycle is not done, in case it is continued. */
	threadpool_release_handle(handle->threadpool_handle);
	handle->has_threadpool_handle = 0;

	if (handle->continued) {
		handle->continued = 0;

		if (insert_fdevents_locked(&fdevents_global_worker_info, handle) != 0) {
			ASYNCIO_ERROR("Failed to re-dispatch handle to loop.\n");
			unlock_fdevents_mtx();
			fatal_error_on_handle(handle);
			return;
		}

		handle->in_worker_database = 1;
		set_changed_fdevents_locked(&fdevents_global_worker_info);
		unlock_fdevents_mtx();
	} else {
		unlock_fdevents_mtx();
		notify_fdevent_handle_finished(handle);
	}
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

	handle->continued = continued;
}

static void eventloop(struct backend_ops *backend)
{
	if (backend->init_eventloop() != 0)
		return;

	for (;;) {
		if (backend->wait_for_events() != 0)
			break;

		if (backend->has_stopped())
			break;

		backend->process_events();
	}

	backend->cleanup_eventloop();
}

static void stop_all_backends()
{
	struct backend_ops *backend;

	if (lock_events_mtx() != 0) {
		ASYNCIO_ERROR("Failed to lock events mtx.\n");
		return;
	}

	queue_foreach(&all_event_backends, backend, backend->next[BACKEND_QUEUE_ID])
		backend->stop();

	unlock_events_mtx();
}

static int dispatch_handle_to_loop(struct event_handle *handle)
{
	if (lock_events_mtx() != 0) {
		ASYNCIO_ERROR("Failed to lock events mtx.\n");
		return -1;
	}

	if (
	if (insert_fdevents_locked(&fdevents_global_worker_info, handle) != 0) {
		unlock_fdevents_mtx();
		return -1;
	}

	handle->in_worker_database = 1;
	set_changed_fdevents_locked(&fdevents_global_worker_info);

	queue_push(&all_fdevent_handles_queue, handle);

	unlock_fdevents_mtx();
	return 0;
}

int event_init(struct event_worker *worker)
{
	struct threadpool_dispatch_info events_task;
	int oldstate;

	disable_cancellations(&oldstate);

	if (lock_initialization_wrlock() != 0) {
		ASYNCIO_ERROR("Failed to lock initialization wrlock.\n");
		restore_cancelstate(oldstate);
		return -1;
	}

	if (lock_events_mtx() != 0) {
		ASYNCIO_ERROR("Failed to lock events mtx.\n");
		unlock_initialization_lock();
		restore_cancelstate(oldstate);
		return -1;
	}

	if (events_initialized) {
		unlock_events_mtx();
		unlock_initialization_lock();
		restore_cancelstate(oldstate);
		return 0;
	}

	if (threadpool_init() != 0) {
		ASYNCIO_ERROR("Failed to initialize threadpool module.\n");
		unlock_events_mtx();
		unlock_initialization_lock();
		restore_cancelstate(oldstate);
		return -1;
	}

	if (init_events_worker(&fdevents_global_worker_info) != 0) {
		ASYNCIO_ERROR("Failed to initialize fdevents worker.\n");
		threadpool_cleanup();
		unlock_fdevents_mtx();
		unlock_initialization_lock();
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
		threadpool_cleanup();
		unlock_fdevents_mtx();
		unlock_initialization_lock();
		restore_cancelstate(oldstate);
		return -1;
	}

	queue_init(&all_fdevent_handles_queue, ALL_FDEVENT_HANDLES_QUEUE_ID);

	fdevents_stopped = 0;
	fdevents_initialized = 1;

	unlock_fdevents_mtx();
	unlock_initialization_lock();
	restore_cancelstate(oldstate);
	return 0;
}

int fdevent_register(struct fdevent_info *evinfo, fdevent_handle_t *fdhandle)
{
	struct fdevent_handle *handle;
	int oldstate;

	disable_cancellations(&oldstate);

	if (lock_initialization_rdlock() != 0) {
		ASYNCIO_ERROR("Failed to lock initialization rdlock.\n");
		restore_cancelstate(oldstate);
		return -1;
	}

	if (!fdevents_initialized) {
		unlock_initialization_lock();
		restore_cancelstate(oldstate);
		return -1;
	}

	if (evinfo->cb == NULL) {
		unlock_initialization_lock();
		restore_cancelstate(oldstate);
		return -1;
	}

	if (set_nonblocking(evinfo->fd) != 0) {
		unlock_initialization_lock();
		restore_cancelstate(oldstate);
		return -1;
	}

	handle = safe_malloc(sizeof *handle);

	if (handle == NULL) {
		unlock_initialization_lock();
		restore_cancelstate(oldstate);
		return -1;
	}

	if (init_fdevent_handle(handle, evinfo) != 0) {
		safe_free(handle);
		unlock_initialization_lock();
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
		unlock_initialization_lock();
		restore_cancelstate(oldstate);
		return -1;
	}

	unlock_initialization_lock();

	*fdhandle = handle;
	restore_cancelstate(oldstate);
	return 0;
}

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
	int success = 1;
	int notify = 0;
	int oldstate;

	disable_cancellations(&oldstate);

	handle = (struct fdevent_handle *)fdhandle;

	if (lock_initialization_rdlock() != 0) {
		ASYNCIO_ERROR("Failed to lock initialization rdlock.\n");
		restore_cancelstate(oldstate);
		return -1;
	}

	if (lock_fdevents_mtx() != 0) {
		ASYNCIO_ERROR("Failed to lock fdevents mtx.\n");
		unlock_initialization_lock();
		restore_cancelstate(oldstate);
		return -1;
	}

	if (!fdevents_initialized) {
		unlock_fdevents_mtx();
		unlock_initialization_lock();
		restore_cancelstate(oldstate);
		return -1;
	}

	if (handle->in_worker_database) {
		/* Should never happen, but just putting this here to detect potential bugs... */
		if (handle->has_threadpool_handle)
			ASYNCIO_ERROR("This should never happen. The fdevent handle is in worker database and has threadpool handle.\n");

		/* Means it has not yet been dispatched to threadpool, so worker won't have access to handle anymore.
		 * Note that if it was in a thread, it will have access due to the cancelled callback. */
		remove_fdevents_locked(&fdevents_global_worker_info, handle);
		set_changed_fdevents_locked(&fdevents_global_worker_info);
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

	unlock_fdevents_mtx();

	if (notify)
		notify_fdevent_handle_finished(handle);

	unlock_initialization_lock();
	restore_cancelstate(oldstate);

	if (success)
		return 0;
	else
		return -1;
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

void fdevent_cleanup()
{
	struct fdevent_handle *handle;
	int oldstate;

	disable_cancellations(&oldstate);

	if (lock_initialization_wrlock() != 0) {
		ASYNCIO_ERROR("Failed to lock initialization wrlock.\n");
		restore_cancelstate(oldstate);
		return;
	}

	if (fdevents_initialized) {
		unlock_initialization_lock();
		restore_cancelstate(oldstate);
		return;
	}

	fdevents_initialized = 0;
	signal_stop_everything();

	if (threadpool_join(fdevents_worker_threadpool_handle) != 0)
		ASYNCIO_ERROR("Failed to join on fdevents_worker_threadpool_handle.\n");

	threadpool_release_handle(fdevents_worker_threadpool_handle);

	/* Release all fdevent handles */
	while (!queue_empty(&all_fdevent_handles_queue)) {
		queue_pop(&all_fdevent_handles_queue, &handle);

		if (handle->has_threadpool_handle) {
			if (threadpool_join(handle->threadpool_handle) != 0)
				ASYNCIO_ERROR("Failed to join fdevent handle's threadpool handle.\n");
		}

		notify_fdevent_handle_finished(handle);
	}

	threadpool_cleanup();
	unlock_initialization_lock();
	restore_cancelstate(oldstate);
}
