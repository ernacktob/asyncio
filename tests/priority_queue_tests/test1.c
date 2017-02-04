#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <assert.h>

#include "priority_queue.h"

#define MAXENTRIES	1000000
#define K		((uint64_t)12297829382473034411U)	/* 3^(-1) mod 2^64, to get a 'random-like' permutation of the priorities */

struct test_entry {
	uint64_t priority;
	int *valuep;
	int in_queue;
};

static int data[MAXENTRIES];
static int in_queue[MAXENTRIES];
static struct test_entry entries[MAXENTRIES];

int entry_compar(const void *a, const void *b)
{
	const struct test_entry *ea, *eb;

	ea = (const struct test_entry *)a;
	eb = (const struct test_entry *)b;

	if (ea->in_queue == 1 && eb->in_queue == 0)
		return -1;
	else if (ea->in_queue == 0 && eb->in_queue == 1)
		return 1;
	else if (ea->priority < eb->priority)
		return -1;
	else if (ea->priority > eb->priority)
		return 1;
	else
		return 0;
}

int main()
{
	struct priority_queue queue;
	uint64_t start_counter;
	uint64_t counter;
	uint64_t prio;
	int *valuep;
	int i;

	srand(time(NULL));

	for (i = 0; i < MAXENTRIES; i++)
		data[i] = rand();

	assert(priority_queue_init(&queue, MAXENTRIES) == 0);

	start_counter = (((uint64_t)rand()) << 32) + (uint64_t)rand();

	for (i = 0, counter = start_counter; i < MAXENTRIES; i++, counter++) {
		assert(priority_queue_insert(&queue, K * counter, &data[i]) == 0);
		in_queue[i] = 1;
	}

	data[0] = rand();
	assert(priority_queue_modify(&queue, K * start_counter, &data[0]) == 0);

	for (i = 0, counter = start_counter; i < MAXENTRIES; i++, counter++) {
		assert(priority_queue_lookup(&queue, K * counter, (void *)&valuep) == 1);
		assert(*valuep == data[i]);
	}

	/* K * MAXENTRIES is a priority value that should not have occured yet */
	assert(priority_queue_lookup(&queue, K * MAXENTRIES, (void *)&valuep) == 0);

	for (i = MAXENTRIES / 2, counter = start_counter + MAXENTRIES / 2; i < MAXENTRIES; i++, counter++) {
		priority_queue_delete(&queue, K * counter);
		in_queue[i] = 0;
	}

	for (i = 0, counter = start_counter; i < MAXENTRIES; i++, counter++)
		assert(priority_queue_lookup(&queue, K * counter, (void *)&valuep) == in_queue[i]);

	for (i = 0, counter = start_counter; i < MAXENTRIES; i++, counter++) {
		entries[i].priority = K * counter;
		entries[i].valuep = &data[i];
		entries[i].in_queue = in_queue[i];
	}

	qsort(entries, MAXENTRIES, sizeof *entries, entry_compar);

	for (i = 0; i < MAXENTRIES; i++) {
		if (in_queue[i]) {
			assert(priority_queue_peek(&queue, &prio) == 0);
			assert(prio == entries[i].priority);
			assert(priority_queue_pop(&queue, (void *)&valuep) == 0);
			assert(*valuep == *(entries[i].valuep));
			in_queue[i] = 0;
		} else {
			assert(priority_queue_peek(&queue, &prio) == -1);
		}
	}

	priority_queue_destroy(&queue);
	printf("Test passed!\n");

	return 0;
}
