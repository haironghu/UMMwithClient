#include "meta_hash_table.h"
#include "../common/error_codes.h"
#include "../common/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* ==================================================================== */
/* Entry state                                                          */
/* ==================================================================== */

typedef enum {
    MHT_EMPTY   = 0,
    MHT_OCCUPIED= 1,
    MHT_DELETED = 2
} MHTEntryState;

/* ==================================================================== */
/* Hash entry                                                           */
/* ==================================================================== */

typedef struct {
    MHTEntryState  state;
    char          *key;       /* NULL if empty */
    uint32_t       key_len;
    uint32_t       hash;      /* cached hash value */
    uint32_t       probe_dist; /* probe distance from ideal position */
    ChunkMetadata  value;
} MHTEntry;

/* ==================================================================== */
/* Hash table structure                                                  */
/* ==================================================================== */

struct MetaHashTable {
    MHTEntry        *entries;
    uint32_t         capacity;
    uint32_t         count;       /* number of occupied entries */
    uint32_t         tombstones;  /* number of deleted entries */
    pthread_rwlock_t lock;
};

/* ==================================================================== */
/* Constants                                                             */
/* ==================================================================== */

#define MHT_LOAD_FACTOR_NUM 7
#define MHT_LOAD_FACTOR_DEN 10
#define MHT_MIN_CAPACITY    16

/* ==================================================================== */
/* FNV-1a hash function                                                  */
/* ==================================================================== */

static uint32_t fnv1a_hash(const char *key, uint32_t *out_len)
{
    const uint8_t *p = (const uint8_t *)key;
    uint32_t h = 0x811C9DC5U;
    uint32_t len = 0;

    while (*p) {
        h ^= (uint32_t)(*p);
        h *= 0x01000193U;
        p++;
        len++;
    }

    if (out_len)
        *out_len = len;
    return h;
}

/* ==================================================================== */
/* Internal helpers                                                      */
/* ==================================================================== */

static uint32_t desired_pos(uint32_t hash, uint32_t capacity)
{
    return hash & (capacity - 1);
}

static uint32_t probe_distance(uint32_t hash, uint32_t slot_idx, uint32_t capacity)
{
    uint32_t ideal = desired_pos(hash, capacity);
    /* Account for wrap-around */
    if (slot_idx >= ideal)
        return slot_idx - ideal;
    else
        return (capacity - ideal) + slot_idx;
}

/* Check if resize is needed and grow the table */
static int mht_maybe_grow(MetaHashTable *ht)
{
    /* Resize if occupied + tombstones exceed load factor threshold */
    uint32_t threshold = (ht->capacity * MHT_LOAD_FACTOR_NUM) / MHT_LOAD_FACTOR_DEN;
    if (ht->count + ht->tombstones < threshold)
        return UMM_OK;

    uint32_t old_cap = ht->capacity;
    MHTEntry *old_entries = ht->entries;

    uint32_t new_cap = old_cap * 2;
    if (new_cap < MHT_MIN_CAPACITY)
        new_cap = MHT_MIN_CAPACITY;

    MHTEntry *new_entries = calloc(new_cap, sizeof(MHTEntry));
    if (!new_entries)
        return UMM_E_NO_MEMORY;

    ht->entries = new_entries;
    ht->capacity = new_cap;
    ht->count = 0;
    ht->tombstones = 0;

    /* Re-insert all occupied entries */
    for (uint32_t i = 0; i < old_cap; i++) {
        if (old_entries[i].state == MHT_OCCUPIED) {
            /* Find a slot in the new table */
            uint32_t hash = old_entries[i].hash;
            uint32_t idx = desired_pos(hash, new_cap);
            uint32_t dist = 0;

            while (new_entries[idx].state == MHT_OCCUPIED) {
                uint32_t existing_dist = probe_distance(new_entries[idx].hash, idx, new_cap);
                if (dist > existing_dist) {
                    /* Robin Hood: swap with the existing entry */
                    MHTEntry tmp = new_entries[idx];
                    new_entries[idx] = old_entries[i];
                    new_entries[idx].probe_dist = dist;
                    old_entries[i] = tmp;
                    dist = existing_dist;
                    hash = old_entries[i].hash;
                }
                idx = (idx + 1) & (new_cap - 1);
                dist++;
            }

            new_entries[idx] = old_entries[i];
            new_entries[idx].probe_dist = dist;
            ht->count++;
        } else if (old_entries[i].key) {
            free(old_entries[i].key);
            old_entries[i].key = NULL;
        }
    }

    free(old_entries);
    return UMM_OK;
}

