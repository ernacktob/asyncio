#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>

#include "fdevents_priv.h"
#include "events.h"
#include "asyncio_threadpool.h"
#include "queue.h"
#include "threading.h"
#include "logging.h"
#include "safe_malloc.h"
#include "constants.h"
#include "compile_time_assert.h"

/* PROTOTYPES */
static int set_nonblocking(int fd);

static uint16_t to_fdevents_events(short poll_events);
static short to_poll_events(uint16_t events);

static void copy_fdevents_scratch_locked(struct asyncio_fdevents_loop_priv *fdeventloop_priv);

static void init_fdevents_refcount_locked(struct fdevents_refcount *refcounts);
static int increment_fdevents_refcount_locked(struct fdevents_refcount *refcounts, short events);
static void decrement_fdevents_refcount_locked(struct fdevents_refcount *refcounts, short events);
static short get_eventsmask_from_fdevents_refcount_locked(const struct fdevents_refcount *refcounts);

static int fdevents_insert_handle_locked(void *eventloop_instance, void *handle_instance);
static void fdevents_remove_handle_locked(void *eventloop_instance, void *handle_instance);

static void fdevents_cleanup_events_handle(void *instance);

static int fdevents_handle_wait(const struct asyncio_fdevents_handle *handle); /* PUBLIC */
static int fdevents_handle_cancel(const struct asyncio_fdevents_handle *handle); /* PUBLIC */
static int fdevents_handle_acquire(const struct asyncio_fdevents_handle *handle); /* PUBLIC */
static void fdevents_handle_release(const struct asyncio_fdevents_handle *handle); /* PUBLIC */

static void fdevents_handle_callback(void *handle_instance, uint32_t flags, int *continued);

static int fdevents_initialize_eventloop_thread(void *instance);
static int fdevents_wait_for_events(void *instance);
static void fdevents_process_changed_events_locked(void *instance);
static void fdevents_scan_for_events_locked(void *instance);
static void fdevents_continue_eventloop_thread_locked(void *instance);
static void fdevents_cleanup_eventloop_thread(void *instance);

static int fdevents_eventloop_listen(struct asyncio_fdevents_loop *fdeventloop, const struct asyncio_fdevents_info *info, struct asyncio_fdevents_handle **handlep); /* PUBLIC */
static int fdevents_eventloop_acquire(const struct asyncio_fdevents_loop *fdeventloop); /* PUBLIC */
static void fdevents_eventloop_release(const struct asyncio_fdevents_loop *fdeventloop); /* PUBLIC */

static int init_fdeventloop_private_data(const struct asyncio_fdevents_options *options, struct asyncio_fdevents_loop_priv *fdeventloop_priv);
static void fdevents_wakeup_eventloop_locked(void *instance);
static void fdevents_clearwake_eventloop_locked(void *instance);
static void fdevents_cleanup_eventloop(void *instance);
/* END PROTOTYPES */

/* GLOBALS */
static struct events_backend fdevents_backend = EVENTS_BACKEND_INITIALIZER;
/* END GLOBALS */

#define SET_IF_SET(e1, f1, e2, f2) do {if ((e2) & (f2)) { (e1) |= (f1); }} while (0)

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

static uint16_t to_fdevents_events(short poll_events)
{
	uint16_t events;

	events = 0;
	SET_IF_SET(events, ASYNCIO_FDEVENTS_EVENT_READ, poll_events, POLLIN | POLLPRI);
	SET_IF_SET(events, ASYNCIO_FDEVENTS_EVENT_WRITE, poll_events, POLLOUT);
	SET_IF_SET(events, ASYNCIO_FDEVENTS_EVENT_ERROR, poll_events, POLLERR | POLLHUP | POLLNVAL);

	return events;
}

static short to_poll_events(uint16_t events)
{
	short poll_events;

	poll_events = 0;
	SET_IF_SET(poll_events, POLLIN | POLLPRI, events, ASYNCIO_FDEVENTS_EVENT_READ);
	SET_IF_SET(poll_events, POLLOUT, events, ASYNCIO_FDEVENTS_EVENT_WRITE);
	SET_IF_SET(poll_events, POLLERR | POLLHUP | POLLNVAL, events, ASYNCIO_FDEVENTS_EVENT_ERROR);

	return poll_events;
}

