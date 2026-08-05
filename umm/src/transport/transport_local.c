/* ========================================================================
 * transport_local.c  --  Local transport (pure I/O executor)
 *
 * Merged from transport_mock.c + transport_cxl.c (Phase 3 refactor).
 * Both transports followed the same map_device -> memcpy pattern.
 * This unified backend uses hardware atomic builtins with a mutex lock
 * for thread safety.
 *
 * Direct mode: creates a local mem_service_direct instance internally.
 * RPC mode: mem_vtbl/mem_ctx point to an RPC client wrapper (Phase 5).
 * ======================================================================== */

#include "transport.h"
#include "../memory_service/mem_service.h"
#include "../memory_service/mem_service_direct.h"
#include "../common/log.h"
#include "../common/error_codes.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdarg.h>

/* ------------------------------------------------------------------------ */
/* Internal context                                                         */
/* ------------------------------------------------------------------------ */

typedef struct {
    MemoryServiceVtbl *mem_vtbl;   /* <-- points to umms vtbl */
    void              *mem_ctx;     /* <-- points to umms ctx */
    node_id_t          local_node;
    pthread_mutex_t    lock;
} LocalTransportCtx;

/* ------------------------------------------------------------------------ */
/* Logging helpers                                                          */
/* ------------------------------------------------------------------------ */

static void log_error(const char *file, int line, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    umm_log(UMM_LOG_ERROR, file, line, fmt, ap);
    va_end(ap);
}

static void log_info(const char *file, int line, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    umm_log(UMM_LOG_INFO, file, line, fmt, ap);
    va_end(ap);
}

static void log_warn(const char *file, int line, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    umm_log(UMM_LOG_WARN, file, line, fmt, ap);
    va_end(ap);
}

#define local_error(...) log_error(__FILE__, __LINE__, __VA_ARGS__)
#define local_info(...)  log_info(__FILE__, __LINE__, __VA_ARGS__)
#define local_warn(...)  log_warn(__FILE__, __LINE__, __VA_ARGS__)

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

/**
 * Resolve a GPA to a host pointer via umms->map_device().
 *
 * @return  UMM_OK on success, error code otherwise
 */
static int local_resolve(LocalTransportCtx *ctx, gpa_t gpa, uint64_t len, void **out)
{
    node_id_t  node   = gpa_to_node(gpa);
    uint64_t   offset = gpa_to_offset(gpa);

    return ctx->mem_vtbl->map_device(ctx->mem_ctx, gpa_to_tier(gpa),
                                      node, offset, len, out);
}

/* ------------------------------------------------------------------------ */
/* VTable implementations                                                   */
/* ------------------------------------------------------------------------ */

/*
 * P0 锁粒度优化：
 *   锁内只完成 GPA → host 指针解析（local_resolve，本身很快），
 *   数据搬运 memcpy 移出锁外执行。
 *
 *   原实现把 memcpy 也包在锁里，导致同一 transport 实例上所有读写
 *   被完全串行化——上层并发引擎（如 bmpclient ConcurrentIOEngine）
 *   开再多线程也无法并行。收窄后，多线程可对不同地址区间并行 memcpy，
 *   与 ssd_transport 的并发行为对齐。
 *
 *   并发契约（与 ssd_transport 一致）：
 *   - 对不同地址区间的并发 get/put 是安全的；
 *   - 对重叠区间的并发写无原子性保证，上层必须自行保序；
 *   - local_deinit / mem_service 销毁必须等所有在途 I/O 完成后调用
 *     （调用方负责 drain，例如先 close 并发引擎再 umm_deinit）。
 */
static int local_get(void *ctx, gpa_t gpa, uint64_t len, void *out_buf)
{
    LocalTransportCtx *local = (LocalTransportCtx *)ctx;
    void              *src;
    int                rc;

    if (!out_buf && len > 0)
        return UMM_E_INVALID_ARG;

    /* 锁内：仅解析指针（mem_service 内部另有短锁保护其状态） */
    pthread_mutex_lock(&local->lock);
    rc = local_resolve(local, gpa, len, &src);
    pthread_mutex_unlock(&local->lock);

    /* 锁外：数据搬运可与其他线程并行 */
    if (rc == UMM_OK)
        memcpy(out_buf, src, (size_t)len);
    return rc;
}

