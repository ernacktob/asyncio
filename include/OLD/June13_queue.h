#ifndef QUEUE_H
#define QUEUE_H

#include <stddef.h>

#define decl_queue(type,name) \
	struct {\
		type *first;\
		type *last;\
		size_t count;\
	} name

#define queue_init(queue) \
	do {\
		(queue)->first = NULL;\
		(queue)->last = NULL;\
		(queue)->count = 0;\
	} while (0)

#define queue_empty(queue) \
	((queue)->count == 0)

#define queue_count(queue) \
	((queue)->count)

#define queue_first(queue) \
	((queue)->first)

#define queue_push(queue, links, node) \
	do {\
		if ((queue)->first == NULL) {\
			(queue)->first = (queue)->last = (node);\
			(node)->links##_##prev = (node)->links##_##next = NULL;\
		} else {\
			(queue)->last->links##_##next = (node);\
			(node)->links##_##prev = (queue)->last;\
			(queue)->last = (node);\
		}\
		\
		(node)->links##_##next = NULL;\
		++((queue)->count);\
	} while (0)

#define queue_pop(queue, links, nodep) \
	do {\
		*(nodep) = (queue)->first;\
		\
		if ((queue)->first != NULL) {\
			(queue)->first = (queue)->first->links##_##next;\
			\
			if ((queue)->first != NULL)\
				(queue)->first->links##_##prev = NULL;\
			else\
				(queue)->last = NULL;\
			\
			(*(nodep))->links##_##next = NULL;\
			--((queue)->count);\
		}\
	} while (0)

/* Careful, this assumes the node is actually in the queue! */
#define queue_remove(queue, links, node) \
	do {\
		if ((node)->links##_##prev != NULL)\
			(node)->links##_##prev->links##_##next = (node)->links##_##next;\
		if ((node)->links##_##next != NULL)\
			(node)->links##_##next->links##_##prev = (node)->links##_##prev;\
		if ((node) == (queue)->first)\
			(queue)->first = (node)->links##_##next;\
		if ((node) == (queue)->last)\
			(queue)->last = (node)->links##_##prev;\
		(node)->links##_##prev = NULL;\
		(node)->links##_##next = NULL;\
		--((queue)->count);\
	} while (0)

#define queue_iterate(queue, links, type, func) \
	do {\
		void *node, *next;\
		\
		for (node = (queue)->first; node != NULL; node = next) {\
			next = ((type)node)->links##_##next;\
			(func)(node);\
		}\
	} while (0)

#define queue_foreach(queue, node, next) for ((node) = (queue)->first; (node) != NULL; (node) = (next))

#endif
