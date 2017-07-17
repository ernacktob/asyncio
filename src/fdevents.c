#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>

#include "fdevents_poll_backend.h"
#include "fdevents_select_backend.h"
#include "fdevents_priv.h"
#include "events.h"
#include "asyncio_fdevents.h"
#include "asyncio_threadpool.h"
#include "queue.h"
#include "threading.h"
#include "logging.h"
#include "safe_malloc.h"
#include "constants.h"
#include "compile_time_assert.h"

/* PROTOTYPES */
static int set_nonblocking(int fd);

static int fdevents_insert_handle_locked(void *eventloop_instance, void *handle_instance);
static void fdevents_remove_handle_locked(void *eventloop_instance, void *handle_instance);

static void fdevents_cleanup_events_handle(void *eventloop_instance, void *handle_instance);

static int fdevents_handle_wait(const struct asyncio_fdevents_handle *handle); /* PUBLIC */
static int fdevents_handle_cancel(const struct asyncio_fdevents_handle *handle); /* PUBLIC */
static int fdevents_handle_acquire(const struct asyncio_fdevents_handle *handle); /* PUBLIC */
static void fdevents_handle_release(const struct asyncio_fdevents_handle *handle); /* PUBLIC */

static void fdevents_handle_callback(void *handle_instance, uint32_t threadpool_flags, int *continued);

static int fdevents_initialize_eventloop_thread(void *instance);
static int fdevents_wait_for_events(void *instance);
static void fdevents_process_changed_events_locked(void *instance);
static void fdevents_scan_for_events_locked(void *instance);
static void fdevents_continue_eventloop_thread_locked(void *instance);
static void fdevents_cleanup_eventloop_thread(void *instance);

static int fdevents_eventloop_listen(struct asyncio_fdevents_loop *fdeventloop, int fd, const void *evinfo, asyncio_fdevents_callback cb, void *arg, uint32_t threadpool_flags, struct asyncio_fdevents_handle **handlep); /* PUBLIC */
static int fdevents_eventloop_acquire(const struct asyncio_fdevents_loop *fdeventloop); /* PUBLIC */
static void fdevents_eventloop_release(const struct asyncio_fdevents_loop *fdeventloop); /* PUBLIC */

static int init_fdeventloop_private_data(const struct asyncio_fdevents_options *options, struct asyncio_fdevents_loop_priv *fdeventloop_priv);
static void fdevents_wakeup_eventloop_locked(void *instance);
static void fdevents_clearwake_eventloop_locked(void *instance);
static void fdevents_cleanup_eventloop(void *instance);
/* END PROTOTYPES */

/* GLOBALS */
static struct events_backend fdevents_backend = EVENTS_BACKEND_INITIALIZER;
static struct fdevents_backend_ops *fdevents_backend_ops_list[ASYNCIO_FDEVENTS_MAX_BACKEND_TYPES] = {NULL};
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

static int fdevents_insert_handle_locked(void *eventloop_instance, void *handle_instance)
{
	struct asyncio_fdevents_loop_priv *fdeventloop_priv;
	struct asyncio_fdevents_handle_priv *fdhandle_priv;
	size_t index;

	fdeventloop_priv = eventloop_instance;
	fdhandle_priv = handle_instance;

	/* The fd is already in the database, just add handle to queue and acquire evinfo */
	if (asyncio_hashtable_lookup(&fdeventloop_priv->fd_map, fdhandle_priv->fd, &index)) {
		if (fdeventloop_priv->backend_ops->acquire_evinfo_locked(fdeventloop_priv->backend_data, fdhandle_priv->fd, fdhandle_priv->evinfo)) {
			/* ASYNCIO_ERROR("The fdevents backend cannot acquire the evinfo for this fd.\n"); */
			return -1;
		}

		queue_push(&fdeventloop_priv->callbacks[index], fdhandle_priv);
	} else {
		if (fdeventloop_priv->backend_ops->insert_fd_locked(fdeventloop_priv->backend_data, fdhandle_priv->fd, fdhandle_priv->evinfo) != 0) {
			ASYNCIO_ERROR("Failed to insert new fd in fdevents backend database.\n");
			return -1;
		}

		index = fdeventloop_priv->last;

		if (asyncio_hashtable_insert(&fdeventloop_priv->fd_map, fdhandle_priv->fd, index) != 0) {
			ASYNCIO_ERROR("Failed to insert fd in hashtable.\n");
			fdeventloop_priv->backend_ops->remove_fd_locked(fdeventloop_priv->backend_data, fdhandle_priv->fd);
			return -1;
		}

		queue_init(&fdeventloop_priv->callbacks[index], FDEVENTS_HANDLE_QUEUE_ID);
		queue_push(&fdeventloop_priv->callbacks[index], fdhandle_priv);
		++(fdeventloop_priv->last);
	}

	return 0;
}

