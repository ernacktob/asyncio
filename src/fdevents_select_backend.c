#include <sys/select.h>

#include "asyncio_fdevents_select.h"
#include "fdevents_priv.h"

/* STRUCT DEFINITIONS */
struct select_info {
	int max_nfds;
	int nfds;
	fd_set readfds;
	fd_set writefds;
	fd_set errorfds;

	/* scratch space */
	int scratch_nfds;
	fd_set scratch_readfds;
	fd_set scratch_writefds;
	fd_set scratch_errorfds;
};
/* END STRUCT DEFINITIONS */

/* PROTOTYPES */
static int is_valid_fd(int fd);

static int fdevents_select_init_evinfo_revinfo(const void *evinfo, void **evinfop, void **revinfop);
static void fdevents_select_cleanup_evinfo_revinfo(void *evinfo, void *revinfo);
static int fdevents_select_acquire_evinfo_locked(void *backend_data, int fd, const void *evinfo);
static void fdevents_select_release_evinfo_locked(void *backend_data, int fd, const void *evinfo);
static int fdevents_select_match_evinfo_locked(const void *backend_data, int fd, const void *evinfo, void *revinfo);

static int fdevents_select_insert_fd_locked(void *backend_data, int fd, const void *evinfo);
static void fdevents_select_remove_fd_locked(void *backend_data, int fd);

static int fdevents_select_initialize_eventloop_thread(void *backend_data);
static int fdevents_select_wait_for_events(void *backend_data);
static void fdevents_select_process_changed_events_locked(void *backend_data);
static void fdevents_select_scan_for_events_init_locked(void *backend_data);
static int fdevents_select_scan_for_events_next_locked(void *backend_data, int *fd);
static void fdevents_select_continue_eventloop_thread_locked(void *backend_data);
static void fdevents_select_cleanup_eventloop_thread(void *backend_data);

static int fdevents_select_init_backend_data(void **backend_data, size_t max_nfds, int clearwakefd);
static void fdevents_select_cleanup_backend_data(void *backend_data);
/* END PROTOTYPES */

/* GLOBALS */
struct fdevents_backend_ops asyncio_fdevents_select_backend_ops {
	fdevents_select_init_evinfo_revinfo,
	fdevents_select_cleanup_evinfo_revinfo,
	fdevents_select_acquire_evinfo_locked,
	fdevents_select_release_evinfo_locked,
	fdevents_select_match_evinfo_locked,

	fdevents_select_insert_fd_locked,
	fdevents_select_remove_fd_locked,

	fdevents_select_initialize_eventloop_thread,
	fdevents_select_wait_for_events,
	fdevents_select_process_changed_events_locked,
	fdevents_select_scan_for_events_init_locked,
	fdevents_select_scan_for_events_next_locked,
	fdevents_select_continue_eventloop_thread_locked,
	fdevents_select_cleanup_eventloop_thread,

	fdevents_select_init_backend_data,
	fdevents_select_cleanup_backend_data
};
/* END GLOBALS */

static int is_valid_fd(int fd)
{
	if (fd >= 0 && fd < FD_SETSIZE)
		return 1;

	return 0;
}

static int fdevents_select_init_evinfo_revinfo(const void *evinfo, void **evinfop, void **revinfop)
{
	const struct asyncio_fdevents_select_evinfo *the_evinfo;
	struct asyncio_fdevents_select_evinfo *select_evinfo, *select_revinfo;

	select_evinfo = asyncio_safe_malloc(1, sizeof *select_evinfo);

	if (select_evinfo == NULL) {
		ASYNCIO_ERROR("Failed to malloc select_evinfo.\n");
		return -1;
	}

	select_revinfo = asyncio_safe_malloc(1, sizeof *select_revinfo);

	if (select_revinfo == NULL) {
		ASYNCIO_ERROR("Failed to malloc select_revinfo.\n");
		asyncio_safe_free(select_evinfo);
		return -1;
	}

	the_evinfo = evinfo;
	select_evinfo->events = the_evinfo->events;

	*evinfop = select_evinfo;
	*revinfo = select_revinfo;
	return 0;

}

static void fdevents_select_cleanup_evinfo_revinfo(void *evinfo, void *revinfo)
{
	asyncio_safe_free(evinfo);
	asyncio_safe_free(revinfo);
}

