#ifndef CANCELLATIONS_H
#define CANCELLATIONS_H

#include <pthread.h>

#define CANCEL_ENABLE		PTHREAD_CANCEL_ENABLE
#define CANCEL_DISABLE		PTHREAD_CANCEL_DISABLE

#define CANCEL_DEFERRED		PTHREAD_CANCEL_DEFERRED
#define CANCEL_ASYNCHRONOUS	PTHREAD_CANCEL_ASYNCHRONOUS

void disable_cancellations(int *oldstate);
void restore_cancelstate(int oldstate);
void set_cancelstate(int state, int *oldstate);
void restore_canceltype(int oldtype);
void set_canceltype(int type, int *oldtype);

#endif