static void copy_fdevents_scratch_locked(struct asyncio_fdevents_loop_priv *fdeventloop_priv)
{
	nfds_t i;

	for (i = 0; i < fdeventloop_priv->nfds; i++)
		fdeventloop_priv->scratch_fds[i] = fdeventloop_priv->fds[i];

	fdeventloop_priv->scratch_nfds = fdeventloop_priv->nfds;
}

static void init_fdevents_refcount_locked(struct fdevents_refcount *refcounts)
{
	unsigned int i;

	COMPILE_TIME_ASSERT(UINT_MAX > POLLFD_EVENT_NBITS);

	for (i = 0; i < POLLFD_EVENT_NBITS; i++)
		refcounts->bitcounts[i] = 0;
}

static int increment_fdevents_refcount_locked(struct fdevents_refcount *refcounts, short events)
{
	unsigned int i;

	COMPILE_TIME_ASSERT(UINT_MAX > POLLFD_EVENT_NBITS);

	/* First check that we can increment without overflows */
	for (i = 0; i < POLLFD_EVENT_NBITS; i++) {
		if ((events & (1 << i)) && (refcounts->bitcounts[i] == UINT_MAX)) {
			ASYNCIO_ERROR("Reached maximum count for fdevents refcounts.\n");
			return -1;
		}
	}

	for (i = 0; i < POLLFD_EVENT_NBITS; i++) {
		if (events & (1 << i))
			++(refcounts->bitcounts[i]);
	}

	return 0;
}

static void decrement_fdevents_refcount_locked(struct fdevents_refcount *refcounts, short events)
{
	unsigned int i;

	COMPILE_TIME_ASSERT(UINT_MAX > POLLFD_EVENT_NBITS);

	/* First check that we can decrement without underflows */
	for (i = 0; i < POLLFD_EVENT_NBITS; i++) {
		if ((events & (1 << i)) && (refcounts->bitcounts[i] == 0)) {
			ASYNCIO_ERROR("fdevents_refcount bitcounts is zero before decrement.\n");
			return;
		}
	}

	for (i = 0; i < POLLFD_EVENT_NBITS; i++) {
		if (events & (1 << i))
			--(refcounts->bitcounts[i]);
	}
}

static short get_eventsmask_from_fdevents_refcount_locked(const struct fdevents_refcount *refcounts)
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

static int fdevents_insert_handle_locked(void *eventloop_instance, void *handle_instance)
{
	struct asyncio_fdevents_loop_priv *fdeventloop_priv;
	struct asyncio_fdevents_handle_priv *fdhandle_priv;
	decl_queue(struct asyncio_fdevents_handle_priv, *cbqueue);
	size_t index;

	fdeventloop_priv = (struct asyncio_fdevents_loop_priv *)eventloop_instance;
	fdhandle_priv = (struct asyncio_fdevents_handle_priv *)handle_instance;

	/* The fd is already in the database, just add handle to queue and increment events refcounts */
	if (asyncio_hashtable_lookup(&fdeventloop_priv->fd_map, sizeof fdhandle_priv->fd, &fdhandle_priv->fd, (const void **)&cbqueue)) {
		index = ((unsigned char *)cbqueue - (unsigned char *)&fdeventloop_priv->callbacks[0]) / sizeof *cbqueue;

		/* Too many events in this fdevents callback queue */
		if (increment_fdevents_refcount_locked(&fdeventloop_priv->fdevents_refcounts[index], fdhandle_priv->events) != 0)
			return -1;

		queue_push(cbqueue, fdhandle_priv);
		fdeventloop_priv->fds[index].events |= fdhandle_priv->events;
	} else {
		if (fdeventloop_priv->nfds == fdeventloop_priv->max_nfds) {
			ASYNCIO_ERROR("fdeventloop nfds reached max_nfds.\n");
			return -1;
		}

		cbqueue = (void *)&fdeventloop_priv->callbacks[fdeventloop_priv->nfds];

		if (asyncio_hashtable_insert(&fdeventloop_priv->fd_map, sizeof fdhandle_priv->fd, &fdhandle_priv->fd, cbqueue) != 0) {
			ASYNCIO_ERROR("Failed to insert fd in hashtable.\n");
			return -1;
		}

		init_fdevents_refcount_locked(&fdeventloop_priv->fdevents_refcounts[fdeventloop_priv->nfds]);

		if (increment_fdevents_refcount_locked(&fdeventloop_priv->fdevents_refcounts[fdeventloop_priv->nfds], fdhandle_priv->events) != 0) {
			ASYNCIO_ERROR("Failed to increment fdevents refcount.\n");
			asyncio_hashtable_delete(&fdeventloop_priv->fd_map, sizeof fdhandle_priv->fd, &fdhandle_priv->fd);
			return -1;
		}

		queue_init(cbqueue, FDEVENTS_HANDLE_QUEUE_ID);
		queue_push(cbqueue, fdhandle_priv);

		fdeventloop_priv->fds[fdeventloop_priv->nfds].fd = fdhandle_priv->fd;
		fdeventloop_priv->fds[fdeventloop_priv->nfds].events = fdhandle_priv->events;
		++(fdeventloop_priv->nfds);
	}

	return 0;
}

