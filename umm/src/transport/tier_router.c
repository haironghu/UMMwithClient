/* ========================================================================
 * tier_router.c — Tier-aware transport router
 *
 * Holds an array of per-tier MemoryTransportVtbl pointers and routes
 * every operation to the correct tier based on the tier_id embedded in
 * the GPA.  All tiers (CXL, SSD, DRAM, ...) are equal citizens.
 *
 *   gpa = [node_id:6][tier_id:2][offset:56]
 *              ↑
 *         tier = gpa_to_tier(gpa) → index into tier_vtbls[]
 *
 * No tier receives special treatment.  Adding a new tier means
 * registering one more vtbl/ctx pair — nothing in this file changes.
 * ======================================================================== */

#include "tier_router.h"
#include "../common/error_codes.h"
#include "../common/log.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* ------------------------------------------------------------------ */
/* Router context                                                     */
/* ------------------------------------------------------------------ */

struct TierRouter {
    MemoryTransportVtbl *tier_vtbls[UMM_NUM_TIERS];  /* one per tier */
    void                *tier_ctxs[UMM_NUM_TIERS];   /* matching ctx  */
    /* tier_local_unsupported[t] != 0: 该 tier 的本地数据面被有意跳过
     *（nds: 直驱设备——数据面由 worker 直连池 API 持有），
     * get/put 返回 UMM_E_UNSUPPORTED 而非笼统的 INVALID_ARG */
    uint8_t              tier_local_unsupported[UMM_NUM_TIERS];
    char                 tier_unsupported_path[UMM_NUM_TIERS][256];
    /* Phase 1：远端数据面（node 位分派）。remote_vtbl==NULL 时纯 tier
     * 分派（旧行为）；非空时 owner!=my_node 的 GPA 路由到远端。 */
    node_id_t            my_node;
    MemoryTransportVtbl *remote_vtbl;
    void                *remote_ctx;
    MemoryTransportVtbl  router_vtbl;                /* exported interface */
    pthread_mutex_t      lock;
};

/* ------------------------------------------------------------------ */
/* Forward helpers — resolve tier from GPA and dispatch                 */
/* ------------------------------------------------------------------ */

static inline MemoryTransportVtbl* resolve_vtbl(TierRouter *tr, gpa_t gpa)
{
    tier_id_t t = gpa_to_tier(gpa);
    if (t >= UMM_NUM_TIERS)
        return NULL;
    return tr->tier_vtbls[t];
}

static inline void* resolve_ctx(TierRouter *tr, gpa_t gpa)
{
    tier_id_t t = gpa_to_tier(gpa);
    if (t >= UMM_NUM_TIERS)
        return NULL;
    return tr->tier_ctxs[t];
}

/* Phase 1：node 位优先分派。owner != my_node 且已挂远端 transport 时
 * 返回 1（调用方应走 remote），否则返回 0 走既有 tier 分派。 */
static inline int route_is_remote(TierRouter *tr, gpa_t gpa)
{
    return (tr->remote_vtbl != NULL) &&
           (gpa_to_node(gpa) != tr->my_node);
}

/* ------------------------------------------------------------------ */
/* Data path — get / put                                               */
/* ------------------------------------------------------------------ */

/* 该 gpa 所属 tier 是否被有意跳过本地数据面（nds: 直驱设备）。
 * 是则记录清晰日志并返回 UMM_E_UNSUPPORTED；否则返回 0 让调用者走
 * 既有 INVALID_ARG 路径（行为不变）。 */
static int tier_local_unsupported_err(TierRouter *tr, gpa_t gpa,
                                      const char *op)
{
    tier_id_t t = gpa_to_tier(gpa);
    if (t < UMM_NUM_TIERS && tr->tier_local_unsupported[t]) {
        umm_log_error("tier_router: %s 到 tier %d 不受理——客户端本地数据面"
                      "已跳过 NDS 直驱设备 %s（数据面由 worker 直连池 API "
                      "持有，分配走 RPC）",
                      op, (int)t, tr->tier_unsupported_path[t]);
        return UMM_E_UNSUPPORTED;
    }
    return 0;
}

