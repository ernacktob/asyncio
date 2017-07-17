#include <stddef.h>
#include <poll.h>

#include "asyncio_fdevents_poll.h"
#include "fdevents_priv.h"
#include "hashtable.h"
#include "constants.h"
#include "compile_time_assert.h"

COMPILE_TIME_ASSERT(NFDST_MAX <= SIZET_MAX);

/* STRUCT DEFINITIONS */
struct poll_info {
	nfds_t max_nfds;
	nfds_t nfds;
	struct pollfd *fds;
	struct hashtable *fd_map;

	/* scratch space */
	struct pollfd *scratch_fds;
	nfds_t scratch_nfds;
	nfds_t scratch_index;
};
/* END STRUCT DEFINITIONS */

/* PROTOTYPES */
static void copy_fdevents_scratch_locked(struct poll_info *pollinfo);

static int fdevents_poll_init_evinfo_revinfo(const void *evinfo, void **evinfop, void **revinfop);
static void fdevents_poll_cleanup_evinfo_revinfo(void *evinfo, void *revinfo);
static int fdevents_poll_acquire_evinfo_locked(void *backend_data, int fd, const void *evinfo);
static void fdevents_poll_release_evinfo_locked(void *backend_data, int fd, const void *evinfo);
static int fdevents_poll_match_evinfo_locked(const void *backend_data, int fd, const void *evinfo, void *revinfo);

static int fdevents_poll_insert_fd_locked(void *backend_data, int fd, const void *evinfo);
static void fdevents_poll_remove_fd_locked(void *backend_data, int fd);

static int fdevents_poll_initialize_eventloop_thread(void *backend_data);
static int fdevents_poll_wait_for_events(void *backend_data);
static void fdevents_poll_process_changed_events_locked(void *backend_data);
static void fdevents_poll_scan_for_events_init_locked(void *backend_data);
static int fdevents_poll_scan_for_events_next_locked(void *backend_data, int *fd);
static void fdevents_poll_continue_eventloop_thread_locked(void *backend_data);
static void fdevents_poll_cleanup_eventloop_thread(void *backend_data);

static int fdevents_poll_init_backend_data(void **backend_data, size_t max_nfds, int clearwakefd);
static void fdevents_poll_cleanup_backend_data(void *backend_data);
/* END PROTOTYPES */

/* GLOBALS */
struct fdevents_backend_ops asyncio_fdevents_poll_backend_ops = {
	fdevents_poll_init_evinfo_revinfo,
	fdevents_poll_cleanup_evinfo_revinfo,
	fdevents_poll_acquire_evinfo_locked,
	fdevents_poll_release_evinfo_locked,
	fdevents_poll_match_evinfo_locked,

	fdevents_poll_insert_fd_locked,
	fdevents_poll_remove_fd_locked,

	fdevents_poll_initialize_eventloop_thread,
	fdevents_poll_wait_for_events,
	fdevents_poll_process_changed_events_locked,
	fdevents_poll_scan_for_events_init_locked,
	fdevents_poll_scan_for_events_next_locked,
	fdevents_poll_continue_eventloop_thread_locked,
	fdevents_poll_cleanup_eventloop_thread,

	fdevents_poll_init_backend_data,
	fdevents_poll_cleanup_backend_data
};
/* END GLOBALS */

static void copy_fdevents_scratch_locked(struct poll_info *pollinfo)
{
	nfds_t i;

	for (i = 0; i < pollinfo->nfds; i++)
		pollinfo->scratch_fds[i] = pollinfo->fds[i];

	pollinfo->scratch_nfds = pollinfo->nfds;
}

static int fdevents_poll_init_evinfo_revinfo(const void *evinfo, void **evinfop, void **revinfop)
{
	const struct asyncio_fdevents_poll_evinfo *the_evinfo;
	struct asyncio_fdevents_poll_evinfo *pollevinfo, *pollrevinfo;

	pollevinfo = asyncio_safe_malloc(1, sizeof *pollevinfo);

	if (pollevinfo == NULL) {
		ASYNCIO_ERROR("Failed to malloc pollevinfo.\n");
		return -1;
	}

	pollrevinfo = asyncio_safe_malloc(1, sizeof *pollrevinfo);

	if (pollrevinfo == NULL) {
		ASYNCIO_ERROR("Failed to malloc pollrevinfo.\n");
		asyncio_safe_free(pollevinfo);
		return -1;
	}

	the_evinfo = evinfo;
	pollevinfo->events = the_evinfo->events;

	*evinfop = pollevinfo;
	*revinfo = pollrevinfo;
	return 0;
}

static void fdevents_poll_cleanup_evinfo_revinfo(void *evinfo, void *revinfo)
{
	asyncio_safe_free(evinfo);
	asyncio_safe_free(revinfo);
}

