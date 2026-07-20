#ifndef META_SERVICE_H
#define META_SERVICE_H

#include "../common/types.h"

typedef struct MetadataServiceVtbl MetadataServiceVtbl;

/*
 * MetadataServiceVtbl — ummd 的核心接口
 *
 * All chunks are registered here. No Region concept.
 * Every chunk has a unique name and is stored in a flat hash table.
 */
struct MetadataServiceVtbl {
    /* --- original interfaces --- */
    int (*register_chunk)(void *ctx, const char *name, gpa_t gpa,
                          uint64_t size, chunk_id_t *out);
    int (*lookup_chunk)(void *ctx, const char *name, ChunkMetadata *out);
    int (*lookup_chunk_by_id)(void *ctx, chunk_id_t chunk_id, ChunkMetadata *out);
    int (*unregister_chunk)(void *ctx, chunk_id_t chunk_id);
    int (*add_ref)(void *ctx, chunk_id_t chunk_id);
    int (*release_ref)(void *ctx, chunk_id_t chunk_id);
    int (*list_chunks_by_node)(void *ctx, node_id_t node,
                               ChunkMetadata *out_array, uint32_t *inout_count);

    /* --- new: storage topology management --- */
    int (*register_storage_resource)(void *ctx, node_id_t node,
                                      const StorageResource *res);
    int (*get_storage_topology)(void *ctx, node_id_t node,
                                 StorageTopology *out);
};

#endif
