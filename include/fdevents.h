#ifndef FDEVENTS_H
#define FDEVENTS_H

#include <stdint.h>

/* XXX TODO Put this in fdevents_priv.h (and rename to fdevents.h)
 * we will create separate structs / flags for the public library stuff in asyncio.h */

#define FDEVENTS_EVENT_READ		(1 << 0)
#define FDEVENTS_EVENT_WRITE		(1 << 1)
#define FDEVENTS_EVENT_ERROR		(1 << 2)

#define FDEVENTS_FLAG_NONE		0
#define FDEVENTS_FLAG_CONTRACTOR	(1 << 0)
#define FDEVENTS_FLAG_CANCELLABLE	(1 << 1)
#define FDEVENTS_FLAG_ASYNCCANCEL	(1 << 2)

struct fdevents_handle_priv;
struct fdevents_loop_priv;

struct fdevents_handle {
	struct fdevents_handle_priv *priv;

	int (*wait)(const struct fdevents_handle *self);
	int (*cancel)(const struct fdevents_handle *self);
	int (*acquire)(const struct fdevents_handle *self);
	void (*release)(const struct fdevents_handle *self);
};

struct fdevents_loop {
	struct fdevents_loop_priv *priv;

	int (*acquire)(const struct fdevents_loop *self);
	int (*listen)(struct fdevents_loop *self, const struct fdevents_info *info, struct fdevents_handle **handlep);
	void (*release)(const struct fdevents_loop *self);
};

typedef void (*fdevents_callback)(int fd, uint16_t revents, void *arg, int *continued);

struct fdevents_info {
	int fd;
	uint16_t events;
	uint32_t flags;
	fdevents_callback cb;	/* Called when event occurs */
	void *arg;
};

#endif
