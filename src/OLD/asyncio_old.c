#include <limits.h>
#include <pthread.h>

#include "asyncio.h"
#include "threadpool.h"
#include "fdevents.h"
#include "timevents.h"
#include "safe_malloc.h"
#include "cancellations.h"
#include "logging.h"

/* STRUCT DEFINITIONS */
enum asyncio_handle_type {
	ASYNCIO_TYPE_THREAD,
	ASYNCIO_TYPE_FDEVENT,
	ASYNCIO_TYPE_TIMEVENT
};

struct asyncio_handle {
	enum asyncio_handle_type type;
	unsigned int refcount;
	pthread_mutex_t mtx;

	union {
		threadpool_handle_t thandle;
		fdevent_handle_t fdhandle;
		timevent_handle_t timhandle;
	} union_handle;
};
/* END STRUCT DEFINITIONS */

/* PROTOTYPES */
static uint32_t to_threadpool_flags(asyncio_flag_t flags);
static uint32_t to_fdevents_flags(asyncio_flag_t flags);
static uint32_t to_timevents_flags(asyncio_flag_t flags);

static uint16_t to_fdevents_events(asyncio_fdevent_t events);

static int init_asyncio_threadpool_handle(struct asyncio_handle *handle, asyncio_flag_t flags, asyncio_dispatch_fn fn, void *arg);
static int init_asyncio_fdevent_handle(struct asyncio_handle *handle, int fd, asyncio_fdevent_t events, asyncio_flag_t flags, asyncio_fdevent_cb cb, void *arg);
static int init_asyncio_timevent_handle(struct asyncio_handle *handle, asyncio_time_t timeout, asyncio_flag_t flags, asyncio_timevent_cb cb, void *arg);
static int lock_asyncio_handle(struct asyncio_handle *handle);
static void unlock_asyncio_handle(struct asyncio_handle *handle);
static void cleanup_asyncio_handle(struct asyncio_handle *handle);
/* END PROTOTYPES */

#define SET_IF_SET(e1, f1, e2, f2) do {if ((e2) & (f2)) { (e1) |= (f1); }} while (0)

static uint32_t to_threadpool_flags(asyncio_flag_t flags)
{
	uint32_t threadpool_flags;

	threadpool_flags = flags;
	return threadpool_flags;
}

static uint32_t to_fdevents_flags(asyncio_flag_t flags)
{
	uint32_t fdevents_flags;

	fdevents_flags = flags;
	return fdevents_flags;
}

static uint32_t to_timevents_flags(asyncio_flag_t flags)
{
	uint32_t timevents_flags;

	timevents_flags = flags;
	return timevents_flags;
}

static uint16_t to_fdevents_events(asyncio_fdevent_t events)
{
	uint16_t fdevents_events;

	fdevents_events = events;
	return fdevents_events;
}

static int init_asyncio_threadpool_handle(struct asyncio_handle *handle, asyncio_flag_t flags, asyncio_dispatch_fn fn, void *arg)
{
	struct threadpool_dispatch_info info;
	threadpool_handle_t thandle;

	if (pthread_mutex_init(&handle->mtx, NULL) != 0) {
		ASYNCIO_SYSERROR("pthread_mutex_init");
		return -1;
	}

	info.flags = to_threadpool_flags(flags);
	info.dispatch_info.fn = fn;
	info.dispatch_info.arg = arg;
	info.completed_info.cb = NULL;
	info.completed_info.arg = NULL;
	info.cancelled_info.cb = NULL;
	info.cancelled_info.arg = NULL;

	if (threadpool_dispatch(&info, &thandle) != 0) {
		if (pthread_mutex_destroy(&handle->mtx) != 0)
			ASYNCIO_SYSERROR("pthread_mutex_destroy");

		return -1;
	}

	handle->type = ASYNCIO_TYPE_THREAD;
	handle->union_handle.thandle = thandle;
	handle->refcount = 1;
	return 0;
}

