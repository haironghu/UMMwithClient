/* ========================================================================
 * meta_service_direct.c — In-process metadata service (v2.0)
 *
 * Flat chunk directory. No Region concept.
 * Every chunk has a unique name, stored in a Robin Hood hash table.
 * All memory is CXL shared memory.
 * ======================================================================== */

#include "meta_service.h"
#include "meta_hash_table.h"
#include "../common/error_codes.h"
#include "../common/log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>

/* ==================================================================== */
/* Internal types                                                       */
/* ==================================================================== */

typedef struct {
    chunk_id_t    chunk_id;
    uint32_t      ref_count;
    int           valid;        /* 1 = active, 0 = free slot */
    ChunkMetadata meta;
} ChunkEntry;

/* ==================================================================== */
/* Context                                                               */
/* ==================================================================== */

typedef struct {
    MetaHashTable    *chunk_table;      /* name -> ChunkMetadata */
    ChunkEntry       *chunks;           /* array of chunk entries */
    uint32_t          chunk_capacity;
    chunk_id_t        next_chunk_id;
    pthread_rwlock_t  lock;

    /* --- 新增：存储资源表 --- */
    StorageTopology  topologies[64];    /* per-node storage topology, max 64 nodes */
    uint32_t          num_topologies;    /* number of registered nodes */
} MetaServiceCtx;

/* ==================================================================== */
/* Helpers                                                               */
/* ==================================================================== */

/* Find chunk entry by chunk_id, return pointer or NULL */
static ChunkEntry* find_chunk_by_id(MetaServiceCtx *ctx, chunk_id_t chunk_id)
{
    if (chunk_id == 0 || chunk_id >= ctx->next_chunk_id)
        return NULL;
    ChunkEntry *ce = &ctx->chunks[chunk_id - 1];
    if (!ce->valid)
        return NULL;
    return ce;
}

/* Ensure chunks array has room up to chunk_id */
static int ensure_chunk_capacity(MetaServiceCtx *ctx, chunk_id_t needed_id)
{
    uint32_t needed_idx = (uint32_t)needed_id;
    if (needed_idx <= ctx->chunk_capacity)
        return UMM_OK;

    uint32_t new_cap = ctx->chunk_capacity == 0 ? 16 : ctx->chunk_capacity;
    while (new_cap < needed_idx)
        new_cap *= 2;

    ChunkEntry *new_chunks = realloc(ctx->chunks, new_cap * sizeof(ChunkEntry));
    if (!new_chunks)
        return UMM_E_NO_MEMORY;

    memset(&new_chunks[ctx->chunk_capacity], 0,
           (new_cap - ctx->chunk_capacity) * sizeof(ChunkEntry));

    ctx->chunks = new_chunks;
    ctx->chunk_capacity = new_cap;
    return UMM_OK;
}

/* ==================================================================== */
/* Vtable method implementations                                        */
/* ==================================================================== */

static int impl_register_chunk(void *ctx_, const char *name, gpa_t gpa,
                               uint64_t size, chunk_id_t *out)
{
    MetaServiceCtx *ctx = (MetaServiceCtx *)ctx_;
    if (!ctx || !name || !out)
        return UMM_E_INVALID_ARG;

    pthread_rwlock_wrlock(&ctx->lock);

    /* Check for duplicate name */
    ChunkMetadata tmp;
    if (mht_lookup(ctx->chunk_table, name, &tmp) == UMM_OK) {
        pthread_rwlock_unlock(&ctx->lock);
        return UMM_E_ALREADY_EXISTS;
    }

    /* Allocate chunk_id */
    chunk_id_t cid = ctx->next_chunk_id++;
    if (cid == 0) cid = ctx->next_chunk_id++;

    int rc = ensure_chunk_capacity(ctx, cid);
    if (rc != UMM_OK) {
        pthread_rwlock_unlock(&ctx->lock);
        return rc;
    }

    /* Store chunk entry */
    ChunkEntry *ce = &ctx->chunks[cid - 1];
    memset(ce, 0, sizeof(ChunkEntry));
    ce->chunk_id = cid;
    ce->ref_count = 1;  /* initial ref */
    ce->valid = 1;
    ce->meta.chunk_id = cid;  /* <-- also set in metadata for hash table */
    strncpy(ce->meta.name, name, 63);
    ce->meta.name[63] = '\0';
    ce->meta.gpa = gpa;
    ce->meta.size = size;

    /* Insert into hash table */
    rc = mht_insert(ctx->chunk_table, name, &ce->meta);
    if (rc != UMM_OK) {
        ce->valid = 0;
        pthread_rwlock_unlock(&ctx->lock);
        return rc;
    }

    *out = cid;
    pthread_rwlock_unlock(&ctx->lock);
    return UMM_OK;
}

