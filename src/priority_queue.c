#include <stddef.h>
#include <stdint.h>

#include "priority_queue.h"
#include "safe_malloc.h"
#include "constants.h"

/* PROTOTYPES */
static void fix_tree_branch(struct priority_queue *queue, struct prefix_tree_node **nodep);
/* END PROTOTYPES */

static void fix_tree_branch(struct priority_queue *queue, struct prefix_tree_node **nodep)
{
	if ((*nodep)->next_0 == NULL && (*nodep)->next_1 == NULL) {
		(*nodep)->memory_next = queue->first;
		queue->first = *nodep;
		*nodep = NULL;
		return;
	}

	if ((*nodep)->next_0 == NULL) {
		(*nodep)->priority = (*nodep)->next_1->priority;
		(*nodep)->data = (*nodep)->next_1->data;
		fix_tree_branch(queue, &((*nodep)->next_1));
	} else {
		(*nodep)->priority = (*nodep)->next_0->priority;
		(*nodep)->data = (*nodep)->next_0->data;
		fix_tree_branch(queue, &((*nodep)->next_0));
	}
}

int priority_queue_init(struct priority_queue *queue, size_t maxentries)
{
	struct prefix_tree_node *entries;
	size_t i;

	if (maxentries == 0)
		return -1;

	entries = safe_malloc(maxentries, sizeof *entries);

	if (entries == NULL)
		return -1;

	for (i = 0; i < maxentries - 1; i++)
		entries[i].memory_next = &entries[i + 1];

	entries[maxentries - 1].memory_next = NULL;

	queue->root = NULL;
	queue->nentries = 0;
	queue->maxentries = maxentries;
	queue->entries = entries;
	queue->first = &entries[0];

	return 0;
}

int priority_queue_insert(struct priority_queue *queue, uint64_t priority, const void *data)
{
	struct prefix_tree_node *node;
	struct prefix_tree_node **nodep;
	int i;

	if (queue->nentries == queue->maxentries)
		return -1;

	nodep = &queue->root;
	i = 64;

	while (*nodep != NULL) {
		/* Another element with same priority in queue already */
		if ((*nodep)->priority == priority)
			return -1;

		--i;

		if (priority & (((uint64_t)1) << i)) {
			nodep = &((*nodep)->next_1);
		} else {
			nodep = &((*nodep)->next_0);
		}
	}

	/* Get memory for a prefix_tree_node */
	node = queue->first;
	queue->first = node->memory_next;
	++(queue->nentries);

	node->priority = priority;
	node->data = data;
	node->next_0 = NULL;
	node->next_1 = NULL;

	*nodep = node;
	return 0;
}

int priority_queue_modify(struct priority_queue *queue, uint64_t priority, const void *data)
{
	struct prefix_tree_node **nodep;
	int i;

	nodep = &queue->root;
	i = 64;

	while (*nodep != NULL) {
		if ((*nodep)->priority == priority) {
			(*nodep)->data = data;
			return 0;
		}

		--i;

		if (priority & (((uint64_t)1) << i))
			nodep = &((*nodep)->next_1);
		else
			nodep = &((*nodep)->next_0);
	}

	return -1;
}

int priority_queue_lookup(struct priority_queue *queue, uint64_t priority, const void **datap)
{
	struct prefix_tree_node **nodep;
	int i;

	nodep = &queue->root;
	i = 64;

	while (*nodep != NULL) {
		if ((*nodep)->priority == priority) {
			*datap = (*nodep)->data;
			return 1;
		}

		--i;

		if (priority & (((uint64_t)1) << i))
			nodep = &((*nodep)->next_1);
		else
			nodep = &((*nodep)->next_0);
	}

	return 0;
}

int priority_queue_peek(struct priority_queue *queue, uint64_t *priorityp, const void **datap)
{
	struct prefix_tree_node **nodep;
	uint64_t minpri;
	const void *min_data;

	if (queue->nentries == 0)
		return -1;

	minpri = UINT64T_MAX;
	nodep = &queue->root;

	while (*nodep != NULL) {
		if ((*nodep)->priority < minpri) {
			minpri = (*nodep)->priority;
			min_data = (*nodep)->data;
		}

		if ((*nodep)->next_0 != NULL)
			nodep = &((*nodep)->next_0);
		else
			nodep = &((*nodep)->next_1);
	}

	*priorityp = minpri;
	*datap = min_data;
	return 0;
}

int priority_queue_pop(struct priority_queue *queue, uint64_t *priorityp, const void **datap)
{
	struct prefix_tree_node **nodep;
	struct prefix_tree_node **min_nodep;
	uint64_t minpri;

	if (queue->nentries == 0)
		return -1;

	minpri = UINT64T_MAX;
	nodep = &queue->root;

	while (*nodep != NULL) {
		if ((*nodep)->priority < minpri) {
			min_nodep = nodep;
			minpri = (*nodep)->priority;
		}

		if ((*nodep)->next_0 != NULL)
			nodep = &((*nodep)->next_0);
		else
			nodep = &((*nodep)->next_1);
	}

	*priorityp = (*min_nodep)->priority;
	*datap = (*min_nodep)->data;
	fix_tree_branch(queue, min_nodep);
	--(queue->nentries);

	return 0;
}

void priority_queue_delete(struct priority_queue *queue, uint64_t priority)
{
	struct prefix_tree_node **nodep;
	int i;

	nodep = &queue->root;
	i = 64;

	while (*nodep != NULL) {
		if ((*nodep)->priority == priority) {
			fix_tree_branch(queue, nodep);
			--(queue->nentries);
			return;
		}

		--i;

		if (priority & (((uint64_t)1) << i))
			nodep = &((*nodep)->next_1);
		else
			nodep = &((*nodep)->next_0);
	}
}

void priority_queue_destroy(struct priority_queue *queue)
{
	safe_free(queue->entries);
}
