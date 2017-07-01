#ifndef SAFE_MALLOC_H
#define SAFE_MALLOC_H

#include <stdlib.h>

/* These functions should not be called while holding other locks just in case they block */
void *asyncio_safe_malloc(size_t count, size_t size);
void asyncio_safe_free(void *ptr);

#endif