static void fdevents_remove_handle_locked(void *eventloop_instance, void *handle_instance)
{
	struct asyncio_fdevents_loop_priv *fdeventloop_priv;
	struct asyncio_fdevents_handle_priv *fdhandle_priv;
	decl_queue(struct asyncio_fdevents_handle_priv, *cbqueue);
	size_t index;
	nfds_t last;

	/* XXX Explicit casting? */
	fdeventloop_priv = (struct asyncio_fdevents_loop_priv *)eventloop_instance;
	fdhandle_priv = (struct asyncio_fdevents_handle_priv *)handle_instance;

	if (!asyncio_hashtable_lookup(&fdeventloop_priv->fd_map, sizeof fdhandle_priv->fd, &fdhandle_priv->fd, (const void **)&cbqueue)) {
		ASYNCIO_ERROR("Tried to remove handle not in the fdeventloop queue.\n");
		return;
	}

	last = fdeventloop_priv->nfds - 1;
	index = ((unsigned char *)cbqueue - (unsigned char *)&fdeventloop_priv->callbacks[0]) / sizeof *cbqueue;
	decrement_fdevents_refcount_locked(&fdeventloop_priv->fdevents_refcounts[index], fdhandle_priv->events);
	queue_remove(cbqueue, fdhandle_priv);

	if (queue_empty(cbqueue)) {
		asyncio_hashtable_delete(&fdeventloop_priv->fd_map, sizeof fdhandle_priv->fd, &fdhandle_priv->fd);
		fdeventloop_priv->fds[index] = fdeventloop_priv->fds[last];
		fdeventloop_priv->callbacks[index] = fdeventloop_priv->callbacks[last];
		fdeventloop_priv->fdevents_refcounts[index] = fdeventloop_priv->fdevents_refcounts[last];
		asyncio_hashtable_modify(&fdeventloop_priv->fd_map, sizeof fdeventloop_priv->fds[last].fd, &fdeventloop_priv->fds[last].fd, cbqueue); /* This can't fail */
		--(fdeventloop_priv->nfds);
	} else {
		fdeventloop_priv->fds[index].events = get_eventsmask_from_fdevents_refcount_locked(&fdeventloop_priv->fdevents_refcounts[index]);
	}
}

static void fdevents_cleanup_events_handle(void *instance)
{
	struct asyncio_fdevents_handle_priv *fdhandle_priv;

	fdhandle_priv = instance;
	asyncio_safe_free(fdhandle_priv);
}

/* Public */
static int fdevents_handle_wait(const struct asyncio_fdevents_handle *handle)
{
	int oldstate;
	int rc;

	ASYNCIO_DISABLE_CANCELLATIONS(&oldstate);
	rc = handle->priv->base.wait(&handle->priv->base, oldstate);
	ASYNCIO_RESTORE_CANCELSTATE(oldstate);

	return rc;
}

/* Public */
static int fdevents_handle_cancel(const struct asyncio_fdevents_handle *handle)
{
	int oldstate;
	int rc;

	ASYNCIO_DISABLE_CANCELLATIONS(&oldstate);
	rc = handle->priv->base.cancel(&handle->priv->base);
	ASYNCIO_RESTORE_CANCELSTATE(oldstate);

	return rc;
}

/* Public */
static int fdevents_handle_acquire(const struct asyncio_fdevents_handle *handle)
{
	int oldstate;
	int rc;

	ASYNCIO_DISABLE_CANCELLATIONS(&oldstate);
	rc = handle->priv->base.acquire(&handle->priv->base);
	ASYNCIO_RESTORE_CANCELSTATE(oldstate);

	return rc;
}

