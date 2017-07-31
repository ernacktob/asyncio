#include <strings.h>
#include <stddef.h>
#include <limits.h>

#include "asyncio_streams.h"
#include "asyncio_fdevents.h"
#include "streams_priv.h"
#include "safe_malloc.h"
#include "threading.h"
#include "refcounts.h"
#include "logging.h"

/* PROTOTYPES */
static int streams_handle_wait(const struct asyncio_streams_handle *handle); /* PUBLIC */
static int streams_handle_cancel(const struct asyncio_streams_handle *handle); /* PUBLIC */
static int streams_handle_acquire(const struct asyncio_streams_handle *handle); /* PUBLIC */
static void streams_handle_release(const struct asyncio_streams_handle *handle); /* PUBLIC */

static void streams_handle_destructor(void *instance);
static void streams_read_cancelled_cb(void *arg);
static void streams_write_cancelled_cb(void *arg);
static void streams_read_cb(const struct asyncio_fdevents_callback_info *info, int *continued);
static void streams_write_cb(const struct asyncio_fdevents_callback_info *info, int *continued);
/* END PROTOTYPES */

/* PUBLIC */
static int streams_handle_wait(const struct asyncio_streams_handle *handle)
{
	/* Do not disable cancellations while waiting.
	 * The fdevents takes care of taking the oldstate and passing it to events module. */
	return handle->priv->evhandle->wait(handle->priv->evhandle);
}

/* PUBLIC */
static int streams_handle_cancel(const struct asyncio_streams_handle *handle)
{
	int oldstate;
	int rc;

	ASYNCIO_DISABLE_CANCELLATIONS(&oldstate);
	rc = handle->priv->evhandle->cancel(handle->priv->evhandle);
	ASYNCIO_RESTORE_CANCELSTATE(oldstate);

	return rc;
}

/* PUBLIC */
static int streams_handle_acquire(const struct asyncio_streams_handle *handle)
{
	int oldstate;
	int rc;

	ASYNCIO_DISABLE_CANCELLATIONS(&oldstate);
	rc = asyncio_refcount_acquire(&handle->priv->refcount);
	ASYNCIO_RESTORE_CANCELSTATE(oldstate);

	return rc;
}

/* PUBLIC */
static void streams_handle_release(const struct asyncio_streams_handle *handle)
{
	int oldstate;

	ASYNCIO_DISABLE_CANCELLATIONS(&oldstate);
	asyncio_refcount_release(&handle->priv->refcount);
	ASYNCIO_RESTORE_CANCELSTATE(oldstate);
}

static void streams_handle_destructor(void *arg)
{
	struct asyncio_streams_handle_priv *handle_priv;

	handle_priv = arg;
	handle_priv->evhandle->release(handle_priv->evhandle);
	asyncio_safe_free(handle_priv);
}

static void streams_read_cancelled_cb(void *arg)
{
	struct asyncio_streams_handle_priv *handle_priv;

	handle_priv = arg;

	if (handle_priv->policy.cancelled_cb != NULL)
		handle_priv->policy.cancelled_cb(handle_priv->policy.state);

	/* Release our module's reference to the streams_handle because
	 * it won't happen in the read/write callbacks (we got cancelled so these won't get called).
	 * Note that user may still be holding a reference. */
	asyncio_refcount_release(&handle_priv->refcount);
}

static void streams_write_cancelled_cb(void *arg)
{
	struct asyncio_streams_handle_priv *handle_priv;

	handle_priv = arg;

	if (handle_priv->policy.cancelled_cb != NULL)
		handle_priv->policy.cancelled_cb(handle_priv->policy.state);

	/* Release our module's reference to the fdevents_handle because
	 * it won't happen in the read/write callbacks (we got cancelled so these won't get called).
	 * Note that user may still be holding a reference. */
	asyncio_refcount_release(&handle_priv->refcount);
}

