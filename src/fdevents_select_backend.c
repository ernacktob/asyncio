#include <stddef.h>
#include <limits.h>
#include <sys/select.h>

#ifdef FD_COPY
#include <string.h>	/* for bcopy, which FD_COPY uses as macro */
#endif

#include "asyncio_fdevents_select.h"
#include "fdevents_priv.h"
#include "compile_time_assert.h"
#include "safe_malloc.h"
#include "logging.h"

/* STRUCT DEFINITIONS */
struct select_info {
	int max_nfds;
	int cur_nfds; /* This is equal to the number of fds inserted */

	int iterator_fd; /* Used to iterate over the fds after select() returns */
	int clearwakefd; /* Skip the clearwake fd during events scan. */

	/* max_fd is equal to the largest fd in the sets, used for select().
	 * We won't worry about keeping track of an accurate maximum (when the largest
	 * fd is removed, we'd have to backtrack to the second largest and require some
	 * data structure to do this like a stack or priority queue, but this is
	 * just too complicated for a small performance gain). Instead we always keep
	 * it equal to the largest fd "in history". */
	int max_fd;
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
struct fdevents_backend_ops asyncio_fdevents_select_backend_ops = {
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
	*revinfop = select_revinfo;
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
	if ((select_evinfo->events & ASYNCIO_FDEVENTS_SELECT_READABLE) && FD_ISSET(fd, &selectinfo->readfds))
		return -1;
	else if ((select_evinfo->events & ASYNCIO_FDEVENTS_SELECT_WRITABLE) && FD_ISSET(fd, &selectinfo->writefds))
		return -1;
	else if ((select_evinfo->events & ASYNCIO_FDEVENTS_SELECT_ERRORED) && FD_ISSET(fd, &selectinfo->errorfds))
		return -1;

	if (select_evinfo->events & ASYNCIO_FDEVENTS_SELECT_READABLE)
		FD_SET(fd, &selectinfo->readfds);

	if (select_evinfo->events & ASYNCIO_FDEVENTS_SELECT_WRITABLE)
		FD_SET(fd, &selectinfo->writefds);

	if (select_evinfo->events & ASYNCIO_FDEVENTS_SELECT_ERRORED)
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
		return;
	}

	if ((select_evinfo->events & ASYNCIO_FDEVENTS_SELECT_READABLE) && !FD_ISSET(fd, &selectinfo->readfds)) {
		ASYNCIO_ERROR("Tried to remove fd whose event was not set in select backend.\n");
		return;
	} else if ((select_evinfo->events & ASYNCIO_FDEVENTS_SELECT_WRITABLE) && !FD_ISSET(fd, &selectinfo->writefds)) {
		ASYNCIO_ERROR("Tried to remove fd whose event was not set in select backend.\n");
		return;
	} else if ((select_evinfo->events & ASYNCIO_FDEVENTS_SELECT_ERRORED) && !FD_ISSET(fd, &selectinfo->errorfds)) {
		ASYNCIO_ERROR("Tried to remove fd whose event was not set in select backend.\n");
		return;
	}

	if (select_evinfo->events & ASYNCIO_FDEVENTS_SELECT_READABLE)
		FD_CLR(fd, &selectinfo->readfds);

	if (select_evinfo->events & ASYNCIO_FDEVENTS_SELECT_WRITABLE)
		FD_CLR(fd, &selectinfo->writefds);

	if (select_evinfo->events & ASYNCIO_FDEVENTS_SELECT_ERRORED)
		FD_CLR(fd, &selectinfo->writefds);
}

static int fdevents_select_match_evinfo_locked(const void *backend_data, int fd, const void *evinfo, void *revinfo)
{
	const struct select_info *selectinfo;
	const struct asyncio_fdevents_select_evinfo *select_evinfo;
	struct asyncio_fdevents_select_evinfo *select_revinfo;
	int revents = 0;

	selectinfo = backend_data;
	select_evinfo = evinfo;
	select_revinfo = revinfo;

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

	if (select_evinfo->events & revents) {
		select_revinfo->events = revents;
		return 1;	/* Return 1 for a match */
	}

	return 0;
}

