#ifndef FDEVENTS_PRIV_H
#define FDEVENTS_PRIV_H

#include <stdint.h>
#include <poll.h>

#include "fdevents.h"
#include "events.h"
#include "queue.h"
#include "threading.h"
#include "hashtable.h"

#define FDEVENTS_HANDLE_QUEUE_ID	0
#define NUMBER_FDEVENTS_HANDLE_QUEUES	1

#define POLLFD_EVENT_NBITS		(CHAR_BIT * sizeof (short))

struct fdevents_handle_priv {
	struct fdevents_handle pub;
	struct events_handle base;

	int fd;
	uint16_t events;
	uint16_t revents;
	fdevents_callback callback_fn;	/* Called when event occurs */
	void *callback_arg;
};

struct fdevents_refcount {
	unsigned int bitcounts[POLLFD_EVENT_NBITS];
};

struct fdevents_loop_priv {
	struct fdevents_loop pub;
	struct events_loop base;

	nfds_t max_nfds;
	nfds_t nfds;
	struct pollfd *fds;
	decl_queue(struct fdevents_handle_priv, *callbacks);
	struct fdevents_refcount *fdevents_refcounts;
	struct hashtable fd_map;
	int changed;
	int wakefd;
	int clearwakefd;

	/* scratch space */
	struct pollfd *scratch_fds;
	nfds_t scratch_nfds;
};

#endif
