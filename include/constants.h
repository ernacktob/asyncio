#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <stddef.h>
#include <stdint.h>
#include <poll.h>

#define UINT64T_MAX		((uint64_t)(-1)) /* Get rid of compiler warning about 'use of C99 long long integer constant' for UINT64_MAX */
#define SIZET_MAX		((size_t)(-1)) /* Get rid of compiler warning about 'use of C99 long long integer constant' for SIZE_MAX */
#define NFDST_MAX		((nfds_t)(-1))

#endif
