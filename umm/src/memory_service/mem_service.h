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

    /* --- optional: SSD 块设备后端的主机 I/O 回退通路 ---
     * 当 SSD tier 由真实块设备（无 mmap）纳管时，map_device 返回失败，
     * transport 层应改用此接口（pread/pwrite 语义）。
     * offset 语义与 map_device 相同（tier 全局偏移，含 base_offset）。
     * 文件后端（mmap 可用）或未实现时置 NULL。 */
    int (*ssd_read)(void *ctx, tier_id_t tier, node_id_t node,
                    uint64_t offset, uint64_t len, void *buf);
    int (*ssd_write)(void *ctx, tier_id_t tier, node_id_t node,
                     uint64_t offset, uint64_t len, const void *buf);
};

#endif
