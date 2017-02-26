#ifndef SAFE_MALLOC_H
#define SAFE_MALLOC_H

#include <stdlib.h>

#if !defined(MALLOC_IS_THREAD_SAFE) || !defined(FREE_IS_THREAD_SAFE)
#include <sys/errno.h>
#include <pthread.h>
#endif

/* These functions should not be called while holding other locks just in case they block */
#ifdef MALLOC_IS_THREAD_SAFE
#define safe_malloc(size) malloc(size)
#else
void *malloc_locked(size_t size);
#define safe_malloc(size) malloc_locked(size)
#endif

#ifdef FREE_IS_THREAD_SAFE
#define safe_free(ptr) free(ptr)
#else
void free_locked(void *ptr);
#define safe_free(ptr) free_locked(ptr)
#endif

#endif
