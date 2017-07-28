#include <stddef.h>
#include <limits.h>

#include "asyncio_buffevents.h"
#include "asyncio_fdevents.h"
#include "buffevents_priv.h"
#include "safe_malloc.h"
#include "threading.h"
#include "refcounts.h"
#include "logging.h"

/* PROTOTYPES */
static int buffevents_handle_wait(const struct asyncio_buffevents_handle *handle); /* PUBLIC */
static int buffevents_handle_cancel(const struct asyncio_buffevents_handle *handle); /* PUBLIC */
static int buffevents_handle_acquire(const struct asyncio_buffevents_handle *handle); /* PUBLIC */
static void buffevents_handle_release(const struct asyncio_buffevents_handle *handle); /* PUBLIC */

static void buffevents_handle_destructor(void *instance);
static void buffevents_read_cancelled_cb(void *arg);
static void buffevents_write_cancelled_cb(void *arg);
static void buffevents_read_cb(const struct asyncio_fdevents_callback_info *info, int *continued);
static void buffevents_write_cb(const struct asyncio_fdevents_callback_info *info, int *continued);
/* END PROTOTYPES */

/* PUBLIC */
static int buffevents_handle_wait(const struct asyncio_buffevents_handle *handle)
{
	/* Do not disable cancellations while waiting.
	 * The fdevents takes care of taking the oldstate and passing it to events module. */
	return handle->priv->evhandle->wait(handle->priv->evhandle);
}

/* PUBLIC */
static int buffevents_handle_cancel(const struct asyncio_buffevents_handle *handle)
{
	int oldstate;
	int rc;

	ASYNCIO_DISABLE_CANCELLATIONS(&oldstate);
	rc = handle->priv->evhandle->cancel(handle->priv->evhandle);
	ASYNCIO_RESTORE_CANCELSTATE(oldstate);

	return rc;
}

/* PUBLIC */
static int buffevents_handle_acquire(const struct asyncio_buffevents_handle *handle)
{
	int oldstate;
	int rc;

	ASYNCIO_DISABLE_CANCELLATIONS(&oldstate);
	rc = asyncio_refcount_acquire(&handle->priv->refcount);
	ASYNCIO_RESTORE_CANCELSTATE(oldstate);

	return rc;
}

/* PUBLIC */
static void buffevents_handle_release(const struct asyncio_buffevents_handle *handle)
{
	int oldstate;

	ASYNCIO_DISABLE_CANCELLATIONS(&oldstate);
	asyncio_refcount_release(&handle->priv->refcount);
	ASYNCIO_RESTORE_CANCELSTATE(oldstate);
}

static void buffevents_handle_destructor(void *arg)
{
	struct asyncio_buffevents_handle_priv *handle_priv;

	handle_priv = arg;
	handle_priv->evhandle->release(handle_priv->evhandle);
	asyncio_safe_free(handle_priv);
}

static void buffevents_read_cancelled_cb(void *arg)
{
	struct asyncio_buffevents_handle_priv *handle_priv;

	handle_priv = arg;

	if (handle_priv->info.read_info.cancelled_cb != NULL)
		handle_priv->info.read_info.cancelled_cb(handle_priv->info.read_info.arg);

	/* Release our module's reference to the buffevents_handle because
	 * it won't happen in the read/write callbacks (we got cancelled so these won't get called).
	 * Note that user may still be holding a reference. */
	asyncio_refcount_release(&handle_priv->refcount);
}

static void buffevents_write_cancelled_cb(void *arg)
{
	struct asyncio_buffevents_handle_priv *handle_priv;

	handle_priv = arg;

	if (handle_priv->info.write_info.cancelled_cb != NULL)
		handle_priv->info.write_info.cancelled_cb(handle_priv->info.write_info.arg);

	/* Release our module's reference to the fdevents_handle because
	 * it won't happen in the read/write callbacks (we got cancelled so these won't get called).
	 * Note that user may still be holding a reference. */
	asyncio_refcount_release(&handle_priv->refcount);
}

