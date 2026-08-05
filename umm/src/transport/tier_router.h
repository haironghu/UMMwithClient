/* ========================================================================
 * tier_router.h — Tier-aware transport router (public interface)
 *
 * The TierRouter holds an array of per-tier MemoryTransportVtbl pointers
 * and routes every operation to the correct tier based on the tier_id
 * embedded in the GPA.
 *
 *   gpa = [node_id:6][tier_id:2][offset:56]
 *              ↑
 *         tier = gpa_to_tier(gpa) -> index into tier_vtbls[]
 *
 * All tiers (CXL, SSD, DRAM, ...) are equal citizens.
 * ======================================================================== */

#ifndef TIER_ROUTER_H
#define TIER_ROUTER_H

#include "../../include/umm.h"
#include "transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TierRouter TierRouter;

/**
 * tier_router_create — Create a tier router.
 *
 * @param cxl_vtbl  CXL/mock transport vtable (for UMM_TIER_CXL).
 * @param cxl_ctx   CXL transport context.
 * @param ssd_vtbl  SSD transport vtable (for UMM_TIER_SSD), or NULL.
 * @param ssd_ctx   SSD transport context, or NULL.
 * @return          New TierRouter, or NULL on error.
 */
TierRouter* tier_router_create(MemoryTransportVtbl *cxl_vtbl, void *cxl_ctx,
                                MemoryTransportVtbl *ssd_vtbl, void *ssd_ctx);

/**
 * tier_router_destroy — Destroy tier router and free resources.
 */
void tier_router_destroy(TierRouter *tr);

/**
 * tier_router_mark_local_unsupported — Mark a tier's local dataplane as
 * intentionally unsupported (e.g. an "nds:" direct-drive device whose
 * dataplane is owned by the worker's direct pool API, not by the client
 * library).  Subsequent get/put to that tier fail with UMM_E_UNSUPPORTED
 * and a clear log line instead of a generic UMM_E_INVALID_ARG.
 *
 * @param tr          Router instance.
 * @param tier        Tier to mark.
 * @param device_path Device path (for diagnostics), may be NULL.
 */
void tier_router_mark_local_unsupported(TierRouter *tr, tier_id_t tier,
                                        const char *device_path);

/**
 * tier_router_set_remote — 挂载远端数据面 transport（Phase 1）。
 *
 * 挂载后，get/put 先查 GPA node 位：owner != my_node 的 GPA 路由到
 * remote transport（跨节点 RPC），owner == my_node 维持 tier 分派。
 * 未调用本函数（remote == NULL）时行为与旧版完全一致（纯 tier 分派）。
 * 远端原子操作不支持，明确返回 UMM_E_UNSUPPORTED。
 *
 * @param tr        Router 实例。
 * @param my_node   本节点 ID。
 * @param vtbl      远端 transport vtbl（ownership 归调用方，router 不销毁）。
 * @param ctx       远端 transport 上下文。
 */
void tier_router_set_remote(TierRouter *tr, node_id_t my_node,
                            MemoryTransportVtbl *vtbl, void *ctx);

/**
 * tier_router_set_dram — 填充 DRAM tier 槽位（Phase 2 混合池）。
 *
 * tier_router_create 只填 CXL/SSD 两个槽位；无 CXL 硬件的部署里，
 * 客户端本地内存数据面（malloc 后备）应挂到 DRAM 槽位，使
 * umm_alloc_tiered(size, UMM_TIER_DRAM) 的 GPA 数据面有处可去。
 * ownership 归调用方，router 不销毁。
 */
void tier_router_set_dram(TierRouter *tr, MemoryTransportVtbl *vtbl,
                          void *ctx);

/**
 * tier_router_get_vtbl — Get the router's transport vtable.
 *
 * The returned vtable's get/put/atomic/fence will route to the correct
 * tier backend based on the tier_id field in the GPA.
 */
MemoryTransportVtbl* tier_router_get_vtbl(TierRouter *tr);

#ifdef __cplusplus
}
#endif

#endif /* TIER_ROUTER_H */