static void streams_read_cb(const struct asyncio_fdevents_callback_info *info, int *continued)
{
	struct asyncio_streams_handle_priv *handle_priv;
	const struct asyncio_streams_policy *policy;
	size_t rdlen = 0;
	uint8_t *unread_buf;
	size_t unread_len;

	handle_priv = info->arg;
	policy = &handle_priv->policy;

	if (policy->is_errored(info->revinfo, policy->state)) {
		policy->error_cb(policy->state);
		asyncio_refcount_release(&handle_priv->refcount);
		*continued = 0;
		return;
	}

	if (policy->read_cb(handle_priv->info.read_info.buf + handle_priv->pos, handle_priv->len - handle_priv->pos, &rdlen, policy->state) != 0) {
		policy->error_cb(policy->state);
		asyncio_refcount_release(&handle_priv->refcount);
		*continued = 0;
		return;
	}

	/* Check for end-of-file */
	if (rdlen == 0) {
		policy->eof_cb(policy->state);
		asyncio_refcount_release(&handle_priv->refcount);
		*continued = 0;
		return;
	}

	if (rdlen > handle_priv->len - handle_priv->pos) {
		ASYNCIO_ERROR("Buffevents rdlen larger than available size.\n");
		policy->error_cb(policy->state);
		asyncio_refcount_release(&handle_priv->refcount);
		*continued = 0;
		return;
	}

	handle_priv->pos += rdlen;

	if (handle_priv->pos == handle_priv->len) {
		handle_priv->info.read_info.done_cb(handle_priv->pos, policy->state);
		asyncio_refcount_release(&handle_priv->refcount);
		*continued = 0;
		return;
	}

	if (policy->until_cb != NULL) {
		/* If we have an until_cb, shouldn't we be reading 1 byte at a time?
		 * Though, I suppose this is up to the user, and this gets controlled
		 * in the read_cb. */
		if (policy->until_cb(handle_priv->pos, &unread_buf, &unread_len, policy->state)) {
			/* We might have read too much, so we "unread" the lefover data
			 * into the unread buffer. Do some consistency checks too. */
			if (unread_buf == NULL && unread_len != 0) {
				ASYNCIO_ERROR("Tried to unread into NULL buffer for asyncio_streams.\n");
				policy->error_cb(policy->state);
			} else if (unread_len > handle_priv->pos) {
				ASYNCIO_ERROR("Tried to unread more than what was read in asyncio_streams.\n");
				policy->error_cb(policy->state);
			} else if (unread_len > 0) {
				/* The order is very important here! The memory should be overwritten only
				 * after the callback returns, because the user may have provided the same
				 * read buffer as the unread buffer too (for performance reasons). */
				handle_priv->pos -= unread_len;
				handle_priv->info.read_info.done_cb(handle_priv->pos, policy->state);
				memmove(unread_buf, handle_priv->info.read_info.buf + handle_priv->pos, unread_len);
			} else {
				handle_priv->info.read_info.done_cb(handle_priv->pos, policy->state);
			}

			asyncio_refcount_release(&handle_priv->refcount);
			*continued = 0;
			return;
		}
	}

	*continued = 1;
}

static void streams_write_cb(const struct asyncio_fdevents_callback_info *info, int *continued)
{
	struct asyncio_streams_handle_priv *handle_priv;
	const struct asyncio_streams_policy *policy;
	size_t wrlen = 0;

	handle_priv = info->arg;
	policy = &handle_priv->policy;

	if (policy->is_errored(info->revinfo, policy->state)) {
		policy->error_cb(policy->state);
		asyncio_refcount_release(&handle_priv->refcount);
		*continued = 0;
		return;
	}

	if (policy->write_cb(handle_priv->info.write_info.buf + handle_priv->pos, handle_priv->len - handle_priv->pos, &wrlen, policy->state) != 0) {
		policy->error_cb(policy->state);
		asyncio_refcount_release(&handle_priv->refcount);
		*continued = 0;
		return;
	}

	if (wrlen > handle_priv->len - handle_priv->pos) {
		ASYNCIO_ERROR("Buffevents wrlen larger than available size.\n");
		policy->error_cb(policy->state);
		asyncio_refcount_release(&handle_priv->refcount);
		*continued = 0;
		return;
	}

	handle_priv->pos += wrlen;

	if (handle_priv->pos == handle_priv->len) {
		handle_priv->info.write_info.done_cb(policy->state);
		asyncio_refcount_release(&handle_priv->refcount);
		*continued = 0;
		return;
	}

	*continued = 1;
}