static int impl_lookup_chunk(void *ctx_, const char *name,
                             ChunkMetadata *out)
{
    MetaServiceCtx *ctx = (MetaServiceCtx *)ctx_;
    if (!ctx || !name || !out)
        return UMM_E_INVALID_ARG;

    pthread_rwlock_rdlock(&ctx->lock);
    int rc = mht_lookup(ctx->chunk_table, name, out);
    pthread_rwlock_unlock(&ctx->lock);
    return rc;
}

static int impl_lookup_chunk_by_id(void *ctx_, chunk_id_t chunk_id,
                                    ChunkMetadata *out)
{
    MetaServiceCtx *ctx = (MetaServiceCtx *)ctx_;
    if (!ctx || !out)
        return UMM_E_INVALID_ARG;

    pthread_rwlock_rdlock(&ctx->lock);

    ChunkEntry *ce = find_chunk_by_id(ctx, chunk_id);
    if (!ce) {
        pthread_rwlock_unlock(&ctx->lock);
        return UMM_E_NOT_FOUND;
    }

    *out = ce->meta;
    pthread_rwlock_unlock(&ctx->lock);
    return UMM_OK;
}

static int impl_unregister_chunk(void *ctx_, chunk_id_t chunk_id)
{
    MetaServiceCtx *ctx = (MetaServiceCtx *)ctx_;
    if (!ctx)
        return UMM_E_INVALID_ARG;

    pthread_rwlock_wrlock(&ctx->lock);

    ChunkEntry *ce = find_chunk_by_id(ctx, chunk_id);
    if (!ce) {
        pthread_rwlock_unlock(&ctx->lock);
        return UMM_E_NOT_FOUND;
    }

    mht_remove(ctx->chunk_table, ce->meta.name);
    ce->valid = 0;

    pthread_rwlock_unlock(&ctx->lock);
    return UMM_OK;
}

static int impl_add_ref(void *ctx_, chunk_id_t chunk_id)
{
    MetaServiceCtx *ctx = (MetaServiceCtx *)ctx_;
    if (!ctx)
        return UMM_E_INVALID_ARG;

    pthread_rwlock_wrlock(&ctx->lock);

    ChunkEntry *ce = find_chunk_by_id(ctx, chunk_id);
    if (!ce) {
        pthread_rwlock_unlock(&ctx->lock);
        return UMM_E_NOT_FOUND;
    }

    ce->ref_count++;

    pthread_rwlock_unlock(&ctx->lock);
    return UMM_OK;
}

static int impl_release_ref(void *ctx_, chunk_id_t chunk_id)
{
    MetaServiceCtx *ctx = (MetaServiceCtx *)ctx_;
    if (!ctx)
        return UMM_E_INVALID_ARG;

    pthread_rwlock_wrlock(&ctx->lock);

    ChunkEntry *ce = find_chunk_by_id(ctx, chunk_id);
    if (!ce) {
        pthread_rwlock_unlock(&ctx->lock);
        return UMM_E_NOT_FOUND;
    }

    if (ce->ref_count > 0)
        ce->ref_count--;

    if (ce->ref_count == 0) {
        /* Auto-unregister */
        mht_remove(ctx->chunk_table, ce->meta.name);
        ce->valid = 0;
    }

    pthread_rwlock_unlock(&ctx->lock);
    return UMM_OK;
}

static int impl_list_chunks_by_node(void *ctx_, node_id_t node,
                                     ChunkMetadata *out_array,
                                     uint32_t *inout_count)
{
    MetaServiceCtx *ctx = (MetaServiceCtx *)ctx_;
    if (!ctx || !inout_count)
        return UMM_E_INVALID_ARG;

    uint32_t max_out = *inout_count;
    uint32_t found = 0;

    pthread_rwlock_rdlock(&ctx->lock);

    for (chunk_id_t cid = 1; cid < ctx->next_chunk_id && found < max_out; cid++) {
        ChunkEntry *ce = find_chunk_by_id(ctx, cid);
        if (!ce)
            continue;
        if (gpa_to_node(ce->meta.gpa) == node) {
            if (out_array)
                out_array[found] = ce->meta;
            found++;
        }
    }

    pthread_rwlock_unlock(&ctx->lock);

    *inout_count = found;
    return UMM_OK;
}

