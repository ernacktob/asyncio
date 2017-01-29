#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <assert.h>

#include "hashtable.h"

#define NUM_KEYS	1000
#define NUM_TRIALS	1000000

static int keys[NUM_KEYS];
static int values[NUM_KEYS];
static int in_table[NUM_KEYS];

int main()
{
	struct hashtable table;
	int key;
	int *value;
	size_t count;
	int i;

	srand(time(NULL));

	for (i = 0; i < NUM_KEYS; i++) {
		keys[i] = i;
		values[i] = rand();
	}

	assert(hashtable_init(&table, NUM_KEYS) == 0);

	for (i = 0; i < NUM_KEYS; i++) {
		assert(hashtable_insert(&table, sizeof keys[i], &keys[i], &values[i]) == 0);
		in_table[i] = 1;
	}

	for (i = 0; i < NUM_KEYS; i++) {
		assert(hashtable_lookup(&table, sizeof keys[i], &keys[i], (const void **)&value) != 0);
		assert(*value == values[i]);
	}

	count = NUM_KEYS;

	for (i = 0; i < NUM_TRIALS; i++) {
		key = rand() % NUM_KEYS;
		assert(hashtable_lookup(&table, sizeof key, &key, (const void **)&value) == in_table[key]);

		if (in_table[key])
			assert(*value == values[key]);

		if (rand() % 2) {
			hashtable_delete(&table, sizeof key, &key);
			assert(hashtable_lookup(&table, sizeof key, &key, (const void **)&value) == 0);
			in_table[key] = 0;
		} else {
			if (in_table[key]) {
				hashtable_delete(&table, sizeof key, &key);
				assert(hashtable_lookup(&table, sizeof key, &key, (const void **)&value) == 0);
				in_table[key] = 0;
			}

			values[key] = rand();
			assert(hashtable_insert(&table, sizeof keys[key], &keys[key], &values[key]) == 0);
			assert(hashtable_lookup(&table, sizeof key, &key, (const void **)&value) == 1);
			assert(*value == values[key]);
			in_table[key] = 1;
		}
	}

	hashtable_destroy(&table);
	printf("Test passed!\n");

	return 0;
}