static int local_put(void *ctx, gpa_t gpa, uint64_t len, const void *buf)
{
    LocalTransportCtx *local = (LocalTransportCtx *)ctx;
    void              *dst;
    int                rc;

    if (!buf && len > 0)
        return UMM_E_INVALID_ARG;

    /* 锁内：仅解析指针 */
    pthread_mutex_lock(&local->lock);
    rc = local_resolve(local, gpa, len, &dst);
    pthread_mutex_unlock(&local->lock);

    /* 锁外：数据搬运可与其他线程并行 */
    if (rc == UMM_OK)
        memcpy(dst, buf, (size_t)len);
    return rc;
}

static int local_atomic_cas(void *ctx, gpa_t gpa,
                          uint64_t expected, uint64_t desired, uint64_t *old)
{
    LocalTransportCtx *local = (LocalTransportCtx *)ctx;
    void              *ptr;
    int                rc;

    pthread_mutex_lock(&local->lock);
    rc = local_resolve(local, gpa, sizeof(uint64_t), &ptr);
    if (rc == UMM_OK) {
        uint64_t prev = __sync_val_compare_and_swap_8((uint64_t *)ptr,
                                                       expected, desired);
        if (old)
            *old = prev;
    }
    pthread_mutex_unlock(&local->lock);
    return rc;
}

static int local_atomic_fetch_add(void *ctx, gpa_t gpa,
                                uint64_t value, uint64_t *result)
{
    LocalTransportCtx *local = (LocalTransportCtx *)ctx;
    void              *ptr;
    int                rc;

    pthread_mutex_lock(&local->lock);
    rc = local_resolve(local, gpa, sizeof(uint64_t), &ptr);
    if (rc == UMM_OK) {
        uint64_t prev = __sync_fetch_and_add_8((uint64_t *)ptr, value);
        if (result)
            *result = prev;
    }
    pthread_mutex_unlock(&local->lock);
    return rc;
}

static int local_atomic_set(void *ctx, gpa_t gpa, uint64_t value)
{
    LocalTransportCtx *local = (LocalTransportCtx *)ctx;
    void              *ptr;
    int                rc;

    pthread_mutex_lock(&local->lock);
    rc = local_resolve(local, gpa, sizeof(uint64_t), &ptr);
    if (rc == UMM_OK) {
        __sync_lock_test_and_set((uint64_t *)ptr, value);
        __sync_synchronize();
    }
    pthread_mutex_unlock(&local->lock);
    return rc;
}

static void local_fence(void *ctx)
{
    (void)ctx;
    __sync_synchronize();
}

static void local_barrier_all(void *ctx)
{
    (void)ctx;
    /* In a single-device local setup this is a full memory barrier.
     * Multi-device coordination would require additional protocol support. */
    __sync_synchronize();
}

static void local_quiet(void *ctx)
{
    (void)ctx;
    /* Local load/store is synchronous; a fence is sufficient to drain. */
    __sync_synchronize();
}

static int local_register_node(void *ctx, node_id_t node,
                             uint64_t base, uint64_t size,
                             const char *device)
{
    LocalTransportCtx *local = (LocalTransportCtx *)ctx;

    if (node != local->local_node) {
        local_warn("local_register_node: ignoring remote node %u (local=%u)",
                 (unsigned)node, (unsigned)local->local_node);
        return UMM_OK;
    }

    StorageResource res = {
        .tier        = UMM_TIER_CXL,
        .capacity    = size,
        .base_offset = base,
        .online      = 1,
    };
    strncpy(res.device_path, device ? device : "local",
            sizeof(res.device_path) - 1);

    int rc = local->mem_vtbl->register_storage(local->mem_ctx, &res);

    local_info("local: registered node %u, base_gpa=0x%lx, size=%lu bytes (rc=%d)",
             (unsigned)node, (unsigned long)base, (unsigned long)size, rc);
    return rc;
}

