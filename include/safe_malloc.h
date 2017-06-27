#ifndef SAFE_MALLOC_H
#define SAFE_MALLOC_H

#include <stdlib.h>

/* These functions should not be called while holding other locks just in case they block */
void *safe_malloc(size_t count, size_t size);
void *safe_free(void *ptr);

#endif
