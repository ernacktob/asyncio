#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <assert.h>

#include "hashtable.h"

#define NUM_KEYS	1000
#define NUM_TRIALS	1000000

static int keys[NUM_KEYS];
static size_t values[NUM_KEYS];
static int in_table[NUM_KEYS];

int main()
{
	struct hashtable table;
	int key;
	size_t value;
	int i;

	srand(time(NULL));

	for (i = 0; i < NUM_KEYS; i++) {
		keys[i] = i;
		values[i] = rand();
	}

	assert(asyncio_hashtable_init(&table, NUM_KEYS) == 0);

	for (i = 0; i < NUM_KEYS; i++) {
		assert(asyncio_hashtable_insert(&table, keys[i], values[i]) == 0);
		in_table[i] = 1;
	}

	for (i = 0; i < NUM_KEYS; i++) {
		assert(asyncio_hashtable_lookup(&table, keys[i], &value) != 0);
		assert(value == values[i]);
	}

	for (i = 0; i < NUM_TRIALS; i++) {
		key = rand() % NUM_KEYS;
		assert(asyncio_hashtable_lookup(&table, key, &value) == in_table[key]);

		if (in_table[key])
			assert(value == values[key]);

		if (rand() % 2) {
			asyncio_hashtable_delete(&table, key);
			assert(asyncio_hashtable_lookup(&table, key, &value) == 0);
			in_table[key] = 0;
		} else {
			if (in_table[key]) {
				asyncio_hashtable_delete(&table, key);
				assert(asyncio_hashtable_lookup(&table, key, &value) == 0);
				in_table[key] = 0;
			}

			values[key] = rand();
			assert(asyncio_hashtable_insert(&table, keys[key], values[key]) == 0);
			assert(asyncio_hashtable_lookup(&table, key, &value) == 1);
			assert(value == values[key]);
			in_table[key] = 1;
		}
	}

	asyncio_hashtable_destroy(&table);
	printf("Test passed!\n");

	return 0;
}
