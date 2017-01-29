#include <pthread.h>
#include <sys/errno.h>

#include "logging.h"

void disable_cancellations(int *oldstate)
{
	int rc;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", oldstate));
	ASYNCIO_DEBUG_CALL(3 FUNC(pthread_setcancelstate) ARG("%d", PTHREAD_CANCEL_DISABLE) ARG("%p", oldstate));
	if ((rc = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, oldstate)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_setcancelstate");
	}

	ASYNCIO_DEBUG_RETURN(VOIDRET);
}

void restore_cancelstate(int oldstate)
{
	int oldstate1;
	int rc;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", oldstate));
	ASYNCIO_DEBUG_CALL(3 FUNC(pthread_setcancelstate) ARG("%d", oldstate) ARG("%p", &oldstate1));
	if ((rc = pthread_setcancelstate(oldstate, &oldstate1)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_setcancelstate");
	}

	ASYNCIO_DEBUG_RETURN(VOIDRET);
}

void set_cancelstate(int state, int *oldstate)
{
	int rc;

	ASYNCIO_DEBUG_ENTER(2 ARG("%d", state) ARG("%p", oldstate));
	ASYNCIO_DEBUG_CALL(3 FUNC(pthread_setcancelstate) ARG("%d", state) ARG("%p", oldstate));
	if ((rc = pthread_setcancelstate(state, oldstate)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_setcancelstate");
	}

	ASYNCIO_DEBUG_RETURN(VOIDRET);
}

void restore_canceltype(int oldtype)
{
	int oldtype1;
	int rc;

	ASYNCIO_DEBUG_ENTER(1 ARG("%d", oldtype));
	ASYNCIO_DEBUG_CALL(3 FUNC(pthread_setcanceltype) ARG("%d", oldtype) ARG("%p", &oldtype1));

	if ((rc = pthread_setcanceltype(oldtype, &oldtype1)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_setcanceltype");
	}

	ASYNCIO_DEBUG_RETURN(VOIDRET);
}

void set_canceltype(int type, int *oldtype)
{
	int rc;

	ASYNCIO_DEBUG_ENTER(2 ARG("%d", type) ARG("%p", oldtype));
	ASYNCIO_DEBUG_CALL(3 FUNC(pthread_setcanceltype) ARG("%d", type) ARG("%p", oldtype));
	if ((rc = pthread_setcanceltype(type, oldtype)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_setcanceltype");
	}

	ASYNCIO_DEBUG_RETURN(VOIDRET);
}