/* ==================================================================== */
/* Public API                                                            */
/* ==================================================================== */

MetaHashTable* mht_create(uint32_t initial_capacity)
{
    MetaHashTable *ht = malloc(sizeof(MetaHashTable));
    if (!ht)
        return NULL;

    /* Round up to next power of 2, at least MHT_MIN_CAPACITY */
    uint32_t cap = MHT_MIN_CAPACITY;
    while (cap < initial_capacity)
        cap *= 2;

    ht->entries = calloc(cap, sizeof(MHTEntry));
    if (!ht->entries) {
        free(ht);
        return NULL;
    }

    ht->capacity = cap;
    ht->count = 0;
    ht->tombstones = 0;

    if (pthread_rwlock_init(&ht->lock, NULL) != 0) {
        free(ht->entries);
        free(ht);
        return NULL;
    }

    return ht;
}

void mht_destroy(MetaHashTable *ht)
{
    if (!ht)
        return;

    pthread_rwlock_wrlock(&ht->lock);

    for (uint32_t i = 0; i < ht->capacity; i++) {
        if (ht->entries[i].key) {
            free(ht->entries[i].key);
            ht->entries[i].key = NULL;
        }
    }
    free(ht->entries);

    pthread_rwlock_unlock(&ht->lock);
    pthread_rwlock_destroy(&ht->lock);
    free(ht);
}

int mht_insert(MetaHashTable *ht, const char *key, const ChunkMetadata *value)
{
    if (!ht || !key || !value)
        return UMM_E_INVALID_ARG;

    pthread_rwlock_wrlock(&ht->lock);

    /* Grow if needed */
    int rc = mht_maybe_grow(ht);
    if (rc != UMM_OK) {
        pthread_rwlock_unlock(&ht->lock);
        return rc;
    }

    uint32_t key_len;
    uint32_t hash = fnv1a_hash(key, &key_len);
    uint32_t capacity = ht->capacity;
    uint32_t idx = desired_pos(hash, capacity);
    uint32_t dist = 0;

    MHTEntry entry;
    memset(&entry, 0, sizeof(entry));
    entry.state = MHT_OCCUPIED;
    entry.key = strdup(key);
    if (!entry.key) {
        pthread_rwlock_unlock(&ht->lock);
        return UMM_E_NO_MEMORY;
    }
    entry.key_len = key_len;
    entry.hash = hash;
    entry.value = *value;

    while (ht->entries[idx].state == MHT_OCCUPIED) {
        /* Check for duplicate key */
        if (ht->entries[idx].hash == hash &&
            strcmp(ht->entries[idx].key, key) == 0) {
            free(entry.key);
            pthread_rwlock_unlock(&ht->lock);
            return UMM_E_ALREADY_EXISTS;
        }

        uint32_t existing_dist = probe_distance(ht->entries[idx].hash, idx, capacity);
        if (dist > existing_dist) {
            /* Robin Hood: swap with the existing entry */
            MHTEntry tmp = ht->entries[idx];
            ht->entries[idx] = entry;
            ht->entries[idx].probe_dist = dist;
            entry = tmp;
            dist = existing_dist;
            hash = entry.hash;
        }
        idx = (idx + 1) & (capacity - 1);
        dist++;
    }

    /* Place entry in empty or deleted slot */
    if (ht->entries[idx].state == MHT_DELETED) {
        if (ht->entries[idx].key)
            free(ht->entries[idx].key);
        ht->tombstones--;
    }
    ht->entries[idx] = entry;
    ht->entries[idx].probe_dist = dist;
    ht->count++;

    pthread_rwlock_unlock(&ht->lock);
    return UMM_OK;
}

int mht_lookup(MetaHashTable *ht, const char *key, ChunkMetadata *out)
{
    if (!ht || !key || !out)
        return UMM_E_INVALID_ARG;

    pthread_rwlock_rdlock(&ht->lock);

    uint32_t key_len;
    uint32_t hash = fnv1a_hash(key, &key_len);
    uint32_t capacity = ht->capacity;
    uint32_t idx = desired_pos(hash, capacity);
    uint32_t dist = 0;

    while (ht->entries[idx].state != MHT_EMPTY) {
        if (ht->entries[idx].state == MHT_OCCUPIED &&
            ht->entries[idx].hash == hash &&
            strcmp(ht->entries[idx].key, key) == 0) {
            *out = ht->entries[idx].value;
            pthread_rwlock_unlock(&ht->lock);
            return UMM_OK;
        }

        /* Robin Hood property: if we've searched further than the entry's
         * probe distance, the key is not in the table */
        if (ht->entries[idx].state == MHT_OCCUPIED &&
            probe_distance(ht->entries[idx].hash, idx, capacity) < dist) {
            break;
        }

        idx = (idx + 1) & (capacity - 1);
        dist++;

        /* Prevent infinite loop */
        if (dist > capacity)
            break;
    }

    pthread_rwlock_unlock(&ht->lock);
    return UMM_E_NOT_FOUND;
}

