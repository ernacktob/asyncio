#ifndef QUEUE_H
#define QUEUE_H

#include <stddef.h>

#define decl_queue(type, name) \
	struct {\
		type *first;\
		type *last;\
		size_t count;\
		int id;\
	} name

#define queue_init(queue, qid) \
	do {\
		(queue)->first = NULL;\
		(queue)->last = NULL;\
		(queue)->count = 0;\
		(queue)->id = qid;\
	} while (0)

#define queue_empty(queue) \
	((queue)->count == 0)

#define queue_count(queue) \
	((queue)->count)

#define queue_first(queue) \
	((queue)->first)

#define queue_push(queue, node) \
	do {\
		if ((queue)->first == NULL) {\
			(queue)->first = (queue)->last = (node);\
			(node)->prev[(queue)->id] = (node)->next[(queue)->id] = NULL;\
		} else {\
			(queue)->last->next[(queue)->id] = (node);\
			(node)->prev[(queue)->id] = (queue)->last;\
			(queue)->last = (node);\
		}\
		\
		(node)->next[(queue)->id] = NULL;\
		++((queue)->count);\
	} while (0)

#define queue_pop(queue, nodep) \
	do {\
		*(nodep) = (queue)->first;\
		\
		if ((queue)->first != NULL) {\
			(queue)->first = (queue)->first->next[(queue)->id];\
			\
			if ((queue)->first != NULL)\
				(queue)->first->prev[(queue)->id] = NULL;\
			else\
				(queue)->last = NULL;\
			\
			(*(nodep))->next[(queue)->id] = NULL;\
			--((queue)->count);\
		}\
	} while (0)

/* Careful, this assumes the node is actually in the queue! */
#define queue_remove(queue, node) \
	do {\
		if ((node)->prev[(queue)->id] != NULL)\
			(node)->prev[(queue)->id]->next[(queue)->id] = (node)->next[(queue)->id];\
		if ((node)->next[(queue)->id] != NULL)\
			(node)->next[(queue)->id]->prev[(queue)->id] = (node)->prev[(queue)->id];\
		if ((node) == (queue)->first)\
			(queue)->first = (node)->next[(queue)->id];\
		if ((node) == (queue)->last)\
			(queue)->last = (node)->prev[(queue)->id];\
		(node)->prev[(queue)->id] = NULL;\
		(node)->next[(queue)->id] = NULL;\
		--((queue)->count);\
	} while (0)

#define queue_iterate(queue, type, func) \
	do {\
		void *node, *next;\
		\
		for (node = (queue)->first; node != NULL; node = next) {\
			next = ((type)node)->next[(queue)->id];\
			(func)(node);\
		}\
	} while (0)

#define queue_foreach(queue, node, next) for ((node) = (queue)->first; (node) != NULL; (node) = (next))

#endif