static int init_asyncio_fdevent_handle(struct asyncio_handle *handle, int fd, asyncio_fdevent_t events, asyncio_flag_t flags, asyncio_fdevent_cb cb, void *arg)
{
	struct fdevent_info info;
	fdevent_handle_t fdhandle;

	if (pthread_mutex_init(&handle->mtx, NULL) != 0) {
		ASYNCIO_SYSERROR("pthread_mutex_init");
		return -1;
	}

	info.fd = fd;
	info.events = to_fdevents_events(events);
	info.flags = to_fdevents_flags(flags);
	info.cb = cb;
	info.arg = arg;

	if (fdevent_register(&info, &fdhandle) != 0) {
		if (pthread_mutex_destroy(&handle->mtx) != 0)
			ASYNCIO_SYSERROR("pthread_mutex_destroy");

		return -1;
	}

	handle->type = ASYNCIO_TYPE_FDEVENT;
	handle->union_handle.fdhandle = fdhandle;
	handle->refcount = 1;
	return 0;
}

static int init_asyncio_timevent_handle(struct asyncio_handle *handle, asyncio_time_t timeout, asyncio_flag_t flags, asyncio_timevent_cb cb, void *arg)
{
	struct timevent_info info;
	timevent_handle_t timhandle;

	if (pthread_mutex_init(&handle->mtx, NULL) != 0) {
		ASYNCIO_SYSERROR("pthread_mutex_init");
		return -1;
	}

	info.timeout = timeout;
	info.flags = to_timevents_flags(flags);
	info.cb = cb;
	info.arg = arg;

	if (timevent_register(&info, &timhandle) != 0) {
		if (pthread_mutex_destroy(&handle->mtx) != 0)
			ASYNCIO_SYSERROR("pthread_mutex_destroy");

		return -1;
	}

	handle->type = ASYNCIO_TYPE_TIMEVENT;
	handle->union_handle.timhandle = timhandle;
	handle->refcount = 1;
	return 0;
}

static int lock_asyncio_handle(struct asyncio_handle *handle)
{
	if (pthread_mutex_lock(&handle->mtx) != 0) {
		ASYNCIO_SYSERROR("pthread_mutex_lock");
		return -1;
	}

	return 0;
}

static void unlock_asyncio_handle(struct asyncio_handle *handle)
{
	if (pthread_mutex_unlock(&handle->mtx) != 0)
		ASYNCIO_SYSERROR("pthread_mutex_unlock");
}

static void cleanup_asyncio_handle(struct asyncio_handle *handle)
{
	if (pthread_mutex_destroy(&handle->mtx) != 0)
		ASYNCIO_SYSERROR("pthread_mutex_destroy");

	switch (handle->type) {
		case ASYNCIO_TYPE_THREAD:
			threadpool_release_handle(handle->union_handle.thandle);
			break;

		case ASYNCIO_TYPE_FDEVENT:
			fdevent_release_handle(handle->union_handle.fdhandle);
			break;

		case ASYNCIO_TYPE_TIMEVENT:
			timevent_release_handle(handle->union_handle.timhandle);
			break;

		default:
			break;
	}
}

__attribute__((visibility("default")))
int asyncio_dispatch(asyncio_dispatch_fn fn, void *arg, asyncio_flag_t flags, asyncio_handle_t *ahandle)
{
	struct asyncio_handle *handle;
	int oldstate;

	disable_cancellations(&oldstate);
	handle = safe_malloc(sizeof *handle);

	if (handle == NULL) {
		restore_cancelstate(oldstate);
		return -1;
	}

	if (init_asyncio_threadpool_handle(handle, flags, fn, arg) != 0) {
		safe_free(handle);
		restore_cancelstate(oldstate);
		return -1;
	}

	*ahandle = (asyncio_handle_t)handle;

	restore_cancelstate(oldstate);
	return 0;
}

__attribute__((visibility("default")))
int asyncio_fdevent(int fd, asyncio_fdevent_t events, asyncio_fdevent_cb cb, void *arg, asyncio_flag_t flags, asyncio_handle_t *ahandle)
{
	struct asyncio_handle *handle;
	int oldstate;

	disable_cancellations(&oldstate);
	handle = safe_malloc(sizeof *handle);

	if (handle == NULL) {
		restore_cancelstate(oldstate);
		return -1;
	}

	if (init_asyncio_fdevent_handle(handle, fd, events, flags, cb, arg) != 0) {
		safe_free(handle);
		restore_cancelstate(oldstate);
		return -1;
	}

	*ahandle = (asyncio_handle_t)handle;

	restore_cancelstate(oldstate);
	return 0;
}