/* Public */
static void fdevents_handle_release(const struct asyncio_fdevents_handle *handle)
{
	int oldstate;

	ASYNCIO_DISABLE_CANCELLATIONS(&oldstate);
	handle->priv->base.release(&handle->priv->base);
	ASYNCIO_RESTORE_CANCELSTATE(oldstate);
}

static void fdevents_handle_callback(void *handle_instance, uint32_t flags, int *continued)
{
	struct asyncio_fdevents_handle_priv *fdhandle_priv;
	int oldtype;
	int oldstate;

	fdhandle_priv = handle_instance;

	/* Set user-defined cancellation settings */
	if (flags & ASYNCIO_THREADPOOL_FLAG_ASYNCCANCEL)
		ASYNCIO_SET_CANCELTYPE(ASYNCIO_CANCEL_ASYNCHRONOUS, &oldtype);
	else
		ASYNCIO_SET_CANCELTYPE(ASYNCIO_CANCEL_DEFERRED, &oldtype);

	if (flags & ASYNCIO_THREADPOOL_FLAG_CANCELLABLE)
		ASYNCIO_SET_CANCELSTATE(ASYNCIO_CANCEL_ENABLE, &oldstate);
	else
		ASYNCIO_SET_CANCELSTATE(ASYNCIO_CANCEL_DISABLE, &oldstate);

	fdhandle_priv->callback_fn(fdhandle_priv->fd, to_fdevents_events(fdhandle_priv->revents), fdhandle_priv->callback_arg, continued);

	ASYNCIO_RESTORE_CANCELSTATE(oldstate);
	ASYNCIO_RESTORE_CANCELTYPE(oldtype);
}

static int fdevents_initialize_eventloop_thread(void *instance)
{
	struct asyncio_fdevents_loop_priv *fdeventloop_priv;

	fdeventloop_priv = instance;

	copy_fdevents_scratch_locked(fdeventloop_priv);

	return 0;
}

static int fdevents_wait_for_events(void *instance)
{
	struct asyncio_fdevents_loop_priv *fdeventloop_priv;

	fdeventloop_priv = instance;

	if (poll(fdeventloop_priv->scratch_fds, fdeventloop_priv->scratch_nfds, -1) < 0) {
		ASYNCIO_SYSERROR("poll");
		return -1;
	}

	return 0;
}

static void fdevents_process_changed_events_locked(void *instance)
{
	struct asyncio_fdevents_loop_priv *fdeventloop_priv;

	fdeventloop_priv = instance;
	copy_fdevents_scratch_locked(fdeventloop_priv);
}

static void fdevents_scan_for_events_locked(void *instance)
{
	struct asyncio_fdevents_loop_priv *fdeventloop_priv;
	struct asyncio_fdevents_handle_priv *fdhandle_priv, *next;
	nfds_t i;

	fdeventloop_priv = instance;

	for (i = 0; i < fdeventloop_priv->scratch_nfds; i++) {
		if (fdeventloop_priv->scratch_fds[i].revents & (fdeventloop_priv->scratch_fds[i].events | POLLERR | POLLHUP | POLLNVAL)) {
			queue_foreach(&fdeventloop_priv->callbacks[i], fdhandle_priv, next) {
				next = fdhandle_priv->next[FDEVENTS_HANDLE_QUEUE_ID]; /* The handle might get removed and the next pointer overwritten otherwise */

				if (fdeventloop_priv->scratch_fds[i].revents & (fdhandle_priv->events | POLLERR | POLLHUP | POLLNVAL)) {
					/* The callback queues are owned by the fdevents_loop, this is the only
					 * thread that is allowed to remove handles from queues. It should not
					 * be done during an fdevents_release_handle */
					fdhandle_priv->revents = fdeventloop_priv->scratch_fds[i].revents;

					if (fdhandle_priv->base.eventloop->dispatch_handle_to_threadpool_locked(&fdhandle_priv->base) != 0)
						ASYNCIO_ERROR("Failed to dispatch events handle to threadpool, leaving in database.\n");
				}
			}
		}
	}
}

