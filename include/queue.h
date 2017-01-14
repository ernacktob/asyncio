#ifndef QUEUE_H
#define QUEUE_H

#include <stdlib.h>

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

#define queue_push(queue, node) \
	do {\
		if ((queue)->first == NULL) {\
			(queue)->first = (queue)->last = (node);\
			(node)->prev = (node)->next = NULL;\
		} else {\
			(queue)->last->next = (node);\
			(node)->prev = (queue)->last;\
			(queue)->last = (node);\
		}\
		\
		(node)->next = NULL;\
		++((queue)->count);\
	} while (0)

#define queue_pop(queue, nodep) \
	do {\
		*(nodep) = (queue)->first;\
		\
		if ((queue)->first != NULL) {\
			(queue)->first = (queue)->first->next;\
			\
			if ((queue)->first != NULL)\
				(queue)->first->prev = NULL;\
			else\
				(queue)->last = NULL;\
			\
			(*(nodep))->next = NULL;\
			--((queue)->count);\
		}\
	} while (0)

/* Careful, this assumes the node is actually in the queue! */
#define queue_remove(queue, node) \
	do {\
		if ((node)->prev != NULL)\
			(node)->prev->next = (node)->next;\
		if ((node)->next != NULL)\
			(node)->next->prev = (node)->prev;\
		if ((node) == (queue)->first)\
			(queue)->first = (node)->next;\
		if ((node) == (queue)->last)\
			(queue)->last = (node)->prev;\
		(node)->prev = NULL;\
		(node)->next = NULL;\
		--((queue)->count);\
	} while (0)

#define queue_iterate(queue, type, func) \
	do {\
		void *node, *next;\
		\
		for (node = (queue)->first; node != NULL; node = next) {\
			next = ((type)node)->next;\
			(func)(node);\
		}\
	} while (0)

#define queue_foreach(queue, node, next) for ((node) = (queue)->first; (node) != NULL; (node) = (next))

#endif