static void fdevents_remove_handle_locked(void *eventloop_instance, void *handle_instance)
{
	struct asyncio_fdevents_loop_priv *fdeventloop_priv;
	struct asyncio_fdevents_handle_priv *fdhandle_priv, *last_fdhandle;
	size_t index, last_valid_index;

	fdeventloop_priv = eventloop_instance;
	fdhandle_priv = handle_instance;

	if (fdeventloop_priv->last == 0) {
		ASYNCIO_ERROR("Tried to remove handle from fdeventloop with last == 0. Should not happen.\n");
		return;
	}

	if (!asyncio_hashtable_lookup(&fdeventloop_priv->fd_map, fdhandle_priv->fd, &index)) {
		ASYNCIO_ERROR("Tried to remove handle not in the fdeventloop queue.\n");
		return;
	}

	fdeventloop_priv->backend_ops->release_evinfo_locked(fdeventloop_priv->backend_data, fdhandle_priv->fd, fdhandle_priv->evinfo);
	queue_remove(&fdeventloop_priv->callbacks[index], fdhandle_priv);

	if (queue_empty(&fdeventloop_priv->callbacks[index])) {
		asyncio_hashtable_delete(&fdeventloop_priv->fd_map, fdhandle_priv->fd);
		fdeventloop_priv->backend_ops->remove_fd_locked(fdeventloop_priv->backend_data, fdhandle_priv->fd);

		last_valid_index = fdeventloop_priv->last - 1;

		/* Swap the last callback queue into the released slot to keep all valid queues consecutive. */
		if (index < last_valid_index) {
			last_fdhandle = queue_first(&fdeventloop_priv->callbacks[last_valid_index]);
			asyncio_hashtable_modify(&fdeventloop_priv->fd_map, last_fdhandle->fd, index);	/* This can't fail. */
			fdeventloop_priv->callbacks[index] = fdeventloop_priv->callbacks[last_valid_index];
		}

		--(fdeventloop_priv->last);
	}
}