static int router_get(void *ctx, gpa_t gpa, uint64_t len, void *out_buf)
{
    TierRouter *tr  = ctx;
    if (route_is_remote(tr, gpa))
        return tr->remote_vtbl->get(tr->remote_ctx, gpa, len, out_buf);
    MemoryTransportVtbl *v = resolve_vtbl(tr, gpa);
    void                *c = resolve_ctx(tr, gpa);
    if (!v || !v->get) {
        int urc = tier_local_unsupported_err(tr, gpa, "read_chunk");
        return urc ? urc : UMM_E_INVALID_ARG;
    }
    return v->get(c, gpa, len, out_buf);
}

static int router_put(void *ctx, gpa_t gpa, uint64_t len, const void *buf)
{
    TierRouter *tr  = ctx;
    if (route_is_remote(tr, gpa))
        return tr->remote_vtbl->put(tr->remote_ctx, gpa, len, buf);
    MemoryTransportVtbl *v = resolve_vtbl(tr, gpa);
    void                *c = resolve_ctx(tr, gpa);
    if (!v || !v->put) {
        int urc = tier_local_unsupported_err(tr, gpa, "write_chunk");
        return urc ? urc : UMM_E_INVALID_ARG;
    }
    return v->put(c, gpa, len, buf);
}

/* ------------------------------------------------------------------ */
/* Atomic operations                                                   */
/* ------------------------------------------------------------------ */

static int router_atomic_cas(void *ctx, gpa_t gpa,
                              uint64_t expected, uint64_t desired,
                              uint64_t *old)
{
    TierRouter *tr  = ctx;
    if (route_is_remote(tr, gpa)) {
        umm_log_warn("tier_router: remote atomic_cas on gpa 0x%lx is "
                     "UNSUPPORTED (Phase 1)", (unsigned long)gpa);
        return UMM_E_UNSUPPORTED;
    }
    MemoryTransportVtbl *v = resolve_vtbl(tr, gpa);
    void                *c = resolve_ctx(tr, gpa);
    if (!v || !v->atomic_cas)
        return UMM_E_INVALID_ARG;
    return v->atomic_cas(c, gpa, expected, desired, old);
}

static int router_atomic_fetch_add(void *ctx, gpa_t gpa,
                                    uint64_t value, uint64_t *result)
{
    TierRouter *tr  = ctx;
    if (route_is_remote(tr, gpa)) {
        umm_log_warn("tier_router: remote atomic_fetch_add on gpa 0x%lx is "
                     "UNSUPPORTED (Phase 1)", (unsigned long)gpa);
        return UMM_E_UNSUPPORTED;
    }
    MemoryTransportVtbl *v = resolve_vtbl(tr, gpa);
    void                *c = resolve_ctx(tr, gpa);
    if (!v || !v->atomic_fetch_add)
        return UMM_E_INVALID_ARG;
    return v->atomic_fetch_add(c, gpa, value, result);
}

static int router_atomic_set(void *ctx, gpa_t gpa, uint64_t value)
{
    TierRouter *tr  = ctx;
    if (route_is_remote(tr, gpa)) {
        umm_log_warn("tier_router: remote atomic_set on gpa 0x%lx is "
                     "UNSUPPORTED (Phase 1)", (unsigned long)gpa);
        return UMM_E_UNSUPPORTED;
    }
    MemoryTransportVtbl *v = resolve_vtbl(tr, gpa);
    void                *c = resolve_ctx(tr, gpa);
    if (!v || !v->atomic_set)
        return UMM_E_INVALID_ARG;
    return v->atomic_set(c, gpa, value);
}

/* ------------------------------------------------------------------ */
/* Barriers — broadcast to ALL registered tiers                        */
/* ------------------------------------------------------------------ */

static void router_fence(void *ctx)
{
    TierRouter *tr = ctx;
    for (int t = 0; t < UMM_NUM_TIERS; t++) {
        if (tr->tier_vtbls[t] && tr->tier_vtbls[t]->fence)
            tr->tier_vtbls[t]->fence(tr->tier_ctxs[t]);
    }
}

static void router_barrier_all(void *ctx)
{
    TierRouter *tr = ctx;
    for (int t = 0; t < UMM_NUM_TIERS; t++) {
        if (tr->tier_vtbls[t] && tr->tier_vtbls[t]->barrier_all)
            tr->tier_vtbls[t]->barrier_all(tr->tier_ctxs[t]);
    }
}

