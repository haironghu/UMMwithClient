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
