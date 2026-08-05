/* ========================================================================
 * transport_ssd.c -- SSD transport (pure I/O executor)
 *
 * Phase 3 refactor: transport is no longer a storage manager.
 * All SSD storage management (chunk files, mmap) is handled by umms.
 * The transport obtains physical pointers via mem_vtbl->map_device()
 * and performs memcpy / atomic operations only.
 *
 * Direct mode: creates a local mem_service_direct instance internally.
 * RPC mode: mem_vtbl/mem_ctx point to an RPC client wrapper (Phase 5).
 *
 * Backward compatibility:
 *   - ssd_transport_create() is retained as a transitional API
 *   - ssd_transport_destroy() cleans up both ctx and internally-allocated vtbl
 * ======================================================================== */

#include "transport.h"
#include "transport_ssd.h"
#include "../memory_service/mem_service.h"
#include "../memory_service/mem_service_direct.h"
#include "../common/error_codes.h"
#include "../common/log.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* Context                                                                  */
/* ------------------------------------------------------------------------ */

typedef struct {
    MemoryServiceVtbl   *mem_vtbl;   /* <-- points to umms vtbl */
    void                *mem_ctx;     /* <-- points to umms ctx */
    MemoryTransportVtbl *my_vtbl;     /* owned vtbl (for destroy) */
} SsdTransportCtx;

/* ------------------------------------------------------------------------ */
/* MemoryTransportVtbl implementations                                      */
/* ------------------------------------------------------------------------ */

static int ssd_transport_get(void *ctx, gpa_t gpa, uint64_t len, void *out_buf)
{
    SsdTransportCtx *stx = ctx;
    void            *ptr;

    int rc = stx->mem_vtbl->map_device(stx->mem_ctx, UMM_TIER_SSD,
                                        gpa_to_node(gpa), gpa_to_offset(gpa),
                                        len, &ptr);
    if (rc == UMM_OK) {
        /* 文件后端：mmap 指针，memcpy 通路 */
        memcpy(out_buf, ptr, (size_t)len);
        return UMM_OK;
    }

    /* 块设备后端：无 mmap，回退到主机 I/O 通路（pread） */
    if (stx->mem_vtbl->ssd_read) {
        return stx->mem_vtbl->ssd_read(stx->mem_ctx, UMM_TIER_SSD,
                                       gpa_to_node(gpa), gpa_to_offset(gpa),
                                       len, out_buf);
    }
    return rc;
}

static int ssd_transport_put(void *ctx, gpa_t gpa, uint64_t len,
                             const void *buf)
{
    SsdTransportCtx *stx = ctx;
    void            *ptr;

    int rc = stx->mem_vtbl->map_device(stx->mem_ctx, UMM_TIER_SSD,
                                        gpa_to_node(gpa), gpa_to_offset(gpa),
                                        len, &ptr);
    if (rc == UMM_OK) {
        /* 文件后端：mmap 指针，memcpy 通路 */
        memcpy(ptr, buf, (size_t)len);
        return UMM_OK;
    }

    /* libnvm 后端：无 mmap，回退到主机 I/O 通路（ssd_pool_pwrite） */
    if (stx->mem_vtbl->ssd_write) {
        return stx->mem_vtbl->ssd_write(stx->mem_ctx, UMM_TIER_SSD,
                                        gpa_to_node(gpa), gpa_to_offset(gpa),
                                        len, buf);
    }
    return rc;
}

/* ------------------------------------------------------------------------ */
/* Atomic operations (software implementation on mapped memory)             */
/* ------------------------------------------------------------------------ */

static int ssd_transport_atomic_cas(void *ctx, gpa_t gpa,
                                    uint64_t expected, uint64_t desired,
                                    uint64_t *old)
{
    SsdTransportCtx *stx = ctx;
    void            *ptr;

    int rc = stx->mem_vtbl->map_device(stx->mem_ctx, UMM_TIER_SSD,
                                        gpa_to_node(gpa), gpa_to_offset(gpa),
                                        sizeof(uint64_t), &ptr);
    if (rc != UMM_OK)
        return rc;

    volatile uint64_t *cell = (volatile uint64_t *)ptr;
    uint64_t prev = *cell;
    if (prev == expected)
        *cell = desired;
    if (old)
        *old = prev;
    return UMM_OK;
}

static int ssd_transport_atomic_fetch_add(void *ctx, gpa_t gpa,
                                          uint64_t value, uint64_t *result)
{
    SsdTransportCtx *stx = ctx;
    void            *ptr;

    int rc = stx->mem_vtbl->map_device(stx->mem_ctx, UMM_TIER_SSD,
                                        gpa_to_node(gpa), gpa_to_offset(gpa),
                                        sizeof(uint64_t), &ptr);
    if (rc != UMM_OK)
        return rc;

    uint64_t prev = __sync_fetch_and_add((volatile uint64_t *)ptr, value);
    if (result)
        *result = prev;
    return UMM_OK;
}