static int fdevents_select_acquire_evinfo_locked(void *backend_data, int fd, const void *evinfo)
{
	struct select_info *selectinfo;
	const struct asyncio_fdevents_select_evinfo *select_evinfo;

	selectinfo = backend_data;
	select_evinfo = evinfo;

	if (!is_valid_fd(fd)) {
		ASYNCIO_ERROR("Invalid fd given for fdevents select backend.\n");
		return -1;
	}

	/* Check that this event has not been set yet. */
	if ((evinfo->events & ASYNCIO_FDEVENTS_SELECT_READABLE) && FD_ISSET(fd, &selectinfo->readfds))
		return -1;
	else if ((evinfo->events & ASYNCIO_FDEVENTS_SELECT_WRITABLE) && FD_ISSET(fd, &selectinfo->writefds))
		return -1;
	else if ((evindo->events & ASYNCIO_FDEVENTS_SELECT_ERRORED) && FD_ISSET(fd, &selectinfo->errorfds))
		return -1;

	if (evinfo->events & ASYNCIO_FDEVENTS_SELECT_READABLE)
		FD_SET(fd, &selectinfo->readfds);

	if (evinfo->events & ASYNCIO_FDEVENTS_SELECT_WRITABLE)
		FD_SET(fd, &selectinfo->writefds);

	if (evinfo->events & ASYNCIO_FDEVENTS_SELECT_ERRORED)
		FD_SET(fd, &selectinfo->writefds);

	return 0;
}

static void fdevents_select_release_evinfo_locked(void *backend_data, int fd, const void *evinfo)
{
	struct select_info *selectinfo;
	const struct asyncio_fdevents_select_evinfo *select_evinfo;

	selectinfo = backend_data;
	select_evinfo = evinfo;

	if (!is_valid_fd(fd)) {
		ASYNCIO_ERROR("Invalid fd given for fdevents select backend.\n");
		return -1;
	}

	if ((evinfo->events & ASYNCIO_FDEVENTS_SELECT_READABLE) && !FD_ISSET(fd, &selectinfo->readfds)) {
		ASYNCIO_ERROR("Tried to remove fd whose event was not set in select backend.\n");
		return -1;
	} else if ((evinfo->events & ASYNCIO_FDEVENTS_SELECT_WRITABLE) && !FD_ISSET(fd, &selectinfo->writefds)) {
		ASYNCIO_ERROR("Tried to remove fd whose event was not set in select backend.\n");
		return -1;
	} else if ((evindo->events & ASYNCIO_FDEVENTS_SELECT_ERRORED) && !FD_ISSET(fd, &selectinfo->errorfds)) {
		ASYNCIO_ERROR("Tried to remove fd whose event was not set in select backend.\n");
		return -1;
	}

	if (evinfo->events & ASYNCIO_FDEVENTS_SELECT_READABLE)
		FD_CLR(fd, &selectinfo->readfds);

	if (evinfo->events & ASYNCIO_FDEVENTS_SELECT_WRITABLE)
		FD_CLR(fd, &selectinfo->writefds);

	if (evinfo->events & ASYNCIO_FDEVENTS_SELECT_ERRORED)
		FD_CLR(fd, &selectinfo->writefds);
}

static int fdevents_select_match_evinfo_locked(const void *backend_data, int fd, const void *evinfo, void *revinfo)
{
	struct select_info *selectinfo;
	const struct asyncio_fdevents_select_evinfo *select_evinfo;
	int revents = 0;

	selectinfo = backend_data;
	select_evinfo = evinfo;

	if (!is_valid_fd(fd)) {
		ASYNCIO_ERROR("Invalid fd given for fdevents select backend.\n");
		return -1;
	}

	if (FD_ISSET(fd, &selectinfo->readfds))
		revents |= ASYNCIO_FDEVENTS_SELECT_READABLE;

	if (FD_ISSET(fd, &selectinfo->writefds))
		revents |= ASYNCIO_FDEVENTS_SELECT_WRITABLE;

	if (FD_ISSET(fd, &selectinfo->errorfds))
		revents |= ASYNCIO_FDEVENTS_SELECT_ERRORED;

	if (evinfo->events & revents) {
		revinfo->events = revents;
		return 1;	/* Return 1 for a match */
	}

	return 0;
}

static int fdevents_select_insert_fd_locked(void *backend_data, int fd, const void *evinfo)
{
	struct select_info *selectinfo;
	const struct asyncio_fdevents_select_evinfo *select_evinfo;
	int revents = 0;

	selectinfo = backend_data;
	select_evinfo = evinfo;

	if (!is_valid_fd(fd)) {
		ASYNCIO_ERROR("Invalid fd given for fdevents select backend.\n");
		return -1;
	}


}

static void fdevents_select_remove_fd_locked(void *backend_data, int fd);

static int fdevents_select_initialize_eventloop_thread(void *backend_data);
static int fdevents_select_wait_for_events(void *backend_data);
static void fdevents_select_process_changed_events_locked(void *backend_data);
static void fdevents_select_scan_for_events_init_locked(void *backend_data);
static int fdevents_select_scan_for_events_next_locked(void *backend_data, int *fd);
static void fdevents_select_continue_eventloop_thread_locked(void *backend_data);
static void fdevents_select_cleanup_eventloop_thread(void *backend_data);

static int fdevents_select_init_backend_data(void **backend_data, size_t max_nfds, int clearwakefd);
static void fdevents_select_cleanup_backend_data(void *backend_data);
