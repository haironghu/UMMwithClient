#ifndef META_HASH_TABLE_H
#define META_HASH_TABLE_H

#include "../common/types.h"

typedef struct MetaHashTable MetaHashTable;

MetaHashTable* mht_create(uint32_t initial_capacity);
void mht_destroy(MetaHashTable *ht);
int mht_insert(MetaHashTable *ht, const char *key, const ChunkMetadata *value);
int mht_lookup(MetaHashTable *ht, const char *key, ChunkMetadata *out);
int mht_remove(MetaHashTable *ht, const char *key);
int mht_exists(MetaHashTable *ht, const char *key);
uint32_t mht_count(MetaHashTable *ht);
void mht_dump(MetaHashTable *ht);

/* Iterator */
typedef struct { uint32_t idx; } MHTIterator;
MHTIterator mht_iterator(MetaHashTable *ht);
int mht_next(MetaHashTable *ht, MHTIterator *it, const char **out_key, ChunkMetadata *out_val);

#endif