static int fdevents_select_insert_fd_locked(void *backend_data, int fd, const void *evinfo)
{
	struct select_info *selectinfo;
	const struct asyncio_fdevents_select_evinfo *select_evinfo;

	selectinfo = backend_data;
	select_evinfo = evinfo;

	if (!is_valid_fd(fd)) {
		ASYNCIO_ERROR("Invalid fd given for fdevents select backend.\n");
		return -1;
	}

	if (selectinfo->cur_nfds >= selectinfo->max_nfds) {
		ASYNCIO_ERROR("Reached max limit for fdevents select backend max_nfds.\n");
		return -1;
	}

	if (select_evinfo->events & ASYNCIO_FDEVENTS_SELECT_READABLE)
		FD_SET(fd, &selectinfo->readfds);

	if (select_evinfo->events & ASYNCIO_FDEVENTS_SELECT_WRITABLE)
		FD_SET(fd, &selectinfo->writefds);

	if (select_evinfo->events & ASYNCIO_FDEVENTS_SELECT_ERRORED)
		FD_SET(fd, &selectinfo->errorfds);

	/* Update max fd */
	if (fd > selectinfo->max_fd)
		selectinfo->max_fd = fd;

	++(selectinfo->cur_nfds);
	return 0;
}

static void fdevents_select_remove_fd_locked(void *backend_data, int fd)
{
	struct select_info *selectinfo;

	selectinfo = backend_data;

	if (!is_valid_fd(fd)) {
		ASYNCIO_ERROR("Invalid fd given for fdevents select backend.\n");
		return;
	}

	if (selectinfo->cur_nfds == 0) {
		ASYNCIO_ERROR("Tried to remove fd from fdevent select backend with 0 cur_nfds.\n");
		return;
	}

	/* XXX Check somehow if fd was actually in the database? But fdevents.c does that already. */
	FD_CLR(fd, &selectinfo->readfds);
	FD_CLR(fd, &selectinfo->writefds);
	FD_CLR(fd, &selectinfo->errorfds);

	/* Don't backtrack to second max fd. Too complicated. */
	--(selectinfo->cur_nfds);
}

static int fdevents_select_initialize_eventloop_thread(void *backend_data)
{
	struct select_info *selectinfo;

	selectinfo = backend_data;

	/* This is completely retarded, but apparently FD_COPY is not supported
	 * on all platforms that supposrt select(). And also doing struct equality
	 * assignment is apparently not quite standard either... Oh my goodness. */
#ifdef FD_COPY
	FD_COPY(&selectinfo->readfds, &selectinfo->scratch_readfds);
	FD_COPY(&selectinfo->writefds, &selectinfo->scratch_writefds);
	FD_COPY(&selectinfo->errorfds, &selectinfo->scratch_errorfds);
#else
	selectinfo->scratch_readfds = selectinfo->readfds;
	selectinfo->scratch_writefds = selectinfo->writefds;
	selectinfo->scratch_errorfds = selectinfo->errorfds;
#endif

	/* nfds argument to select() is max fd plus 1. */
	selectinfo->scratch_nfds = selectinfo->max_fd + 1;

	return 0;
}

static int fdevents_select_wait_for_events(void *backend_data)
{
	struct select_info *selectinfo;
	int rc;

	selectinfo = backend_data;

	rc = select(selectinfo->scratch_nfds, &selectinfo->scratch_readfds, &selectinfo->scratch_writefds, &selectinfo->scratch_errorfds, NULL);

	/* On success select() returns the number of fds in the sets. On error it returns -1. On timeout it returns 0. */
	if (rc < 0) {
		ASYNCIO_SYSERROR("select");
		return -1;
	} else if (rc == 0) {
		ASYNCIO_ERROR("select returned due to timeout, but we did not set a timeout.\n");
		return -1;
	}

	return 0;
}