static int ssd_transport_atomic_set(void *ctx, gpa_t gpa, uint64_t value)
{
    SsdTransportCtx *stx = ctx;
    void            *ptr;

    int rc = stx->mem_vtbl->map_device(stx->mem_ctx, UMM_TIER_SSD,
                                        gpa_to_node(gpa), gpa_to_offset(gpa),
                                        sizeof(uint64_t), &ptr);
    if (rc != UMM_OK)
        return rc;

    *(volatile uint64_t *)ptr = value;
    return UMM_OK;
}

/* ------------------------------------------------------------------------ */
/* Memory barriers                                                          */
/* ------------------------------------------------------------------------ */

static void ssd_transport_fence(void *ctx)
{
    SsdTransportCtx *stx = ctx;

    /* CPU 屏障（原有语义） */
    __sync_synchronize();

    /* 落盘（语义修正）：存储 tier 的 fence 必须含持久化——此前只有
     * CPU 屏障，mmap 脏页仍滞留页缓存，"写完落盘"无从保证。
     * 对文件/块设备后端做全池 msync(MS_SYNC)；无 pool（或未注册
     * SSD tier）时退化为纯屏障。libnvm/NDS 后端无 mmap（MAP_FAILED），
     * ssd_backend_sync 会判无效跳过——直驱设备本就 bypass 页缓存。 */
    if (stx && stx->mem_ctx) {
        SsdPool *pool = mem_service_direct_ssd_pool(stx->mem_ctx);
        if (pool) {
            int rc = ssd_pool_sync(pool, 0,
                                   ssd_pool_total_capacity(pool));
            if (rc != UMM_OK)
                umm_log_warn("ssd_transport_fence: pool sync rc=%d", rc);
        }
    }
}

/* ssd_transport_invalidate — 丢弃 SSD 数据面的缓存页视图。
 * 共享盘读共享场景：对端节点 umm_write + umm_fence 落盘后，本端须先
 * invalidate 再读，否则 mmap 页缓存返回旧数据（guest 页缓存不会
 * 自动感知另一 VM 经同一后备文件的写入）。 */
int ssd_transport_invalidate(void *ctx, gpa_t gpa, uint64_t len)
{
    SsdTransportCtx *stx = ctx;
    if (!stx || !stx->mem_ctx || len == 0)
        return UMM_E_INVALID_ARG;
    SsdPool *pool = mem_service_direct_ssd_pool(stx->mem_ctx);
    if (!pool)
        return UMM_E_UNSUPPORTED;
    return ssd_pool_invalidate(pool, gpa_to_offset(gpa), len);
}

static void ssd_transport_barrier_all(void *ctx)
{
    (void)ctx;
    __sync_synchronize();
}

static void ssd_transport_quiet(void *ctx)
{
    (void)ctx;
    __sync_synchronize();
}

/* ------------------------------------------------------------------------ */
/* Lifecycle                                                                */
/* ------------------------------------------------------------------------ */

static int ssd_transport_register_node(void *ctx, node_id_t node,
                                       uint64_t base, uint64_t size,
                                       const char *device)
{
    (void)node;
    SsdTransportCtx *stx = ctx;

    StorageResource res = {
        .tier        = UMM_TIER_SSD,
        .capacity    = size,
        .base_offset = base,
        .online      = 1,
    };
    strncpy(res.device_path, device ? device : "ssd",
            sizeof(res.device_path) - 1);

    return stx->mem_vtbl->register_storage(stx->mem_ctx, &res);
}

static int ssd_transport_init(void *ctx, const UMMConfig *cfg)
{
    SsdTransportCtx *stx = ctx;
    (void)cfg;

    /* In the new architecture, mem_vtbl/mem_ctx are provided externally
     * (set by ssd_transport_create or by Phase 5 init). */
    if (!stx->mem_vtbl || !stx->mem_ctx)
        return UMM_E_NOT_INITIALIZED;

    return UMM_OK;
}

static void ssd_transport_deinit(void *ctx)
{
    SsdTransportCtx *stx = ctx;
    if (!stx)
        return;

    /* Destroy local mem_service instance if present */
    if (stx->mem_ctx) {
        mem_service_direct_destroy(stx->mem_ctx);
        stx->mem_ctx = NULL;
    }
    stx->mem_vtbl = NULL;
}

/* ------------------------------------------------------------------------ */
/* Factory (transitional API)                                               */
/* ------------------------------------------------------------------------ */

