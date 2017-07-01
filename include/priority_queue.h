#ifndef PRIORITY_QUEUE_H
#define PRIORITY_QUEUE_H

#include <stddef.h>
#include <stdint.h>

struct prefix_tree_node {
	uint64_t priority;
	const void *data;
	struct prefix_tree_node *next_0;
	struct prefix_tree_node *next_1;
	struct prefix_tree_node *memory_next;
};

struct priority_queue {
	struct prefix_tree_node *root;
	size_t nentries;
	size_t maxentries;
	struct prefix_tree_node *entries;
	struct prefix_tree_node *first;
};

#define priority_queue_empty(queue) ((queue)->nentries == 0)

int asyncio_priority_queue_init(struct priority_queue *queue, size_t maxentries);
int asyncio_priority_queue_insert(struct priority_queue *queue, uint64_t priority, const void *data);
int asyncio_priority_queue_modify(struct priority_queue *queue, uint64_t priority, const void *data);
int asyncio_priority_queue_lookup(struct priority_queue *queue, uint64_t priority, const void **datap);
int asyncio_priority_queue_peek(struct priority_queue *queue, uint64_t *priorityp, const void **datap);
int asyncio_priority_queue_pop(struct priority_queue *queue, uint64_t *priorityp, const void **datap);
void asyncio_priority_queue_delete(struct priority_queue *queue, uint64_t priority);
void asyncio_priority_queue_destroy(struct priority_queue *queue);

#endif
