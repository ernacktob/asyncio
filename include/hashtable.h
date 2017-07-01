#ifndef HASHTABLE_H
#define HASHTABLE_H

#include <stddef.h>

struct hashtable_entry {
	size_t klen;
	const void *key;
	const void *value;

	struct hashtable_entry *bucket_next;
	struct hashtable_entry *memory_next;
};

struct hashtable {
	size_t nbuckets;
	struct hashtable_entry **buckets;

	size_t nentries;
	size_t maxentries;
	struct hashtable_entry *entries;
	struct hashtable_entry *first;
};

int asyncio_hashtable_init(struct hashtable *table, size_t maxentries);
int asyncio_hashtable_insert(struct hashtable *table, size_t klen, const void *key, const void *value);
int asyncio_hashtable_modify(struct hashtable *table, size_t klen, const void *key, const void *value);
int asyncio_hashtable_lookup(struct hashtable *table, size_t klen, const void *key, const void **valuep);
void asyncio_hashtable_delete(struct hashtable *table, size_t klen, const void *key);
void asyncio_hashtable_destroy(struct hashtable *table);

#endif