static void fdevents_continue_eventloop_thread_locked(void *instance)
{
	struct asyncio_fdevents_loop_priv *fdeventloop_priv;

	fdeventloop_priv = instance;

	/* The masked fds must be removed from the scratch fds array because poll
	 * will return on exceptions even when events is 0. */
	copy_fdevents_scratch_locked(fdeventloop_priv);
}


static void fdevents_cleanup_eventloop_thread(void *instance)
{
	(void)instance;
	/* Nothing to do */
}

/* Public */
static int fdevents_eventloop_listen(struct asyncio_fdevents_loop *fdeventloop, const struct asyncio_fdevents_info *info, struct asyncio_fdevents_handle **handlep)
{
	struct asyncio_fdevents_handle_priv *handle_priv;
	int oldstate;

	ASYNCIO_DISABLE_CANCELLATIONS(&oldstate);

	if (info->cb == NULL) {
		ASYNCIO_RESTORE_CANCELSTATE(oldstate);
		return -1;
	}

	if (set_nonblocking(info->fd) != 0) {
		ASYNCIO_RESTORE_CANCELSTATE(oldstate);
		return -1;
	}

	handle_priv = asyncio_safe_malloc(1, sizeof *handle_priv);

	if (handle_priv == NULL) {
		ASYNCIO_RESTORE_CANCELSTATE(oldstate);
		return -1;
	}

	if (fdeventloop->priv->base.handle_init(&fdeventloop->priv->base, &handle_priv->base, info->flags) != 0) {
		ASYNCIO_ERROR("Failed to init events handle.\n");
		asyncio_safe_free(handle_priv);
		ASYNCIO_RESTORE_CANCELSTATE(oldstate);
		return -1;
	}

	handle_priv->base.instance = handle_priv;

	handle_priv->pub.priv = handle_priv;
	handle_priv->pub.wait = fdevents_handle_wait;
	handle_priv->pub.cancel = fdevents_handle_cancel;
	handle_priv->pub.acquire = fdevents_handle_acquire;
	handle_priv->pub.release = fdevents_handle_release;

	handle_priv->fd = info->fd;
	handle_priv->events = to_poll_events(info->events);
	handle_priv->revents = 0;
	handle_priv->callback_fn = info->cb;
	handle_priv->callback_arg = info->arg;

	if (fdeventloop->priv->base.dispatch_handle_to_eventloop(&fdeventloop->priv->base, &handle_priv->base) != 0) {
		ASYNCIO_ERROR("Failed to dispatch handle to loop.\n");
		fdeventloop->priv->base.handle_cleanup_before_dispatch(&handle_priv->base);
		asyncio_safe_free(handle_priv);
		ASYNCIO_RESTORE_CANCELSTATE(oldstate);
		return -1;
	}

	*handlep = &handle_priv->pub;
	return 0;
}

/* Public */
static int fdevents_eventloop_acquire(const struct asyncio_fdevents_loop *fdeventloop)
{
	int oldstate;
	int rc;

	ASYNCIO_DISABLE_CANCELLATIONS(&oldstate);
	rc = fdeventloop->priv->base.acquire(&fdeventloop->priv->base);
	ASYNCIO_RESTORE_CANCELSTATE(oldstate);

	return rc;
}

/* Public */
static void fdevents_eventloop_release(const struct asyncio_fdevents_loop *fdeventloop)
{
	int oldstate;

	ASYNCIO_DISABLE_CANCELLATIONS(&oldstate);
	fdeventloop->priv->base.release(&fdeventloop->priv->base);
	ASYNCIO_RESTORE_CANCELSTATE(oldstate);
}