static int fdevents_poll_acquire_evinfo_locked(void *backend_data, int fd, const void *evinfo)
{
	struct poll_info *pollinfo;
	const struct asyncio_fdevents_poll_evinfo *pollevinfo;
	size_t index;

	pollinfo = backend_data;
	pollevinfo = evinfo;

	if (!asyncio_hashtable_lookup(&pollinfo->fd_map, fd, &index)) {
		ASYNCIO_ERROR("Failed to find fd in pollinfo hashtable. Should not happen.\n");
		return -1;
	}

	/* Check that the bits have not been set yet. */
	if (pollinfo->fds[index].events & pollevinfo->events)
		return -1;

	pollinfo->fds[index].events |= pollevinfo->events;
	return 0;
}

static void fdevents_poll_release_evinfo_locked(void *backend_data, int fd, const void *evinfo)
{
	struct poll_info *pollinfo;
	const struct asyncio_fdevents_poll_evinfo *pollevinfo;
	size_t index;

	pollinfo = backend_data;
	pollevinfo = evinfo;

	if (!asyncio_hashtable_lookup(&pollinfo->fd_map, fd, &index)) {
		ASYNCIO_ERROR("Failed to find fd in pollinfo hashtable. Should not happen.\n");
		return;
	}

	/* Clear the bits related to evinfo. */
	pollinfo->fds[index].events &= ~(pollevinfo->events);
}

static int fdevents_poll_match_evinfo_locked(const void *backend_data, int fd, const void *evinfo, void *revinfo)
{
	const struct poll_info *pollinfo;
	const struct asyncio_fdevents_poll_evinfo *pollevinfo;
	struct asyncio_fdevents_poll_evinfo *pollrevinfo;
	size_t index;

	pollinfo = backend_data;
	pollevinfo = evinfo;
	pollrevinfo = revinfo;

	if (!asyncio_hashtable_lookup(&pollinfo->fd_map, fd, &index)) {
		ASYNCIO_ERROR("Failed to find fd in pollinfo hashtable. Should not happen.\n");
		return 0; /* Return 0 to indicate not match. */
	}

	/* Need to use the scratch fds here since this is the return from poll */
	if (pollinfo->scratch_fds[index].revents & (pollevinfo->events | POLLERR | POLLHUP | POLLNVAL)) {
		pollrevinfo->events = pollinfo->scratch_fds[index].revents;
		return 1;
	}

	return 0;
}

static int fdevents_poll_insert_fd_locked(void *backend_data, int fd, const void *evinfo)
{
	struct poll_info *pollinfo;
	const struct asyncio_fdevents_poll_evinfo *pollevinfo;
	size_t index;

	pollinfo = backend_data;
	pollevinfo = evinfo;

	if (pollinfo->nfds == pollinfo->max_nfds) {
		ASYNCIO_ERROR("pollinfo nfds reached max_nfds.\n");
		return -1;
	}

	/* XXX Check that is hasn't been inserted already? */

	index = (size_t)(pollinfo->nfds);

	if (asyncio_hashtable_insert(&pollinfo->fd_map, fd, index) != 0) {
		ASYNCIO_ERROR("Failed to insert fd in pollinfo hashtable.\n");
		return -1;
	}

	pollinfo->fds[index].fd = fd;
	pollinfo->fds[index].events = pollevinfo->events;
	++(pollinfo->nfds);

	return 0;
}

static void fdevents_poll_remove_fd_locked(void *backend_data, int fd)
{
	struct poll_info *pollinfo;
	size_t index, last_valid_index;

	pollinfo = backend_data;

	if (pollinfo->nfds <= 1) {
		/* Note that the clearwakefd is not inserted/removed this way, and has not hashtable entry. */
		ASYNCIO_ERROR("Tried to remove fd from pollinfo with nfds <= 1. Should not happen.\n");
		return;
	}

	if (asyncio_hashtable_lookup(&pollinfo->fd_map, fd, &index) != 0) {
		ASYNCIO_ERROR("Failed to lookup fd for pollinfo remove.\n");
		return;
	}

	asyncio_hashtable_delete(&pollinfo->fd_map, fd);
	last_valid_index = (size_t)(pollinfo->nfds - 1);

	/* Copy the last valid entry in the empty slot to keep the fds consecutive.
	 * Remap the hashtable entry to point to new index. */
	if (index < last_valid_index) {
		pollinfo->fds[index] = pollinfo->fds[last_valid_index];
		asyncio_hashtable_modify(&pollinfo->fd_map, pollinfo->fds[last_valid_index].fd, index);
	}

	--(pollinfo->nfds);
}

