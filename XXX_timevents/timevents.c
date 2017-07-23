#include "events.h"
#include "threading.h"
#include "safe_malloc.h"
#include "logging.h"

/* PROTOTYPES */
static int init_timeventloop_private_data(const struct asyncio_timevents_options *options, struct asyncio_timevents_loop_priv *timeventloop_priv);
/* END PROTOTYPES */

/* GLOBALS */
static struct events_backend timevents_backend = EVENTS_BACKEND_INITIALIZER;
/* END GLOBALS */

static int init_timeventloop_private_data(const struct asyncio_timevents_options *options, struct asyncio_timevents_loop_priv *timeventloop_priv)
{

}

/* PUBLIC */
int asyncio_timevents_init(void)
{
	int oldstate;
	int rc;

	ASYNCIO_DISABLE_CANCELLATIONS(&oldstate);
	rc = asyncio_events_backend_init(&timevents_backend);
	ASYNCIO_RESTORE_CANCELSTATE(oldstate);

	return rc;
}

/* PUBLIC */
int asyncio_timevents_eventloop(const struct asyncio_timevents_options *options, struct asyncio_timevents_loop **eventloop)
{
	struct asyncio_timevents_loop_priv *timeventloop_priv;
	int oldstate;

	ASYNCIO_DISABLE_CANCELLATIONS(&oldstate);
	timeventloop_priv = asyncio_safe_malloc(1, sizeof *timeventloop_priv);

	if (timeventloop_priv == NULL) {
		ASYNCIO_ERROR("Failed to malloc timeventloop_priv.\n");
		ASYNCIO_RESTORE_CANCELSTATE(oldstate);
		return -1;
	}

	if (asyncio_events_backend_eventloop(&timevents_backend, &timeventloop_priv->base) != 0) {
		ASYNCIO_ERROR("Failed to create backend base events_loop.\n");
		asyncio_safe_free(timeventloop_priv);
		ASYNCIO_RESTORE_CANCELSTATE(oldstate);
		return -1;
	}

	timeventloop_priv->base.instance = timeventloop_priv;
	timeventloop_priv->base.backend_callback = timevents_handle_callback;

	timeventloop_priv->base.backend_insert_events_handle_locked = timevents_insert_handle_locked;
	timeventloop_priv->base.backend_remove_events_handle_locked = timevents_remove_handle_locked;
	timeventloop_priv->base.backend_cleanup_events_handle = timevents_cleanup_events_handle;

	timeventloop_priv->base.backend_initialize_eventloop_thread = timevents_initialize_eventloop_thread;
	timeventloop_priv->base.backend_wait_for_events = timevents_wait_for_events;
	timeventloop_priv->base.backend_process_changed_events_locked = timevents_process_changed_events_locked;
	timeventloop_priv->base.backend_scan_for_events_locked = timevents_scan_for_events_locked;
	timeventloop_priv->base.backend_continue_eventloop_thread_locked = timevents_continue_eventloop_thread_locked;
	timeventloop_priv->base.backend_cleanup_eventloop_thread = timevents_cleanup_eventloop_thread;

	timeventloop_priv->base.backend_wakeup_eventloop_locked = timevents_wakeup_eventloop_locked;
	timeventloop_priv->base.backend_clearwake_eventloop_locked = timevents_clearwake_eventloop_locked;
	timeventloop_priv->base.backend_cleanup_eventloop = timevents_cleanup_eventloop;

	timeventloop_priv->pub.acquire = timevents_eventloop_acquire;
	timeventloop_priv->pub.release = timevents_eventloop_release;
	timeventloop_priv->pub.listen = timevents_eventloop_listen;

	if (init_timeventloop_private_data(options, timeventloop_priv) != 0) {
		ASYNCIO_ERROR("Failed to initialize timeventloop private data.\n");
		timeventloop_priv->base.release(&timeventloop_priv->base);
		asyncio_safe_free(timeventloop_priv);
		ASYNCIO_RESTORE_CANCELSTATE(oldstate);
		return -1;
	}

	*eventloop = &timeventloop_priv->pub;
	ASYNCIO_RESTORE_CANCELSTATE(oldstate);
	return 0;
}

/* PUBLIC */
void asyncio_timevents_cleanup(void)
{
	int oldstate;

	ASYNCIO_DISABLE_CANCELLATIONS(&oldstate);
	asyncio_events_backend_cleanup(&timevents_backend);
	ASYNCIO_RESTORE_CANCELSTATE(oldstate);
}