static void router_quiet(void *ctx)
{
    TierRouter *tr = ctx;
    for (int t = 0; t < UMM_NUM_TIERS; t++) {
        if (tr->tier_vtbls[t] && tr->tier_vtbls[t]->quiet)
            tr->tier_vtbls[t]->quiet(tr->tier_ctxs[t]);
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle stubs (router itself has no resources to init)            */
/* ------------------------------------------------------------------ */

static int router_register_node(void *ctx, node_id_t node,
                                 uint64_t base, uint64_t size,
                                 const char *device)
{
    /* Delegate to CXL tier by default for node registration */
    TierRouter *tr = ctx;
    if (tr->tier_vtbls[UMM_TIER_CXL] &&
        tr->tier_vtbls[UMM_TIER_CXL]->register_node)
        return tr->tier_vtbls[UMM_TIER_CXL]->register_node(
            tr->tier_ctxs[UMM_TIER_CXL], node, base, size, device);
    return UMM_OK;
}

static int router_init(void *ctx, const UMMConfig *cfg)
{
    (void)ctx; (void)cfg;
    return UMM_OK;
}

static void router_deinit(void *ctx)
{
    TierRouter *tr = ctx;
    if (!tr)
        return;
    pthread_mutex_destroy(&tr->lock);
    free(tr);
}

/* ------------------------------------------------------------------ */
/* Factory                                                              */
/* ------------------------------------------------------------------ */

TierRouter* tier_router_create(MemoryTransportVtbl *cxl_vtbl, void *cxl_ctx,
                                MemoryTransportVtbl *ssd_vtbl, void *ssd_ctx)
{
    TierRouter *tr = calloc(1, sizeof(TierRouter));
    if (!tr)
        return NULL;

    /* Register tier backends — all equal, no special treatment */
    tr->tier_vtbls[UMM_TIER_CXL] = cxl_vtbl;
    tr->tier_ctxs[UMM_TIER_CXL]  = cxl_ctx;
    tr->tier_vtbls[UMM_TIER_SSD] = ssd_vtbl;
    tr->tier_ctxs[UMM_TIER_SSD]  = ssd_ctx;
    /* UMM_TIER_DRAM slot is free for future DRAM tier — no code changes needed */

    /* Build exported router vtable */
    tr->router_vtbl.get              = router_get;
    tr->router_vtbl.put              = router_put;
    tr->router_vtbl.atomic_cas       = router_atomic_cas;
    tr->router_vtbl.atomic_fetch_add = router_atomic_fetch_add;
    tr->router_vtbl.atomic_set       = router_atomic_set;
    tr->router_vtbl.fence            = router_fence;
    tr->router_vtbl.barrier_all      = router_barrier_all;
    tr->router_vtbl.quiet            = router_quiet;
    tr->router_vtbl.register_node    = router_register_node;
    tr->router_vtbl.init             = router_init;
    tr->router_vtbl.deinit           = router_deinit;

    pthread_mutex_init(&tr->lock, NULL);

    umm_log_info("tier_router: created with %d tier slots",
                 (int)UMM_NUM_TIERS);
    return tr;
}

void tier_router_destroy(TierRouter *tr)
{
    if (!tr)
        return;
    router_deinit(tr);
}

void tier_router_set_remote(TierRouter *tr, node_id_t my_node,
                            MemoryTransportVtbl *vtbl, void *ctx)
{
    if (!tr)
        return;
    tr->my_node     = my_node;
    tr->remote_vtbl = vtbl;
    tr->remote_ctx  = ctx;
    umm_log_info("tier_router: remote transport attached (my_node=%u)",
                 (unsigned)my_node);
}

void tier_router_set_dram(TierRouter *tr, MemoryTransportVtbl *vtbl,
                          void *ctx)
{
    if (!tr)
        return;
    tr->tier_vtbls[UMM_TIER_DRAM] = vtbl;
    tr->tier_ctxs[UMM_TIER_DRAM]  = ctx;
    umm_log_info("tier_router: DRAM tier slot attached (local malloc backend)");
}

void tier_router_mark_local_unsupported(TierRouter *tr, tier_id_t tier,
                                        const char *device_path)
{
    if (!tr || tier >= UMM_NUM_TIERS)
        return;
    tr->tier_local_unsupported[tier] = 1;
    if (device_path) {
        strncpy(tr->tier_unsupported_path[tier], device_path,
                sizeof(tr->tier_unsupported_path[tier]) - 1);
        tr->tier_unsupported_path[tier]
            [sizeof(tr->tier_unsupported_path[tier]) - 1] = '\0';
    }
}

struct MemoryTransportVtbl* tier_router_get_vtbl(TierRouter *tr)
{
    return tr ? &tr->router_vtbl : NULL;
}
