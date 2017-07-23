#include <pthread.h>
#include <sys/errno.h>

#include "threading.h"
#include "logging.h"

int ASYNCIO_THREAD_CREATE(ASYNCIO_THREAD_T *thread, void *(start_routine)(void *), void *arg)
{
	int rc;

	ASYNCIO_DEBUG_ENTER(3 ARG("%p", thread) ARG("%p", start_routine) ARG("%p", arg));
	ASYNCIO_DEBUG_CALL(5 FUNC(pthread_create) ARG("%p", thread) ARG("%p", NULL) ARG("%p", start_routine) ARG("%p", arg));
	if ((rc = pthread_create(thread, NULL, start_routine, arg)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_create");
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	ASYNCIO_DEBUG_RETURN(RET("%d", 0));
	return 0;
}

int ASYNCIO_THREAD_JOIN(ASYNCIO_THREAD_T thread)
{
	int rc;

	ASYNCIO_DEBUG_ENTER(1 ARG("%016llx", thread));
	ASYNCIO_DEBUG_CALL(3 FUNC(pthread_join) ARG("%016llx", thread) ARG("%p", NULL));
	if ((rc = pthread_join(thread, NULL)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_join");
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	ASYNCIO_DEBUG_RETURN(RET("%d", 0));
	return 0;
}

int ASYNCIO_THREAD_CANCEL(ASYNCIO_THREAD_T thread)
{
	int rc;

	ASYNCIO_DEBUG_ENTER(1 ARG("%016llx", thread));
	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_cancel) ARG("%016llx", thread));
	if ((rc = pthread_cancel(thread)) != 0) {
		if (rc != ESRCH) {
			/* Some implementations return ESRCH when a thread has terminated by has not yet been released
			   by a pthread_join or pthread_detach. This is not really a problem so ignore it. */
			errno = rc;
			ASYNCIO_SYSERROR("pthread_cancel");
			ASYNCIO_DEBUG_RETURN(RET("%d", -1));
			return -1;
		}
	}

	ASYNCIO_DEBUG_RETURN(RET("%d", 0));
	return 0;
}

void ASYNCIO_DISABLE_CANCELLATIONS(int *oldstate)
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

void ASYNCIO_RESTORE_CANCELSTATE(int oldstate)
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

void ASYNCIO_SET_CANCELSTATE(int state, int *oldstate)
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

void ASYNCIO_RESTORE_CANCELTYPE(int oldtype)
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

void ASYNCIO_SET_CANCELTYPE(int type, int *oldtype)
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

int ASYNCIO_RWLOCK_INIT(ASYNCIO_RWLOCK_T *lock)
{
	int rc;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", lock));
	ASYNCIO_DEBUG_CALL(3 FUNC(pthread_rwlock_init) ARG("%p", lock) ARG("%p", NULL));
	if ((rc = pthread_rwlock_init(lock, NULL)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_rwlock_init");
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	ASYNCIO_DEBUG_RETURN(RET("%d", 0));
	return 0;
}

int ASYNCIO_RWLOCK_WRLOCK(ASYNCIO_RWLOCK_T *lock)
{
	int rc;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", lock));
	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_rwlock_wrlock) ARG("%p", lock));
	if ((rc = pthread_rwlock_wrlock(lock)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_rwlock_wrlock");
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	ASYNCIO_DEBUG_RETURN(RET("%d", 0));
	return 0;
}

int ASYNCIO_RWLOCK_RDLOCK(ASYNCIO_RWLOCK_T *lock)
{
	int rc;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", lock));
	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_rwlock_rdlock) ARG("%p", lock));
	if ((rc = pthread_rwlock_rdlock(lock)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_rwlock_rdlock");
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	ASYNCIO_DEBUG_RETURN(RET("%d", 0));
	return 0;
}

void ASYNCIO_RWLOCK_UNLOCK(ASYNCIO_RWLOCK_T *lock)
{
	int rc;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", lock));
	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_rwlock_unlock) ARG("%p", lock));
	if ((rc = pthread_rwlock_unlock(lock)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_rwlock_unlock");
	}

	ASYNCIO_DEBUG_RETURN(VOIDRET);
}

void ASYNCIO_RWLOCK_DESTROY(ASYNCIO_RWLOCK_T *lock)
{
	int rc;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", lock));
	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_rwlock_destroy) ARG("%p", lock));
	if ((rc = pthread_rwlock_destroy(lock)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_rwlock_destroy");
	}

	ASYNCIO_DEBUG_RETURN(VOIDRET);
}

int ASYNCIO_MUTEX_INIT(ASYNCIO_MUTEX_T *mtx)
{
	int rc;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", mtx));
	ASYNCIO_DEBUG_CALL(3 FUNC(pthread_mutex_init) ARG("%p", mtx) ARG("%p", NULL));
	if ((rc = pthread_mutex_init(mtx, NULL)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_mutex_init");
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	ASYNCIO_DEBUG_RETURN(RET("%d", 0));
	return 0;
}

int ASYNCIO_MUTEX_LOCK(ASYNCIO_MUTEX_T *mtx)
{
	int rc;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", mtx));
	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_lock) ARG("%p", mtx));
	if ((rc = pthread_mutex_lock(mtx)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_mutex_lock");
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	ASYNCIO_DEBUG_RETURN(RET("%d", 0));
	return 0;
}

void ASYNCIO_MUTEX_UNLOCK(ASYNCIO_MUTEX_T *mtx)
{
	int rc;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", mtx));
	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_unlock) ARG("%p", mtx));
	if ((rc = pthread_mutex_unlock(mtx)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_mutex_unlock");
	}

	ASYNCIO_DEBUG_RETURN(VOIDRET);
}

void ASYNCIO_MUTEX_DESTROY(ASYNCIO_MUTEX_T *mtx)
{
	int rc;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", mtx));
	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_mutex_destroy) ARG("%p", mtx));
	if ((rc = pthread_mutex_destroy(mtx)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_mutex_destroy");
	}

	ASYNCIO_DEBUG_RETURN(VOIDRET);
}

int ASYNCIO_COND_INIT(ASYNCIO_COND_T *cond)
{
	int rc;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", cond));
	ASYNCIO_DEBUG_CALL(3 FUNC(pthread_cond_init) ARG("%p", cond) ARG("%p", NULL));
	if ((rc = pthread_cond_init(cond, NULL)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_cond_init");
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	ASYNCIO_DEBUG_RETURN(RET("%d", 0));
	return 0;
}

int ASYNCIO_COND_WAIT(ASYNCIO_COND_T *cond, ASYNCIO_MUTEX_T *mtx)
{
	int rc;

	ASYNCIO_DEBUG_ENTER(2 ARG("%p", cond) ARG("%p", mtx));
	ASYNCIO_DEBUG_CALL(3 FUNC(pthread_cond_wait) ARG("%p", cond) ARG("%p", mtx));
	if ((rc = pthread_cond_wait(cond, mtx)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_cond_wait");
		ASYNCIO_DEBUG_RETURN(RET("%d", -1));
		return -1;
	}

	ASYNCIO_DEBUG_RETURN(RET("%d", 0));
	return 0;
}

void ASYNCIO_COND_SIGNAL(ASYNCIO_COND_T *cond)
{
	int rc;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", mtx));
	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_cond_signal) ARG("%p", cond));
	if ((rc = pthread_cond_signal(cond)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_cond_signal");
	}

	ASYNCIO_DEBUG_RETURN(VOIDRET);
}

void ASYNCIO_COND_BROADCAST(ASYNCIO_COND_T *cond)
{
	int rc;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", mtx));
	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_cond_broadcast) ARG("%p", cond));
	if ((rc = pthread_cond_broadcast(cond)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_cond_broadcast");
	}

	ASYNCIO_DEBUG_RETURN(VOIDRET);
}

void ASYNCIO_COND_DESTROY(ASYNCIO_COND_T *cond)
{
	int rc;

	ASYNCIO_DEBUG_ENTER(1 ARG("%p", mtx));
	ASYNCIO_DEBUG_CALL(2 FUNC(pthread_cond_destroy) ARG("%p", cond));
	if ((rc = pthread_cond_destroy(cond)) != 0) {
		errno = rc;
		ASYNCIO_SYSERROR("pthread_cond_destroy");
	}

	ASYNCIO_DEBUG_RETURN(VOIDRET);
}