MemoryTransportVtbl* ssd_transport_create(const char *base_dir,
                                          uint64_t max_bytes,
                                          void **out_ctx)
{
    (void)base_dir;  /* storage path now managed by umms */

    if (!out_ctx)
        return NULL;

    MemoryTransportVtbl *vtbl = calloc(1, sizeof(MemoryTransportVtbl));
    if (!vtbl)
        return NULL;

    SsdTransportCtx *stx = calloc(1, sizeof(SsdTransportCtx));
    if (!stx) {
        free(vtbl);
        return NULL;
    }

    /* Create local memory service for direct mode */
    void *mem_ctx = NULL;
    MemoryServiceVtbl *mem_vtbl = mem_service_direct_create(0, max_bytes, 0,
                                                             &mem_ctx);
    if (!mem_vtbl || !mem_ctx) {
        free(stx);
        free(vtbl);
        return NULL;
    }

    stx->mem_vtbl = mem_vtbl;
    stx->mem_ctx  = mem_ctx;
    stx->my_vtbl  = vtbl;

    /* Register SSD storage */
    StorageResource res = {
        .tier        = UMM_TIER_SSD,
        .capacity    = max_bytes,
        .base_offset = 0,
        .online      = 1,
    };
    strncpy(res.device_path, base_dir ? base_dir : "/tmp/umm_ssd",
            sizeof(res.device_path) - 1);
    mem_vtbl->register_storage(mem_ctx, &res);

    vtbl->get              = ssd_transport_get;
    vtbl->put              = ssd_transport_put;
    vtbl->atomic_cas       = ssd_transport_atomic_cas;
    vtbl->atomic_fetch_add = ssd_transport_atomic_fetch_add;
    vtbl->atomic_set       = ssd_transport_atomic_set;
    vtbl->fence            = ssd_transport_fence;
    vtbl->barrier_all      = ssd_transport_barrier_all;
    vtbl->quiet            = ssd_transport_quiet;
    vtbl->register_node    = ssd_transport_register_node;
    vtbl->init             = ssd_transport_init;
    vtbl->deinit           = ssd_transport_deinit;

    *out_ctx = stx;
    return vtbl;
}

MemoryTransportVtbl* ssd_transport_create_multi(const SsdDeviceConfig *devs,
                                                 uint32_t num_devs,
                                                 void **out_ctx)
{
    if (!devs || num_devs == 0 || !out_ctx)
        return NULL;

    uint64_t total = 0;
    for (uint32_t i = 0; i < num_devs; i++)
        total += devs[i].size;

    MemoryTransportVtbl *vtbl = calloc(1, sizeof(MemoryTransportVtbl));
    if (!vtbl)
        return NULL;

    SsdTransportCtx *stx = calloc(1, sizeof(SsdTransportCtx));
    if (!stx) {
        free(vtbl);
        return NULL;
    }

    void *mem_ctx = NULL;
    MemoryServiceVtbl *mem_vtbl = mem_service_direct_create(0, total, 0,
                                                             &mem_ctx);
    if (!mem_vtbl || !mem_ctx) {
        free(stx);
        free(vtbl);
        return NULL;
    }

    stx->mem_vtbl = mem_vtbl;
    stx->mem_ctx  = mem_ctx;
    stx->my_vtbl  = vtbl;

    /* 逐设备注册进同一 pool（pool 内部维护虚拟偏移拼接）；
     * 任一失败即整体回滚——共享池实验里设备打不开必须当场暴露 */
    for (uint32_t i = 0; i < num_devs; i++) {
        StorageResource res = {
            .tier        = UMM_TIER_SSD,
            .capacity    = devs[i].size,
            .base_offset = 0,
            .online      = 1,
        };
        strncpy(res.device_path, devs[i].path,
                sizeof(res.device_path) - 1);
        int rc = mem_vtbl->register_storage(mem_ctx, &res);
        if (rc != UMM_OK) {
            umm_log_error("ssd_transport_multi: register device[%u] %s "
                          "failed (rc=%d), rolling back",
                          i, devs[i].path, rc);
            mem_service_direct_destroy(mem_ctx);
            free(stx);
            free(vtbl);
            return NULL;
        }
    }
    umm_log_info("ssd_transport_multi: %u device(s), total=%lu MB",
                 num_devs, (unsigned long)(total / (1024 * 1024)));

    vtbl->get              = ssd_transport_get;
    vtbl->put              = ssd_transport_put;
    vtbl->atomic_cas       = ssd_transport_atomic_cas;
    vtbl->atomic_fetch_add = ssd_transport_atomic_fetch_add;
    vtbl->atomic_set       = ssd_transport_atomic_set;
    vtbl->fence            = ssd_transport_fence;
    vtbl->barrier_all      = ssd_transport_barrier_all;
    vtbl->quiet            = ssd_transport_quiet;
    vtbl->register_node    = ssd_transport_register_node;
    vtbl->init             = ssd_transport_init;
    vtbl->deinit           = ssd_transport_deinit;

    *out_ctx = stx;
    return vtbl;
}

/* ------------------------------------------------------------------------ */
/* Public destructor                                                        */
/* ------------------------------------------------------------------------ */

void ssd_transport_destroy(void *ctx)
{
    SsdTransportCtx *stx = ctx;
    if (!stx)
        return;

    ssd_transport_deinit(ctx);

    /* Free the vtbl if it was heap-allocated by ssd_transport_create */
    if (stx->my_vtbl)
        free(stx->my_vtbl);

    free(stx);
}
