#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include <sys/errno.h>

#include "logging.h"
#include "threading.h"

/* PROTOTYPES */
static void print_thread(ASYNCIO_THREAD_T thread);
/* END PROTOTYPES */

/* XXX Maybe have a thread whose job is to print to terminal?? */

static void print_thread(ASYNCIO_THREAD_T thread)
{
	unsigned char *p;
	size_t i;

	p = (unsigned char *)&thread;

	for (i = 0; i < sizeof thread; i++) {
		fprintf(stderr, "%02x", *p);
		++p;
	}
}

#ifdef DEBUG
void ASYNCIO_DEBUG(const char *prefixfmt, const char *func, const char *type, int nargs, ...)
{
	va_list args;
	const char *fmt;
	int i;

	flockfile(stderr);
	fprintf(stderr, "[ASYNCIO_DEBUG] ");
	fprintf(stderr, "{thread: ");
	print_thread(ASYNCIO_THREAD_SELF());
	fprintf(stderr, "} ");
	fprintf(stderr, prefixfmt, func);
	fprintf(stderr, "%s: ", type);
	va_start(args, nargs);

	/* This should increment over each pair of fmt, value in args */
	for (i = 0; i < nargs - 1; i++) {
		fmt = va_arg(args, const char *);
		vfprintf(stderr, fmt, args);
		fprintf(stderr, ", ");
	}

	fmt = va_arg(args, const char *);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fprintf(stderr, "\n");
	fflush(stderr);
	funlockfile(stderr);
}
#endif

void default_error_func(const char *fmt, va_list args)
{
	/* This will show up as a prefix. */
	flockfile(stderr);
	fprintf(stderr, "[ASYNCIO_ERROR] {thread: ");
	print_thread(ASYNCIO_THREAD_SELF());
	fprintf(stderr, "} ");
	vfprintf(stderr, fmt, args);
	fflush(stderr);
	funlockfile(stderr);
}

void default_syserror_func(const char *s, int errnum)
{
	flockfile(stderr);
	fprintf(stderr, "[ASYNCIO_SYSERROR] {thread: ");
	print_thread(ASYNCIO_THREAD_SELF());
	fprintf(stderr, "} ");
	fprintf(stderr, "%s: (%s)\n", s, strerror(errnum));
	fflush(stderr);
	funlockfile(stderr);
}

void ASYNCIO_ERROR(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	default_error_func(fmt, args);
	va_end(args);
}

void ASYNCIO_SYSERROR(const char *s)
{
	default_syserror_func(s, errno);
}