static int local_init(void *ctx, const UMMConfig *cfg)
{
    LocalTransportCtx *local = (LocalTransportCtx *)ctx;

    local->local_node = cfg->my_node_id;

    if (pthread_mutex_init(&local->lock, NULL) != 0) {
        local_error("local_init: pthread_mutex_init failed");
        return UMM_E_TRANSPORT_ERROR;
    }

    /* ---- Direct mode: create local mem_service_direct instance ---- */
    uint64_t mem_size = cfg->memory_size ? cfg->memory_size
                                         : 256ULL * 1024 * 1024; /* 256 MiB */
    void *mem_ctx = NULL;
    MemoryServiceVtbl *mem_vtbl;
    if (cfg->local_mem_as_dram) {
        /* Phase 2 混合池（无 CXL 硬件）：本地内存数据面注册为 DRAM tier。
         * 用 create_v2 避免自动注册 CXL mock，只保留 DRAM 一层。 */
        mem_vtbl = mem_service_direct_create_v2(cfg->my_node_id, &mem_ctx);
    } else {
        mem_vtbl = mem_service_direct_create(cfg->my_node_id,
                                              mem_size,
                                              0, &mem_ctx);
    }
    if (!mem_vtbl || !mem_ctx) {
        local_error("local_init: mem_service_direct_create failed");
        pthread_mutex_destroy(&local->lock);
        return UMM_E_NO_MEMORY;
    }

    local->mem_vtbl = mem_vtbl;
    local->mem_ctx  = mem_ctx;

    /* Register local storage */
    StorageResource res = {
        .tier        = cfg->local_mem_as_dram ? UMM_TIER_DRAM
                                               : UMM_TIER_CXL,
        .capacity    = mem_size,
        .base_offset = 0,
        .online      = 1,
    };
    if (cfg->local_mem_as_dram) {
        /* DRAM 分支：device_path 默认为空（注册即 malloc 私有后备）。
         * 复用 cxl_device 字段作为"内存层后备设备"（避免 ABI 变更）：
         * 非空时 DRAM tier 以该设备为后备（Phase 2.5 共享内存窗口，
         * 如 virtio-pmem /dev/pmem0，open+mmap(MAP_SHARED)，失败即注册
         * 失败、不回退 malloc——否则"共享"会无声退化成"私有"）。 */
        if (cfg->cxl_device[0]) {
            snprintf(res.device_path, sizeof(res.device_path), "%s",
                     cfg->cxl_device);
        } else {
            res.device_path[0] = '\0';
        }
    } else {
        const char *dev_path = cfg->cxl_device[0] ? cfg->cxl_device
                                                   : "/dev/cxl/mem0";
        snprintf(res.device_path, sizeof(res.device_path), "%s", dev_path);
    }
    mem_vtbl->register_storage(mem_ctx, &res);

    local_info("local transport: direct mode, node=%u, mem=%lu bytes, tier=%s",
             (unsigned)local->local_node, (unsigned long)mem_size,
             cfg->local_mem_as_dram ? "DRAM" : "CXL");
    return UMM_OK;
}

static void local_deinit(void *ctx)
{
    LocalTransportCtx *local = (LocalTransportCtx *)ctx;
    if (!local)
        return;

    pthread_mutex_lock(&local->lock);

    /* Destroy local mem_service instance (direct mode) */
    if (local->mem_ctx)
        mem_service_direct_destroy(local->mem_ctx);
    local->mem_ctx  = NULL;
    local->mem_vtbl = NULL;

    pthread_mutex_unlock(&local->lock);
    pthread_mutex_destroy(&local->lock);

    memset(local, 0, sizeof(*local));
}

/* ------------------------------------------------------------------------ */
/* VTable singleton                                                         */
/* ------------------------------------------------------------------------ */

static const MemoryTransportVtbl g_local_vtbl = {
    .get              = local_get,
    .put              = local_put,
    .atomic_cas       = local_atomic_cas,
    .atomic_fetch_add = local_atomic_fetch_add,
    .atomic_set       = local_atomic_set,
    .fence            = local_fence,
    .barrier_all      = local_barrier_all,
    .quiet            = local_quiet,
    .register_node    = local_register_node,
    .init             = local_init,
    .deinit           = local_deinit,
};

const MemoryTransportVtbl *umm_local_vtbl_get(void)
{
    return &g_local_vtbl;
}