static void buffevents_read_cb(const struct asyncio_fdevents_callback_info *info, int *continued)
{
	struct asyncio_buffevents_handle_priv *handle_priv;
	const struct asyncio_buffevents_read_info *read_info;
	size_t rdlen = 0;

	handle_priv = info->arg;
	read_info = &handle_priv->info.read_info;

	if (read_info->is_errored(info->revinfo)) {
		read_info->error_cb(read_info->arg);
		asyncio_refcount_release(&handle_priv->refcount);
		*continued = 0;
		return;
	}

	if (read_info->read_cb(info->fd, read_info->buf + handle_priv->pos, read_info->len - handle_priv->pos, &rdlen, read_info->arg) != 0) {
		read_info->error_cb(read_info->arg);
		asyncio_refcount_release(&handle_priv->refcount);
		*continued = 0;
		return;
	}

	if (rdlen > read_info->len - handle_priv->pos) {
		ASYNCIO_ERROR("Buffevents rdlen larger than available size.\n");
		read_info->error_cb(read_info->arg);
		asyncio_refcount_release(&handle_priv->refcount);
		*continued = 0;
		return;
	}

	handle_priv->pos += rdlen;

	if (handle_priv->pos == read_info->len) {
		read_info->done_cb(read_info->buf, read_info->len, read_info->arg);
		asyncio_refcount_release(&handle_priv->refcount);
		*continued = 0;
		return;
	}

	if (read_info->until_cb != NULL) {
		/* If we have an until_cb, shouldn't we be reading 1 byte at a time?
		 * Though, I suppose this is up to the user, and this gets controlled
		 * in the read_cb. */
		if (read_info->until_cb(read_info->buf, handle_priv->pos, read_info->arg)) {
			read_info->done_cb(read_info->buf, handle_priv->pos, read_info->arg);
			asyncio_refcount_release(&handle_priv->refcount);
			*continued = 0;
			return;
		}
	}

	*continued = 1;
}

static void buffevents_write_cb(const struct asyncio_fdevents_callback_info *info, int *continued)
{
	struct asyncio_buffevents_handle_priv *handle_priv;
	const struct asyncio_buffevents_write_info *write_info;
	size_t wrlen = 0;

	handle_priv = info->arg;
	write_info = &handle_priv->info.write_info;

	if (write_info->is_errored(info->revinfo)) {
		write_info->error_cb(write_info->arg);
		asyncio_refcount_release(&handle_priv->refcount);
		*continued = 0;
		return;
	}

	if (write_info->write_cb(info->fd, write_info->buf + handle_priv->pos, write_info->len - handle_priv->pos, &wrlen, write_info->arg) != 0) {
		write_info->error_cb(write_info->arg);
		asyncio_refcount_release(&handle_priv->refcount);
		*continued = 0;
		return;
	}

	if (wrlen > write_info->len - handle_priv->pos) {
		ASYNCIO_ERROR("Buffevents wrlen larger than available size.\n");
		write_info->error_cb(write_info->arg);
		asyncio_refcount_release(&handle_priv->refcount);
		*continued = 0;
		return;
	}

	handle_priv->pos += wrlen;

	if (handle_priv->pos == write_info->len) {
		write_info->done_cb(write_info->buf_to_free, write_info->len, write_info->arg);
		asyncio_refcount_release(&handle_priv->refcount);
		*continued = 0;
		return;
	}

	*continued = 1;
}