static int fdevents_poll_initialize_eventloop_thread(void *backend_data)
{
	struct poll_info *pollinfo;

	pollinfo = backend_data;
	copy_fdevents_scratch_locked(pollinfo);

	return 0;
}

static int fdevents_poll_wait_for_events(void *backend_data)
{
	struct poll_info *pollinfo;

	pollinfo = backend_data;

	if (poll(pollinfo->scratch_fds, pollinfo->scratch_nfds, -1) < 0) {
		ASYNCIO_SYSERROR("poll");
		return -1;
	}

	return 0;
}

static void fdevents_poll_process_changed_events_locked(void *backend_data)
{
	struct poll_info *pollinfo;

	pollinfo = backend_data;
	copy_fdevents_scratch_locked(pollinfo);
}

static void fdevents_poll_scan_for_events_init_locked(void *backend_data)
{
	struct poll_info *pollinfo;

	pollinfo = backend_data;
	pollinfo->scratch_index = 1;	/* Skip index 0, which contains clearwakefd */
}

static int fdevents_poll_scan_for_events_next_locked(void *backend_data, int *fd)
{
	struct poll_info *pollinfo;
	nfds_t i;

	pollinfo = backend_data;

	for (i = pollinfo->scratch_index; i < pollinfo->scratch_nfds; i++) {
		if (pollinfo->scratch_fds[i].revents & (pollinfo->scratch_fds[i].events | POLLERR | POLLHUP | POLLNVAL)) {
			pollinfo->scratch_index = i + 1;
			*fd = pollinfo->scratch_fds[i].fd;
			return 1; /* Return 1 to indicate we found an event. */
		}
	}

	pollinfo->scratch_index = i;
	return 0;
}

static void fdevents_poll_continue_eventloop_thread_locked(void *backend_data)
{
	struct poll_info *pollinfo;

	pollinfo = backend_data;

	/* The masked fds must be removed from the scratch fds array because poll
	 * will return on exceptions even when events is 0. */
	copy_fdevents_scratch_locked(pollinfo);
}

static void fdevents_poll_cleanup_eventloop_thread(void *backend_data)
{
	(void)backend_data;
	/* Nothing to be done. */
}

static int fdevents_poll_init_backend_data(void **backend_data, size_t max_nfds, int clearwakefd)
{
	struct poll_info *pollinfo;

	if (max_nfds > NFDST_MAX) {
		ASYNCIO_ERROR("Value for fdevents max_nfds options too large for poll backend.\n");
		rc = -1;
		goto return_;
	} else if (max_nfds < 1) {
		ASYNCIO_ERROR("Value for max_nfds must by at least 1 for poll backend.\n");
		rc = -1;
		goto return_;
	}

	pollinfo = asyncio_safe_malloc(1, sizeof *pollinfo);

	if (pollinfo == NULL) {
		ASYNCIO_ERROR("Failed to malloc struct poll_info.\n");
		rc = -1;
		goto return_;
	}

	pollinfo->max_nfds = max_nfds;
	pollinfo->nfds = 0;

	pollinfo->fds = asyncio_safe_malloc(max_nfds, sizeof *(pollinfo->fds));

	if (pollinfo->fds == NULL) {
		ASYNCIO_ERROR("Failed to malloc pollinfo fds.\n");
		rc = -1;
		goto return_pollinfo;
	}

	if (asyncio_hashtable_init(&pollinfo->fd_map, max_nfds) != 0) {
		ASYNCIO_ERROR("Failed to initialize pollinfo fd_map.\n");
		rc = -1;
		goto return_fds;
	}

	pollinfo->scratch_fds = asyncio_safe_malloc(max_nfds, sizeof *(pollinfo->scratch_fds));

	if (pollinfo->scratch_fds == NULL) {
		ASYNCIO_ERROR("Failed to malloc pollinfo scratch fds.\n");
		rc = -1;
		goto return_fd_map;
	}

	/* Initialize the clearwakefd. */
	pollinfo->fds[0].fd = clearwakefd;
	pollinfo->fds[0].events = POLLIN;
	++(pollinfo->nfds);

	*backend_data = pollinfo;
	return 0;

return_fd_map:
	asyncio_hashtable_destroy(&pollinfo->fd_map);

return_fds:
	asyncio_safe_free(pollinfo->fds);

return_pollinfo:
	asyncio_safe_free(pollinfo);

return_:
	return rc;
}

static void fdevents_poll_cleanup_backend_data(void *backend_data)
{
	struct poll_info *pollinfo;

	pollinfo = backend_data;

	asyncio_hashtable_destroy(&pollinfo->fd_map);
	asyncio_safe_free(pollinfo->scratch_fds);
	asyncio_safe_free(pollinfo->fds);
	asyncio_safe_free(pollinfo);
}