int mht_remove(MetaHashTable *ht, const char *key)
{
    if (!ht || !key)
        return UMM_E_INVALID_ARG;

    pthread_rwlock_wrlock(&ht->lock);

    uint32_t key_len;
    uint32_t hash = fnv1a_hash(key, &key_len);
    uint32_t capacity = ht->capacity;
    uint32_t idx = desired_pos(hash, capacity);
    uint32_t dist = 0;

    while (ht->entries[idx].state != MHT_EMPTY) {
        if (ht->entries[idx].state == MHT_OCCUPIED &&
            ht->entries[idx].hash == hash &&
            strcmp(ht->entries[idx].key, key) == 0) {
            /* Mark as deleted (tombstone) */
            ht->entries[idx].state = MHT_DELETED;
            /* Keep the key for the tombstone (needed for probing) */
            ht->count--;
            ht->tombstones++;
            pthread_rwlock_unlock(&ht->lock);
            return UMM_OK;
        }

        if (ht->entries[idx].state == MHT_OCCUPIED &&
            probe_distance(ht->entries[idx].hash, idx, capacity) < dist) {
            break;
        }

        idx = (idx + 1) & (capacity - 1);
        dist++;

        if (dist > capacity)
            break;
    }

    pthread_rwlock_unlock(&ht->lock);
    return UMM_E_NOT_FOUND;
}

int mht_exists(MetaHashTable *ht, const char *key)
{
    if (!ht || !key)
        return 0;

    ChunkMetadata tmp;
    return (mht_lookup(ht, key, &tmp) == UMM_OK) ? 1 : 0;
}

uint32_t mht_count(MetaHashTable *ht)
{
    if (!ht)
        return 0;

    pthread_rwlock_rdlock(&ht->lock);
    uint32_t c = ht->count;
    pthread_rwlock_unlock(&ht->lock);
    return c;
}

void mht_dump(MetaHashTable *ht)
{
    if (!ht)
        return;

    pthread_rwlock_rdlock(&ht->lock);

    fprintf(stderr, "=== MetaHashTable dump ===\n");
    fprintf(stderr, "  capacity=%u, count=%u, tombstones=%u\n",
            ht->capacity, ht->count, ht->tombstones);

    for (uint32_t i = 0; i < ht->capacity; i++) {
        const char *state_str = "?";
        switch (ht->entries[i].state) {
        case MHT_EMPTY:    state_str = "E"; break;
        case MHT_OCCUPIED: state_str = "O"; break;
        case MHT_DELETED:  state_str = "D"; break;
        }
        if (ht->entries[i].state == MHT_OCCUPIED) {
            fprintf(stderr, "  [%u] %s probe=%u hash=%08x key='%s'\n",
                    (unsigned)i, state_str,
                    ht->entries[i].probe_dist,
                    ht->entries[i].hash,
                    ht->entries[i].key ? ht->entries[i].key : "(null)");
        } else {
            fprintf(stderr, "  [%u] %s\n", (unsigned)i, state_str);
        }
    }
    fflush(stderr);

    pthread_rwlock_unlock(&ht->lock);
}

/* ==================================================================== */
/* Iterator                                                              */
/* ==================================================================== */

MHTIterator mht_iterator(MetaHashTable *ht)
{
    (void)ht;
    MHTIterator it = { 0 };
    return it;
}

int mht_next(MetaHashTable *ht, MHTIterator *it, const char **out_key, ChunkMetadata *out_val)
{
    if (!ht || !it)
        return UMM_E_INVALID_ARG;

    pthread_rwlock_rdlock(&ht->lock);

    while (it->idx < ht->capacity) {
        uint32_t i = it->idx++;
        if (ht->entries[i].state == MHT_OCCUPIED) {
            if (out_key)
                *out_key = ht->entries[i].key;
            if (out_val)
                *out_val = ht->entries[i].value;
            pthread_rwlock_unlock(&ht->lock);
            return UMM_OK;
        }
    }

    pthread_rwlock_unlock(&ht->lock);
    return UMM_E_NOT_FOUND; /* No more entries */
}
