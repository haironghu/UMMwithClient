#ifndef MEM_SERVICE_DIRECT_H
#define MEM_SERVICE_DIRECT_H

#include "mem_service.h"
#include "../common/types.h"
#include "../transport/ssd_pool.h"

#ifdef __cplusplus
extern "C" {
#endif

MemoryServiceVtbl* mem_service_direct_create(node_id_t node_id, uint64_t memory_size, uint64_t base_gpa, void **out_ctx);
MemoryServiceVtbl* mem_service_direct_create_v2(node_id_t node_id, void **out_ctx);
/* Phase 2 混合池：可选内存层 tier（UMM_TIER_CXL 默认 / UMM_TIER_DRAM 无 CXL 硬件）。
 * Phase 2.5 共享内存窗口：mem_device 非 NULL 非空时作为该 tier 的后备设备
 * （如 virtio-pmem 的 /dev/pmem0，open+mmap(MAP_SHARED)；CXL tier 为 lazy，
 * DRAM tier 为注册即映射）。NULL/"" = 旧行为（CXL=mock lazy malloc，DRAM=eager
 * malloc）。显式给设备但打不开/映射失败 → 注册失败（绝不静默回退私有 malloc，
 * 否则"共享"无声退化成"私有"，实验结论即无效）。 */
MemoryServiceVtbl* mem_service_direct_create_tiered(node_id_t node_id, uint64_t memory_size, uint64_t base_gpa, tier_id_t tier, const char *mem_device, void **out_ctx);
void mem_service_direct_destroy(void *ctx);
/* 访问器：SSD tier 的 pool（无 SSD tier 返回 NULL）；供 transport 数据面
 * fence/invalidate 使用 */
SsdPool* mem_service_direct_ssd_pool(void *ctx);

#ifdef __cplusplus
}
#endif

#endif
