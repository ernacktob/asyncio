#ifndef THREADING_H
#define THREADING_H

#include <pthread.h>

void disable_cancellations(int *oldstate);
void restore_cancelstate(int oldstate);
void set_cancelstate(int state, int *oldstate);
void restore_canceltype(int oldtype);
void set_canceltype(int type, int *oldtype);

#define ASYNCIO_CLEANUP_PUSH	pthread_cleanup_push
#define ASYNCIO_CLEANUP_POP	pthread_cleanup_pop

#endif