static void fdevents_select_process_changed_events_locked(void *backend_data)
{
	struct select_info *selectinfo;

	selectinfo = backend_data;

#ifdef FD_COPY
	FD_COPY(&selectinfo->readfds, &selectinfo->scratch_readfds);
	FD_COPY(&selectinfo->writefds, &selectinfo->scratch_writefds);
	FD_COPY(&selectinfo->errorfds, &selectinfo->scratch_errorfds);
#else
	selectinfo->scratch_readfds = selectinfo->readfds;
	selectinfo->scratch_writefds = selectinfo->writefds;
	selectinfo->scratch_errorfds = selectinfo->errorfds;
#endif

	/* nfds argument to select() is max fd plus 1. */
	selectinfo->scratch_nfds = selectinfo->max_fd + 1;
}

static void fdevents_select_scan_for_events_init_locked(void *backend_data)
{
	struct select_info *selectinfo;

	selectinfo = backend_data;
	selectinfo->iterator_fd = 0;
}

static int fdevents_select_scan_for_events_next_locked(void *backend_data, int *fd)
{
	struct select_info *selectinfo;
	int ifd;

	selectinfo = backend_data;

	/* Also not the most efficient thing, but who cares, select isn't really meant to be efficient anyway */
	for (ifd = selectinfo->iterator_fd; ifd <= selectinfo->max_fd; ifd++) {
		/* Skip the clearwakefd, it is not a user event. */
		if (ifd == selectinfo->clearwakefd)
			continue;

		if (FD_ISSET(ifd, &selectinfo->readfds) || FD_ISSET(ifd, &selectinfo->writefds) || FD_ISSET(ifd, &selectinfo->errorfds)) {
			selectinfo->iterator_fd = ifd + 1;
			*fd = ifd;
			return 1;
		}
	}

	selectinfo->iterator_fd = ifd;
	return 0;
}

static void fdevents_select_continue_eventloop_thread_locked(void *backend_data)
{
	struct select_info *selectinfo;

	selectinfo = backend_data;

#ifdef FD_COPY
	FD_COPY(&selectinfo->readfds, &selectinfo->scratch_readfds);
	FD_COPY(&selectinfo->writefds, &selectinfo->scratch_writefds);
	FD_COPY(&selectinfo->errorfds, &selectinfo->scratch_errorfds);
#else
	selectinfo->scratch_readfds = selectinfo->readfds;
	selectinfo->scratch_writefds = selectinfo->writefds;
	selectinfo->scratch_errorfds = selectinfo->errorfds;
#endif

	/* nfds argument to select() is max fd plus 1. */
	selectinfo->scratch_nfds = selectinfo->max_fd + 1;
}

static void fdevents_select_cleanup_eventloop_thread(void *backend_data)
{
	(void)backend_data;
	/* Nothing to do */
}

static int fdevents_select_init_backend_data(void **backend_data, size_t max_nfds, int clearwakefd)
{
	struct select_info *selectinfo;
	COMPILE_TIME_ASSERT(INT_MAX > 0);

	if (max_nfds > INT_MAX) {
		ASYNCIO_ERROR("Given max_nfds value too large for select backend.\n");
		return -1;
	} else if (max_nfds < 1) {
		ASYNCIO_ERROR("The max_nfds must be at least 1 for select backend.\n");
		return -1;
	}

	if (!is_valid_fd(clearwakefd)) {
		ASYNCIO_ERROR("The clearwakefd is invalid for select backend.\n");
		return -1;
	}

	selectinfo = asyncio_safe_malloc(1, sizeof *selectinfo);

	if (selectinfo == NULL) {
		ASYNCIO_ERROR("Failed to malloc selectinfo.\n");
		return -1;
	}

	selectinfo->max_nfds = (int)max_nfds;
	selectinfo->cur_nfds = 1;	/* The clearwakefd */
	selectinfo->clearwakefd = clearwakefd;
	selectinfo->max_fd = clearwakefd; /* it is the max, as of now. */

	/* Initialize descriptor sets */
	FD_ZERO(&selectinfo->readfds);
	FD_ZERO(&selectinfo->writefds);
	FD_ZERO(&selectinfo->errorfds);

	/* Add clearwakefd as a readable event. */
	FD_SET(clearwakefd, &selectinfo->readfds);

	*backend_data = selectinfo;
	return 0;
}

static void fdevents_select_cleanup_backend_data(void *backend_data)
{
	struct select_info *selectinfo;

	selectinfo = backend_data;
	asyncio_safe_free(selectinfo);
}