static void fdevents_cleanup_events_handle(void *eventloop_instance, void *handle_instance)
{
	struct asyncio_fdevents_loop_priv *fdeventloop_priv;
	struct asyncio_fdevents_handle_priv *fdhandle_priv;

	fdeventloop_priv = eventloop_instance;
	fdhandle_priv = handle_instance;

	fdeventloop_priv->backend_ops->cleanup_evinfo_revinfo(fdhandle_priv->evinfo, fdhandle_priv->revinfo);
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

static void fdevents_handle_callback(void *handle_instance, uint32_t threadpool_flags, int *continued)
{
	struct asyncio_fdevents_handle_priv *fdhandle_priv;
	int oldtype;
	int oldstate;

	fdhandle_priv = handle_instance;

	/* Set user-defined cancellation settings */
	if (threadpool_flags & ASYNCIO_THREADPOOL_FLAG_ASYNCCANCEL)
		ASYNCIO_SET_CANCELTYPE(ASYNCIO_CANCEL_ASYNCHRONOUS, &oldtype);
	else
		ASYNCIO_SET_CANCELTYPE(ASYNCIO_CANCEL_DEFERRED, &oldtype);

	if (threadpool_flags & ASYNCIO_THREADPOOL_FLAG_CANCELLABLE)
		ASYNCIO_SET_CANCELSTATE(ASYNCIO_CANCEL_ENABLE, &oldstate);
	else
		ASYNCIO_SET_CANCELSTATE(ASYNCIO_CANCEL_DISABLE, &oldstate);

	fdhandle_priv->callback_fn(fdhandle_priv->fd, fdhandle_priv->revinfo, fdhandle_priv->callback_arg, continued);

	ASYNCIO_RESTORE_CANCELSTATE(oldstate);
	ASYNCIO_RESTORE_CANCELTYPE(oldtype);
}

static int fdevents_initialize_eventloop_thread(void *instance)
{
	struct asyncio_fdevents_loop_priv *fdeventloop_priv;

	fdeventloop_priv = instance;
	return fdeventloop_priv->backend_ops->initialize_eventloop_thread(fdeventloop_priv->backend_data);
}

static int fdevents_wait_for_events(void *instance)
{
	struct asyncio_fdevents_loop_priv *fdeventloop_priv;

	fdeventloop_priv = instance;
	return fdeventloop_priv->backend_ops->wait_for_events(fdeventloop_priv->backend_data);
}

static void fdevents_process_changed_events_locked(void *instance)
{
	struct asyncio_fdevents_loop_priv *fdeventloop_priv;

	fdeventloop_priv = instance;
	fdeventloop_priv->backend_ops->process_changed_events_locked(fdeventloop_priv->backend_data);
}

static void fdevents_scan_for_events_locked(void *instance)
{
	struct asyncio_fdevents_loop_priv *fdeventloop_priv;
	struct asyncio_fdevents_handle_priv *fdhandle_priv, *next;
	size_t index;
	int fd;

	fdeventloop_priv = instance;
	fdeventloop_priv->backend_ops->scan_for_events_init_locked(fdeventloop_priv->backend_data);

	while (fdeventloop_priv->backend_ops->scan_for_events_next_locked(fdeventloop_priv->backend_data, &fd)) {
		if (!asyncio_hashtable_lookup(&fdeventloop_priv->fd_map, fd, &index)) {
			ASYNCIO_ERROR("Did not have fd in fdeventloop hashtable. Should never happen.\n");
			break;
		}

		queue_foreach(&fdeventloop_priv->callbacks[index], fdhandle_priv, next) {
			next = fdhandle_priv->next[FDEVENTS_HANDLE_QUEUE_ID]; /* The handle might get removed and the next pointer overwritten otherwise */

			/* This will also set the revinfo upon successful match */
			if (fdeventloop_priv->backend_ops->match_evinfo_locked(fdeventloop_priv->backend_data, fd, fdhandle_priv->evinfo, fdhandle_priv->revinfo)) {
				/* The callback queues are owned by the fdevents_loop, this is the only
				 * thread that is allowed to remove handles from queues. It should not
				 * be done during an fdevents_release_handle */

				/* Note that the dispatch_handle_to_threadpool_locked calls our fdevents_remove_handle_locked which
				 * will remove handle from the callback queue upon successful dispatch to threadpool. */
				if (fdhandle_priv->base.eventloop->dispatch_handle_to_threadpool_locked(&fdhandle_priv->base) != 0)
					ASYNCIO_ERROR("Failed to dispatch events handle to threadpool, leaving in database.\n");
			}
		}
	}
}

static void fdevents_continue_eventloop_thread_locked(void *instance)
{
	struct asyncio_fdevents_loop_priv *fdeventloop_priv;

	fdeventloop_priv = instance;
	fdeventloop_priv->backend_ops->continue_eventloop_thread_locked(fdeventloop_priv->backend_data);
}


static void fdevents_cleanup_eventloop_thread(void *instance)
{
	struct asyncio_fdevents_loop_priv *fdeventloop_priv;

	fdeventloop_priv = instance;
	fdeventloop_priv->backend_ops->cleanup_eventloop_thread(fdeventloop_priv->backend_data);
}

/* Public */
static int fdevents_eventloop_listen(struct asyncio_fdevents_loop *fdeventloop, int fd, const void *evinfo, asyncio_fdevents_callback cb, void *arg, uint32_t threadpool_flags, struct asyncio_fdevents_handle **handlep)
{
	struct asyncio_fdevents_handle_priv *handle_priv;
	int oldstate;

	ASYNCIO_DISABLE_CANCELLATIONS(&oldstate);

	if (cb == NULL) {
		ASYNCIO_RESTORE_CANCELSTATE(oldstate);
		return -1;
	}

	/* XXX We don't need no nonblocking! (for user fds, they can set them if they want) */
	if (set_nonblocking(fd) != 0) {
		ASYNCIO_RESTORE_CANCELSTATE(oldstate);
		return -1;
	}

	handle_priv = asyncio_safe_malloc(1, sizeof *handle_priv);

	if (handle_priv == NULL) {
		ASYNCIO_RESTORE_CANCELSTATE(oldstate);
		return -1;
	}

	if (fdeventloop->priv->base.handle_init(&fdeventloop->priv->base, &handle_priv->base, threadpool_flags) != 0) {
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

	if (fdeventloop->priv->backend_ops->init_evinfo_revinfo(evinfo, &handle_priv->evinfo, &handle_priv->revinfo) != 0) {
		ASYNCIO_ERROR("Failed to init backend evinfo and revinfo for handle.\n");
		fdeventloop->priv->base.handle_cleanup_before_dispatch(&handle_priv->base);
		asyncio_safe_free(handle_priv);
		ASYNCIO_RESTORE_CANCELSTATE(oldstate);
		return -1;
	}

	handle_priv->fd = fd;
	handle_priv->callback_fn = cb;
	handle_priv->callback_arg = arg;

	if (fdeventloop->priv->base.dispatch_handle_to_eventloop(&fdeventloop->priv->base, &handle_priv->base) != 0) {
		ASYNCIO_ERROR("Failed to dispatch handle to loop.\n");
		fdeventloop->priv->backend_ops->cleanup_evinfo_revinfo(handle_priv->evinfo, handle_priv->revinfo);
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

	if (options->backend_type >= ASYNCIO_FDEVENTS_MAX_BACKEND_TYPES) {
		ASYNCIO_ERROR("Invalid fdevents backend type.\n");
		rc = -1;
		goto return_;
	}

	if (fdevents_backend_ops_list[options->backend_type] == NULL) {
		ASYNCIO_ERROR("Invalid fdevents backend type.\n");
		rc = -1;
		goto return_;
	}

	/* Since we are adding one descriptor ourselves, should check for overflow */
	if (options->max_nfds > SIZET_MAX - 1) {
		ASYNCIO_ERROR("The max_nfds option for fdevents is too large.\n");
		rc = -1;
		goto return_;
	}

	fdeventloop_priv->backend_ops = fdevents_backend_ops_list[options->backend_type];

	if (pipe(pipefds) != 0) {
		ASYNCIO_SYSERROR("pipe");
		rc = -1;
		goto return_;
	}

	if (set_nonblocking(pipefds[0]) != 0 || set_nonblocking(pipefds[1]) != 0) {
		ASYNCIO_ERROR("Failed to set pipefds nonblocking.\n");
		rc = -1;
		goto return_pipefds;
	}

	/* The user nfds and also one for the clearwakefd. */
	if (fdeventloop_priv->backend_ops->init_backend_data(&fdeventloop_priv->backend_data, options->max_nfds + 1, pipefds[0]) != 0) {
		ASYNCIO_ERROR("Failed to init fdevents backend data.\n");
		rc = -1;
		goto return_pipefds;
	}

	/* The wakefd does not have any callbacks */
	fdeventloop_priv->callbacks = asyncio_safe_malloc(options->max_nfds, sizeof *(fdeventloop_priv->callbacks));

	if (fdeventloop_priv->callbacks == NULL) {
		ASYNCIO_ERROR("Failed to malloc fdeventloop callbacks.\n");
		rc = -1;
		goto return_backend_data;
	}

	/* No need to insert wake pipefd[0] in hashtable. */
	if (asyncio_hashtable_init(&fdeventloop_priv->fd_map, options->max_nfds) != 0) {
		ASYNCIO_ERROR("Failed to init fdeventloop_priv fd_map.\n");
		rc = -1;
		goto return_callbacks;
	}

	/* Points to next available slot. Note that the wakefd does not take a callback queue slot. */
	fdeventloop_priv->last = 0;

	/* Add wake event for poll interruption when a change occured in pollfds */
	fdeventloop_priv->wakefd = pipefds[1];
	fdeventloop_priv->clearwakefd = pipefds[0];

	return 0;

return_callbacks:
	asyncio_safe_free(fdeventloop_priv->callbacks);

return_backend_data:
	fdeventloop_priv->backend_ops->cleanup_backend_data(fdeventloop_priv->backend_data);

return_pipefds:
	if (close(pipefds[0]) != 0)
		ASYNCIO_SYSERROR("close");

	if (close(pipefds[1]) != 0)
		ASYNCIO_SYSERROR("close");

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

	fdeventloop_priv->backend_ops->cleanup_backend_data(fdeventloop_priv->backend_data);
	asyncio_safe_free(fdeventloop_priv->callbacks);

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

	fdevents_backend_ops_list[ASYNCIO_FDEVENTS_BACKEND_POLL] = &asyncio_fdevents_poll_backend_ops;
	fdevents_backend_ops_list[ASYNCIO_FDEVENTS_BACKEND_SELECT] = &asyncio_fdevents_select_backend_ops;

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
		ASYNCIO_RESTORE_CANCELSTATE(oldstate);
		return -1;
	}

	*eventloop = &fdeventloop_priv->pub;
	ASYNCIO_RESTORE_CANCELSTATE(oldstate);
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