static int impl_register_storage_resource(void *ctx_, node_id_t node,
                                          const StorageResource *res)
{
    MetaServiceCtx *ctx = (MetaServiceCtx *)ctx_;
    if (!ctx || !res)
        return UMM_E_INVALID_ARG;
    if (res->tier >= UMM_NUM_TIERS)
        return UMM_E_INVALID_ARG;

    pthread_rwlock_wrlock(&ctx->lock);

    /* Look for existing topology for this node */
    StorageTopology *topo = NULL;
    for (uint32_t i = 0; i < ctx->num_topologies; i++) {
        if (ctx->topologies[i].node_id == node) {
            topo = &ctx->topologies[i];
            break;
        }
    }

    if (!topo) {
        /* New node — add a fresh topology entry */
        if (ctx->num_topologies >= 64) {
            pthread_rwlock_unlock(&ctx->lock);
            return UMM_E_NO_MEMORY;
        }
        topo = &ctx->topologies[ctx->num_topologies++];
        memset(topo, 0, sizeof(StorageTopology));
        topo->node_id = node;
    }

    /* Update / overwrite resource for this tier */
    topo->resources[res->tier] = *res;

    /* Recalculate num_resources */
    topo->num_resources = 0;
    for (int t = 0; t < UMM_NUM_TIERS; t++) {
        if (topo->resources[t].online)
            topo->num_resources++;
    }

    pthread_rwlock_unlock(&ctx->lock);

    umm_log_info("ummd: REGISTER_STORAGE node=%u tier=%u dev=%s cap=%lu -> OK",
                 (unsigned)node, (unsigned)res->tier,
                 res->device_path, (unsigned long)res->capacity);
    return UMM_OK;
}

static int impl_get_storage_topology(void *ctx_, node_id_t node,
                                     StorageTopology *out)
{
    MetaServiceCtx *ctx = (MetaServiceCtx *)ctx_;
    if (!ctx || !out)
        return UMM_E_INVALID_ARG;

    pthread_rwlock_rdlock(&ctx->lock);

    for (uint32_t i = 0; i < ctx->num_topologies; i++) {
        if (ctx->topologies[i].node_id == node) {
            /* 内部按 tier 稀疏存放，对外返回紧凑数组——
             * 线协议 pack 从 resources[0] 连续取 num_resources 项，
             * 稀疏布局会导致空槽上线、真实资源丢失 */
            StorageTopology *src = &ctx->topologies[i];
            memset(out, 0, sizeof(*out));
            out->node_id = src->node_id;
            for (int t = 0; t < UMM_NUM_TIERS; t++) {
                if (src->resources[t].online)
                    out->resources[out->num_resources++] = src->resources[t];
            }
            pthread_rwlock_unlock(&ctx->lock);

            umm_log_info("ummd: GET_TOPOLOGY node=%u -> found %u resources",
                         (unsigned)node, out->num_resources);
            return UMM_OK;
        }
    }

    pthread_rwlock_unlock(&ctx->lock);

    umm_log_info("ummd: GET_TOPOLOGY node=%u -> NOT_FOUND", (unsigned)node);
    return UMM_E_NOT_FOUND;
}

/* ==================================================================== */
/* Factory                                                               */
/* ==================================================================== */

MetadataServiceVtbl* meta_service_direct_create(void **out_ctx)
{
    if (!out_ctx)
        return NULL;

    MetaServiceCtx *ctx = calloc(1, sizeof(MetaServiceCtx));
    if (!ctx)
        return NULL;

    ctx->chunk_table = mht_create(64);
    if (!ctx->chunk_table) {
        free(ctx);
        return NULL;
    }

    ctx->next_chunk_id = 1;
    ctx->num_topologies = 0;

    if (pthread_rwlock_init(&ctx->lock, NULL) != 0) {
        mht_destroy(ctx->chunk_table);
        free(ctx);
        return NULL;
    }

    MetadataServiceVtbl *vtbl = malloc(sizeof(MetadataServiceVtbl));
    if (!vtbl) {
        pthread_rwlock_destroy(&ctx->lock);
        mht_destroy(ctx->chunk_table);
        free(ctx);
        return NULL;
    }

    vtbl->register_chunk              = impl_register_chunk;
    vtbl->lookup_chunk                = impl_lookup_chunk;
    vtbl->lookup_chunk_by_id          = impl_lookup_chunk_by_id;
    vtbl->unregister_chunk            = impl_unregister_chunk;
    vtbl->add_ref                     = impl_add_ref;
    vtbl->release_ref                 = impl_release_ref;
    vtbl->list_chunks_by_node         = impl_list_chunks_by_node;
    vtbl->register_storage_resource   = impl_register_storage_resource;
    vtbl->get_storage_topology        = impl_get_storage_topology;

    *out_ctx = ctx;
    return vtbl;
}

void meta_service_direct_destroy(void *ctx_)
{
    MetaServiceCtx *ctx = (MetaServiceCtx *)ctx_;
    if (!ctx)
        return;

    pthread_rwlock_wrlock(&ctx->lock);

    if (ctx->chunk_table)
        mht_destroy(ctx->chunk_table);
    free(ctx->chunks);

    pthread_rwlock_unlock(&ctx->lock);
    pthread_rwlock_destroy(&ctx->lock);
    free(ctx);
}