static int init_fdeventloop_private_data(const struct asyncio_fdevents_options *options, struct asyncio_fdevents_loop_priv *fdeventloop_priv)
{
	int pipefds[2];
	int rc;

	if (options->max_nfds > NFDST_MAX - 1) {
		ASYNCIO_ERROR("Value for fdevents max_nfds options too large.\n");
		rc = -1;
		goto return_;
	}

	/* No need to insert wake pipefd[0] in hashtable, we always know it's at index 0 */
	if (asyncio_hashtable_init(&fdeventloop_priv->fd_map, options->max_nfds + 1) != 0) {
		ASYNCIO_ERROR("Failed to initialize fdevents_loop hashtable.\n");
		rc = -1;
		goto return_;
	}

	fdeventloop_priv->fds = asyncio_safe_malloc(options->max_nfds + 1, sizeof *(fdeventloop_priv->fds));

	if (fdeventloop_priv->fds == NULL) {
		ASYNCIO_ERROR("Failed to malloc fdeventloop fds.\n");
		rc = -1;
		goto return_fd_map;
	}

	fdeventloop_priv->callbacks = asyncio_safe_malloc(options->max_nfds + 1, sizeof *(fdeventloop_priv->callbacks));

	if (fdeventloop_priv->callbacks == NULL) {
		ASYNCIO_ERROR("Failed to malloc fdeventloop callbacks.\n");
		rc = -1;
		goto return_fds;
	}

	fdeventloop_priv->fdevents_refcounts = asyncio_safe_malloc(options->max_nfds + 1, sizeof *(fdeventloop_priv->fdevents_refcounts));

	if (fdeventloop_priv->fdevents_refcounts == NULL) {
		ASYNCIO_ERROR("Failed to malloc fdeventloop fdevents_refcounts.\n");
		rc = -1;
		goto return_callbacks;
	}

	if (pipe(pipefds) != 0) {
		ASYNCIO_SYSERROR("pipe");
		rc = -1;
		goto return_fdevents_refcounts;
	}

	if (set_nonblocking(pipefds[0]) != 0 || set_nonblocking(pipefds[1]) != 0) {
		rc = -1;
		goto return_pipefds;
	}

	fdeventloop_priv->scratch_fds = asyncio_safe_malloc(options->max_nfds + 1, sizeof *(fdeventloop_priv->scratch_fds));

	if (fdeventloop_priv->scratch_fds == NULL) {
		ASYNCIO_ERROR("Failed to malloc fdeventloop scratch fds.\n");
		rc = -1;
		goto return_pipefds;
	}

	/* These are unused for the wake event */
	queue_init(&fdeventloop_priv->callbacks[0], FDEVENTS_HANDLE_QUEUE_ID);
	init_fdevents_refcount_locked(&fdeventloop_priv->fdevents_refcounts[0]);

	/* Add wake event for poll interruption when a change occured in pollfds */
	fdeventloop_priv->fds[0].fd = pipefds[0];
	fdeventloop_priv->fds[0].events = POLLIN;
	fdeventloop_priv->nfds = 1;
	fdeventloop_priv->max_nfds = (nfds_t)options->max_nfds + 1;
	fdeventloop_priv->changed = 0;
	fdeventloop_priv->wakefd = pipefds[1];
	fdeventloop_priv->clearwakefd = pipefds[0];

	return 0;

return_pipefds:
	if (close(pipefds[0]) != 0)
		ASYNCIO_SYSERROR("close");

	if (close(pipefds[1]) != 0)
		ASYNCIO_SYSERROR("close");

return_fdevents_refcounts:
	asyncio_safe_free(fdeventloop_priv->fdevents_refcounts);

return_callbacks:
	asyncio_safe_free(fdeventloop_priv->callbacks);

return_fds:
	asyncio_safe_free(fdeventloop_priv->fds);

return_fd_map:
	asyncio_hashtable_destroy(&fdeventloop_priv->fd_map);

return_:
	return rc;
}

static void fdevents_wakeup_eventloop_locked(void *instance)
{
	struct asyncio_fdevents_loop_priv *fdeventloop_priv;
	unsigned char dummy = 0;

	fdeventloop_priv = instance;
	dummy = 0;

	if (write(fdeventloop_priv->wakefd, &dummy, sizeof dummy) != sizeof dummy)
		ASYNCIO_SYSERROR("write");
}

static void fdevents_clearwake_eventloop_locked(void *instance)
{
	struct asyncio_fdevents_loop_priv *fdeventloop_priv;
	unsigned char dummy;

	fdeventloop_priv = instance;

	if (read(fdeventloop_priv->clearwakefd, &dummy, sizeof dummy) != sizeof dummy)
		ASYNCIO_SYSERROR("read");
	else if (dummy != 0)
		ASYNCIO_ERROR("Read invalid dummy value.\n");
}