/* PUBLIC */
int asyncio_streams_read(uint8_t *buf, size_t len, void (*done_cb)(size_t len, void *state), const struct asyncio_streams_policy *policy, struct asyncio_streams_handle **handlep)
{
	struct asyncio_streams_handle_priv *handle_priv;
	struct asyncio_fdevents_listen_info listen_info;
	int oldstate;

	ASYNCIO_DISABLE_CANCELLATIONS(&oldstate);
	handle_priv = asyncio_safe_malloc(1, sizeof *handle_priv);
	
	if (handle_priv == NULL) {
		ASYNCIO_ERROR("Failed to malloc streams handle_priv.\n");
		ASYNCIO_RESTORE_CANCELSTATE(oldstate);
		return -1;
	}

	/* One reference for caller, one for streams module. */
	if (asyncio_refcount_init(&handle_priv->refcount, handle_priv, streams_handle_destructor, 2) != 0) {
		ASYNCIO_ERROR("Failed to init streams handle refcount.\n");
		asyncio_safe_free(handle_priv);
		ASYNCIO_RESTORE_CANCELSTATE(oldstate);
		return -1;
	}

	handle_priv->pub.priv = handle_priv;
	handle_priv->pub.wait = streams_handle_wait;
	handle_priv->pub.cancel = streams_handle_cancel;
	handle_priv->pub.acquire = streams_handle_acquire;
	handle_priv->pub.release = streams_handle_release;

	handle_priv->pos = 0;
	handle_priv->len = len;
	handle_priv->info.read_info.buf = buf;
	handle_priv->info.read_info.done_cb = done_cb;
	handle_priv->policy = *policy;

	listen_info.fd = policy->fd;
	listen_info.evinfo = policy->read_evinfo;
	listen_info.cb = streams_read_cb;
	listen_info.cancelled_cb = streams_read_cancelled_cb;
	listen_info.arg = handle_priv;
	listen_info.threadpool_flags = policy->threadpool_flags;

	if (policy->eventloop->listen(policy->eventloop, &listen_info, &handle_priv->evhandle) != 0) {
		ASYNCIO_ERROR("Failed to listen on eventloop for stream.\n");
		asyncio_refcount_deinit(&handle_priv->refcount);
		asyncio_safe_free(handle_priv);
		ASYNCIO_RESTORE_CANCELSTATE(oldstate);
		return -1;
	}

	*handlep = &handle_priv->pub;
	ASYNCIO_RESTORE_CANCELSTATE(oldstate);
	return 0;
}

/* PUBLIC */
int asyncio_streams_write(const uint8_t *buf, size_t len, void (*done_cb)(void *state), const struct asyncio_streams_policy *policy, struct asyncio_streams_handle **handlep)
{
	struct asyncio_streams_handle_priv *handle_priv;
	struct asyncio_fdevents_listen_info listen_info;
	int oldstate;

	ASYNCIO_DISABLE_CANCELLATIONS(&oldstate);
	handle_priv = asyncio_safe_malloc(1, sizeof *handle_priv);

	if (handle_priv == NULL) {
		ASYNCIO_ERROR("Failed to malloc streams handle_priv.\n");
		ASYNCIO_RESTORE_CANCELSTATE(oldstate);
		return -1;
	}

	/* One reference for caller, one for streams module. */
	if (asyncio_refcount_init(&handle_priv->refcount, handle_priv, streams_handle_destructor, 2) != 0) {
		ASYNCIO_ERROR("Failed to init streams handle refcount.\n");
		asyncio_safe_free(handle_priv);
		ASYNCIO_RESTORE_CANCELSTATE(oldstate);
		return -1;
	}

	handle_priv->pub.priv = handle_priv;
	handle_priv->pub.wait = streams_handle_wait;
	handle_priv->pub.cancel = streams_handle_cancel;
	handle_priv->pub.acquire = streams_handle_acquire;
	handle_priv->pub.release = streams_handle_release;

	handle_priv->pos = 0;
	handle_priv->len = len;
	handle_priv->info.write_info.buf = buf;
	handle_priv->info.write_info.done_cb = done_cb;
	handle_priv->policy = *policy;

	listen_info.fd = policy->fd;
	listen_info.evinfo = policy->write_evinfo;
	listen_info.cb = streams_write_cb;
	listen_info.cancelled_cb = streams_write_cancelled_cb;
	listen_info.arg = handle_priv;
	listen_info.threadpool_flags = policy->threadpool_flags;

	if (policy->eventloop->listen(policy->eventloop, &listen_info, &handle_priv->evhandle) != 0) {
		ASYNCIO_ERROR("Failed to listen on eventloop for stream.\n");
		asyncio_refcount_deinit(&handle_priv->refcount);
		asyncio_safe_free(handle_priv);
		ASYNCIO_RESTORE_CANCELSTATE(oldstate);
		return -1;
	}

	*handlep = &handle_priv->pub;
	ASYNCIO_RESTORE_CANCELSTATE(oldstate);
	return 0;
}