__attribute__((visibility("default")))
int asyncio_timevent(asyncio_time_t timeout, asyncio_timevent_cb cb, void *arg, asyncio_flag_t flags, asyncio_handle_t *ahandle)
{
	struct asyncio_handle *handle;
	int oldstate;

	disable_cancellations(&oldstate);
	handle = safe_malloc(sizeof *handle);

	if (handle == NULL) {
		restore_cancelstate(oldstate);
		return -1;
	}

	if (init_asyncio_timevent_handle(handle, timeout, flags, cb, arg) != 0) {
		safe_free(handle);
		restore_cancelstate(oldstate);
		return -1;
	}

	*ahandle = (asyncio_handle_t)handle;

	restore_cancelstate(oldstate);
	return 0;
}

__attribute__((visibility("default")))
int asyncio_join(asyncio_handle_t ahandle)
{
	struct asyncio_handle *handle;
	int oldstate;
	int rc;

	disable_cancellations(&oldstate);

	handle = (struct asyncio_handle *)ahandle;

	switch (handle->type) {
		case ASYNCIO_TYPE_THREAD:
			rc = threadpool_join(handle->union_handle.thandle);
			break;

		case ASYNCIO_TYPE_FDEVENT:
			rc = fdevent_join(handle->union_handle.fdhandle);
			break;

		case ASYNCIO_TYPE_TIMEVENT:
			rc = timevent_join(handle->union_handle.timhandle);
			break;

		default:
			rc = -1;
	}

	restore_cancelstate(oldstate);
	return rc;
}

__attribute__((visibility("default")))
int asyncio_cancel(asyncio_handle_t ahandle)
{
	struct asyncio_handle *handle;
	int oldstate;
	int rc;

	disable_cancellations(&oldstate);

	handle = (struct asyncio_handle *)ahandle;

	switch (handle->type) {
		case ASYNCIO_TYPE_THREAD:
			rc = threadpool_cancel(handle->union_handle.thandle);
			break;

		case ASYNCIO_TYPE_FDEVENT:
			rc = fdevent_cancel(handle->union_handle.fdhandle);
			break;

		case ASYNCIO_TYPE_TIMEVENT:
			rc = timevent_cancel(handle->union_handle.timhandle);
			break;

		default:
			rc = -1;
	}

	restore_cancelstate(oldstate);
	return rc;
}

__attribute__((visibility("default")))
int asyncio_acquire(asyncio_handle_t ahandle)
{
	struct asyncio_handle *handle;
	int oldstate;

	disable_cancellations(&oldstate);

	handle = (struct asyncio_handle *)ahandle;

	if (lock_asyncio_handle(handle) != 0) {
		restore_cancelstate(oldstate);
		return -1;
	}

	if (handle->refcount == UINT_MAX) {
		unlock_asyncio_handle(handle);
		restore_cancelstate(oldstate);
		return -1;
	}

	++handle->refcount;

	unlock_asyncio_handle(handle);
	restore_cancelstate(oldstate);
	return 0;
}

__attribute__((visibility("default")))
void asyncio_release(asyncio_handle_t ahandle)
{
	struct asyncio_handle *handle;
	int oldstate;

	disable_cancellations(&oldstate);

	handle = (struct asyncio_handle *)ahandle;

	if (lock_asyncio_handle(handle) != 0) {
		restore_cancelstate(oldstate);
		return;
	}

	if (handle->refcount == 0) {
		ASYNCIO_ERROR("AsyncIO handle refcount is 0 before release.\n");
		unlock_asyncio_handle(handle);
		restore_cancelstate(oldstate);
		return;
	}

	--handle->refcount;

	if (handle->refcount == 0) {
		unlock_asyncio_handle(handle);
		cleanup_asyncio_handle(handle);
		safe_free(handle);
	}

	restore_cancelstate(oldstate);
}
