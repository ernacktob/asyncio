#ifndef LOGGING_H
#define LOGGING_H

#ifdef DEBUG
void ASYNCIO_DEBUG(const char *prefixfmt, const char *func, const char *type, int nargs, ...);

#define VOIDARG				1, "%s", "void"
#define VOIDCALL			, "%s", "void"
#define ARG(fmt, name)			, #name" = "fmt, name
#define FUNC(fn)			, "function %s (%p)", #fn, fn

#define VOIDRET				"%s", "return"
#define RET(fmt, val)			"return "fmt, val

#define TOSTRING(x)			STRINGIFY(x)
#define STRINGIFY(x)			#x

#define ASYNCIO_DEBUG_ENTER(...)	ASYNCIO_DEBUG("{"__FILE__":"TOSTRING(__LINE__)" %s} ", __func__, "ENTER", __VA_ARGS__)
#define ASYNCIO_DEBUG_CALL(...)		ASYNCIO_DEBUG("{"__FILE__":"TOSTRING(__LINE__)" %s} ", __func__, "CALL", __VA_ARGS__)
#define ASYNCIO_DEBUG_RETURN(...)	ASYNCIO_DEBUG("{"__FILE__":"TOSTRING(__LINE__)" %s} ", __func__, "RETURN", 1, __VA_ARGS__)

#else
#define VOIDARG				"", ""	/* Must expand to two parameters */
#define VOIDCALL			""
#define ARG(fmt, name)			""	/* String concatenation in preprocessor will turn all ARG(x,y) into one big "" */
#define FUNC(fn)			""

#define VOIDRET
#define RET(fmt, val)

#define ASYNCIO_DEBUG_ENTER(x)
#define ASYNCIO_DEBUG_CALL(x)
#define ASYNCIO_DEBUG_RETURN(x)

#endif /* DEBUG */

void ASYNCIO_ERROR(const char *fmt, ...);
void ASYNCIO_SYSERROR(const char *s);

#endif