/* PUBLIC */
int asyncio_buffevents_read(const struct asyncio_buffevents_read_info *info, struct asyncio_buffevents_handle **handlep)
{
	struct asyncio_buffevents_handle_priv *handle_priv;
	struct asyncio_fdevents_listen_info listen_info;
	int oldstate;

	ASYNCIO_DISABLE_CANCELLATIONS(&oldstate);
	handle_priv = asyncio_safe_malloc(1, sizeof *handle_priv);
	
	if (handle_priv == NULL) {
		ASYNCIO_ERROR("Failed to malloc buffevents handle_priv.\n");
		ASYNCIO_RESTORE_CANCELSTATE(oldstate);
		return -1;
	}

	/* One reference for caller, one for buffevents module. */
	if (asyncio_refcount_init(&handle_priv->refcount, handle_priv, buffevents_handle_destructor, 2) != 0) {
		ASYNCIO_ERROR("Failed to init buffevents handle refcount.\n");
		asyncio_safe_free(handle_priv);
		ASYNCIO_RESTORE_CANCELSTATE(oldstate);
		return -1;
	}

	handle_priv->pub.priv = handle_priv;
	handle_priv->pub.wait = buffevents_handle_wait;
	handle_priv->pub.cancel = buffevents_handle_cancel;
	handle_priv->pub.acquire = buffevents_handle_acquire;
	handle_priv->pub.release = buffevents_handle_release;

	handle_priv->pos = 0;
	handle_priv->info.read_info = *info;

	listen_info.fd = info->fd;
	listen_info.evinfo = info->evinfo;
	listen_info.cb = buffevents_read_cb;
	listen_info.cancelled_cb = buffevents_read_cancelled_cb;
	listen_info.arg = handle_priv;
	listen_info.threadpool_flags = info->threadpool_flags;

	if (info->eventloop->listen(info->eventloop, &listen_info, &handle_priv->evhandle) != 0) {
		ASYNCIO_ERROR("Failed to listen on eventloop for buffevent.\n");
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
int asyncio_buffevents_write(const struct asyncio_buffevents_write_info *info, struct asyncio_buffevents_handle **handlep)
{
	struct asyncio_buffevents_handle_priv *handle_priv;
	struct asyncio_fdevents_listen_info listen_info;
	int oldstate;

	ASYNCIO_DISABLE_CANCELLATIONS(&oldstate);
	handle_priv = asyncio_safe_malloc(1, sizeof *handle_priv);

	if (handle_priv == NULL) {
		ASYNCIO_ERROR("Failed to malloc buffevents handle_priv.\n");
		ASYNCIO_RESTORE_CANCELSTATE(oldstate);
		return -1;
	}

	/* One reference for caller, one for buffevents module. */
	if (asyncio_refcount_init(&handle_priv->refcount, handle_priv, buffevents_handle_destructor, 2) != 0) {
		ASYNCIO_ERROR("Failed to init buffevents handle refcount.\n");
		asyncio_safe_free(handle_priv);
		ASYNCIO_RESTORE_CANCELSTATE(oldstate);
		return -1;
	}

	handle_priv->pub.priv = handle_priv;
	handle_priv->pub.wait = buffevents_handle_wait;
	handle_priv->pub.cancel = buffevents_handle_cancel;
	handle_priv->pub.acquire = buffevents_handle_acquire;
	handle_priv->pub.release = buffevents_handle_release;

	handle_priv->pos = 0;
	handle_priv->info.write_info = *info;

	listen_info.fd = info->fd;
	listen_info.evinfo = info->evinfo;
	listen_info.cb = buffevents_write_cb;
	listen_info.cancelled_cb = buffevents_write_cancelled_cb;
	listen_info.arg = handle_priv;
	listen_info.threadpool_flags = info->threadpool_flags;

	if (info->eventloop->listen(info->eventloop, &listen_info, &handle_priv->evhandle) != 0) {
		ASYNCIO_ERROR("Failed to listen on eventloop for buffevent.\n");
		asyncio_refcount_deinit(&handle_priv->refcount);
		asyncio_safe_free(handle_priv);
		ASYNCIO_RESTORE_CANCELSTATE(oldstate);
		return -1;
	}

	*handlep = &handle_priv->pub;
	ASYNCIO_RESTORE_CANCELSTATE(oldstate);
	return 0;
}