static void fdevents_cleanup_eventloop(void *instance)
{
	struct asyncio_fdevents_loop_priv *fdeventloop_priv;

	fdeventloop_priv = instance;

	asyncio_safe_free(fdeventloop_priv->scratch_fds);
	asyncio_safe_free(fdeventloop_priv->fdevents_refcounts);
	asyncio_safe_free(fdeventloop_priv->callbacks);
	asyncio_safe_free(fdeventloop_priv->fds);

	if (close(fdeventloop_priv->wakefd) != 0)
		ASYNCIO_SYSERROR("close");

	if (close(fdeventloop_priv->clearwakefd) != 0)
		ASYNCIO_SYSERROR("close");

	asyncio_hashtable_destroy(&fdeventloop_priv->fd_map);
	asyncio_safe_free(fdeventloop_priv);
}

/* Public */
int asyncio_fdevents_init()
{
	int oldstate;
	int rc;

	ASYNCIO_DISABLE_CANCELLATIONS(&oldstate);
	rc = asyncio_events_backend_init(&fdevents_backend);
	ASYNCIO_RESTORE_CANCELSTATE(oldstate);

	return rc;
}

/* Public */
int asyncio_fdevents_eventloop(const struct asyncio_fdevents_options *options, struct asyncio_fdevents_loop **eventloop)
{
	struct asyncio_fdevents_loop_priv *fdeventloop_priv;
	int oldstate;

	ASYNCIO_DISABLE_CANCELLATIONS(&oldstate);
	fdeventloop_priv = asyncio_safe_malloc(1, sizeof *fdeventloop_priv);

	if (fdeventloop_priv == NULL) {
		ASYNCIO_ERROR("Failed to malloc fdeventloop_priv.\n");
		ASYNCIO_RESTORE_CANCELSTATE(oldstate);
		return -1;
	}

	if (asyncio_events_backend_eventloop(&fdevents_backend, &fdeventloop_priv->base) != 0) {
		ASYNCIO_ERROR("Failed to create backend base events_loop.\n");
		asyncio_safe_free(fdeventloop_priv);
		ASYNCIO_RESTORE_CANCELSTATE(oldstate);
		return -1;
	}

	fdeventloop_priv->base.instance = fdeventloop_priv;
	fdeventloop_priv->base.backend_callback = fdevents_handle_callback;

	fdeventloop_priv->base.backend_insert_events_handle_locked = fdevents_insert_handle_locked;
	fdeventloop_priv->base.backend_remove_events_handle_locked = fdevents_remove_handle_locked;
	fdeventloop_priv->base.backend_cleanup_events_handle = fdevents_cleanup_events_handle;

	fdeventloop_priv->base.backend_initialize_eventloop_thread = fdevents_initialize_eventloop_thread;
	fdeventloop_priv->base.backend_wait_for_events = fdevents_wait_for_events;
	fdeventloop_priv->base.backend_process_changed_events_locked = fdevents_process_changed_events_locked;
	fdeventloop_priv->base.backend_scan_for_events_locked = fdevents_scan_for_events_locked;
	fdeventloop_priv->base.backend_continue_eventloop_thread_locked = fdevents_continue_eventloop_thread_locked;
	fdeventloop_priv->base.backend_cleanup_eventloop_thread = fdevents_cleanup_eventloop_thread;

	fdeventloop_priv->base.backend_wakeup_eventloop_locked = fdevents_wakeup_eventloop_locked;
	fdeventloop_priv->base.backend_clearwake_eventloop_locked = fdevents_clearwake_eventloop_locked;
	fdeventloop_priv->base.backend_cleanup_eventloop = fdevents_cleanup_eventloop;

	fdeventloop_priv->pub.acquire = fdevents_eventloop_acquire;
	fdeventloop_priv->pub.release = fdevents_eventloop_release;
	fdeventloop_priv->pub.listen = fdevents_eventloop_listen;

	if (init_fdeventloop_private_data(options, fdeventloop_priv) != 0) {
		ASYNCIO_ERROR("Failed to initialize fdeventloop private data.\n");
		fdeventloop_priv->base.release(&fdeventloop_priv->base);
		asyncio_safe_free(fdeventloop_priv);
		return -1;
	}

	*eventloop = &fdeventloop_priv->pub;
	return 0;
}

/* Public */
void asyncio_fdevents_cleanup()
{
	int oldstate;

	ASYNCIO_DISABLE_CANCELLATIONS(&oldstate);
	asyncio_events_backend_cleanup(&fdevents_backend);
	ASYNCIO_RESTORE_CANCELSTATE(oldstate);
}
