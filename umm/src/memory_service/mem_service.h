#ifndef MEM_SERVICE_H
#define MEM_SERVICE_H

#include "../common/types.h"

typedef struct MemoryServiceVtbl MemoryServiceVtbl;

struct MemoryServiceVtbl {
    /* --- original interfaces (backward compatible, default operates on CXL tier) --- */
    int (*alloc_local)(void *ctx, uint64_t size, uint64_t *out_offset);
    int (*free_local)(void *ctx, uint64_t offset, uint64_t size);
    int (*get_stats)(void *ctx, uint64_t *total, uint64_t *used, uint64_t *free);

    /* --- new: tier-aware allocation --- */
    int (*alloc_tiered)(void *ctx, tier_id_t tier, uint64_t size, uint64_t *out_offset);
    int (*free_tiered)(void *ctx, tier_id_t tier, uint64_t offset, uint64_t size);
    int (*get_tier_stats)(void *ctx, tier_id_t tier,
                          uint64_t *total, uint64_t *used, uint64_t *free);

    /* --- new: storage resource management --- */
    int (*register_storage)(void *ctx, const StorageResource *res);
    int (*get_topology)(void *ctx, StorageTopology *out);

    /* --- new: device mapping (for transport layer) --- */
    int (*map_device)(void *ctx, tier_id_t tier, node_id_t node,
                      uint64_t offset, uint64_t size, void **out_ptr);
    int (*unmap_device)(void *ctx, tier_id_t tier, node_id_t node,
                        uint64_t offset, uint64_t size);
};

#endif
