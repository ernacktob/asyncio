#include <stdlib.h>
#include <string.h>

#include "hashtable.h"
#include "safe_malloc.h"

#define SIZET_MAX		((size_t)(-1)) /* Get rid of compiler warning about 'use of C99 long long integer constant' for SIZE_MAX */

/* PROTOTYPES */
static unsigned long djb2_hash(const unsigned char *data, size_t len);
static size_t findbucket(const void *key, size_t klen, size_t nbuckets);
/* END PROTOTYPES */

static unsigned long djb2_hash(const unsigned char *data, size_t len)
{
	unsigned long hash = 5381;
	size_t i;

	for (i = 0; i < len; i++)
		hash = ((hash << 5) + hash) + data[i];

	return hash;
}

static size_t findbucket(const void *key, size_t klen, size_t nbuckets)
{
	size_t bucket;
	unsigned long hash;

	hash = djb2_hash((const unsigned char *)key, klen);
	bucket = ((size_t)hash) % nbuckets;

	return bucket;
}

int hashtable_init(struct hashtable *table, size_t maxentries)
{
	size_t nbuckets;
	struct hashtable_entry **buckets;
	struct hashtable_entry *entries;
	size_t i;

	if (maxentries == 0)
		return -1;

	/* Check for overflows during multiplication */
	if (maxentries > SIZET_MAX / sizeof *buckets)
		return -1;

	if (maxentries > SIZET_MAX / sizeof *entries)
		return -1;

	nbuckets = maxentries;
	buckets = safe_malloc(nbuckets * sizeof *buckets);

	if (buckets == NULL)
		return -1;

	entries = safe_malloc(maxentries * sizeof *entries);

	if (entries == NULL) {
		safe_free(buckets);
		return -1;
	}

	for (i = 0; i < nbuckets; i++)
		buckets[i] = NULL;

	for (i = 0; i < maxentries - 1; i++)
		entries[i].memory_next = &entries[i + 1];

	entries[maxentries - 1].memory_next = NULL;

	table->nbuckets = nbuckets;
	table->buckets = buckets;
	table->nentries = 0;
	table->maxentries = maxentries;
	table->entries = entries;
	table->first = &entries[0];

	return 0;
}

int hashtable_insert(struct hashtable *table, size_t klen, const void *key, const void *value)
{
	struct hashtable_entry *entry, *prev;
	size_t bucket;

	if (table->nentries == table->maxentries)
		return -1;

	bucket = findbucket(key, klen, table->nbuckets);

	prev = NULL;
	entry = table->buckets[bucket];

	while (entry != NULL) {
		if (memcmp(entry->key, key, klen) == 0)
			return -1;

		prev = entry;
		entry = entry->bucket_next;
	}

	entry = table->first;

	if (prev == NULL)
		table->buckets[bucket] = entry;
	else
		prev->bucket_next = entry;

	entry->klen = klen;
	entry->key = key;
	entry->value = value;
	entry->bucket_next = NULL;

	table->first = table->first->memory_next;
	++(table->nentries);

	return 0;
}

int hashtable_modify(struct hashtable *table, size_t klen, const void *key, const void *value)
{
	struct hashtable_entry *entry;
	size_t bucket;

	bucket = findbucket(key, klen, table->nbuckets);
	entry = table->buckets[bucket];

	while (entry != NULL) {
		if (memcmp(entry->key, key, klen) == 0) {
			entry->value = value;
			return 0;
		}

		entry = entry->bucket_next;
	}

	return -1;
}

int hashtable_lookup(struct hashtable *table, size_t klen, const void *key, const void **valuep)
{
	struct hashtable_entry *entry;
	size_t bucket;

	bucket = findbucket(key, klen, table->nbuckets);

	for (entry = table->buckets[bucket]; entry != NULL; entry = entry->bucket_next) {
		if (entry->klen == klen) {
			if (memcmp(entry->key, key, klen) == 0) {
				*valuep = entry->value;
				return 1;
			}
		}
	}

	return 0;
}

void hashtable_delete(struct hashtable *table, size_t klen, const void *key)
{
	struct hashtable_entry *entry, *prev;
	size_t bucket;

	bucket = findbucket(key, klen, table->nbuckets);

	for (prev = NULL, entry = table->buckets[bucket]; entry != NULL; prev = entry, entry = entry->bucket_next) {
		if (entry->klen == klen) {
			if (memcmp(entry->key, key, klen) == 0) {
				if (prev == NULL)
					table->buckets[bucket] = entry->bucket_next;
				else
					prev->bucket_next = entry->bucket_next;

				entry->memory_next = table->first;
				table->first = entry;
				--(table->nentries);
				break;
			}
		}
	}
}

void hashtable_destroy(struct hashtable *table)
{
	safe_free(table->entries);
	safe_free(table->buckets);
}
