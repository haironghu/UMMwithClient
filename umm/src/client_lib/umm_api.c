/* ========================================================================
 * umm_api.c  --  Public API implementation (UMM v2.0 client library)
 *
 * v2.0 Architecture: ALL memory is CXL shared memory. No DRAM. No Region.
 *
 * Two sub-modes within the unified path:
 *   - Direct (embedded) mode: local mem_service_direct + meta_service_direct
 *   - RPC mode: RPC clients connect to remote ummd/umms servers
 *
 * The unified allocation path for ALL chunks:
 *   1. alloc_local(size)  -> offset
 *   2. make_gpa(node_id, offset) -> gpa
 *   3. register_chunk(name, gpa, size) -> chunk_id
 *   4. Fill ChunkDescriptor
 *
 * Phase 5: Topology-aware initialization
 *   - When cfg->transport is empty: query topology from ummd/umms, then
 *     configure transports (CXL + optional SSD) via tier_router.
 *   - When cfg->transport is set: legacy single-transport mode.
 * ======================================================================== */

/*
 * NOTE: We do NOT include the public umm.h here.  This internal source file
 * uses the internal headers (../common/types.h, etc.) which define the same
 * types.  Including both would cause redefinition errors.
 */
#include "umm_transport.h"
#include "umm_router.h"
#include "umm_descriptor.h"

#include "../common/types.h"
#include "../common/log.h"
#include "../common/error_codes.h"

#include "../memory_service/mem_service.h"
#include "../memory_service/mem_service_direct.h"
#include "../metadata_service/meta_service.h"
#include "../metadata_service/meta_service_direct.h"

/* Phase 5: tier_router and ssd_transport for topology-aware init */
#include "../transport/tier_router.h"
#include "../transport/transport_ssd.h"

/* Phase 1: 跨节点远程数据面 */
#include "../transport/transport_remote.h"
#include "../cis/cis_router.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#ifndef UMM_PAGE_SIZE
#define UMM_PAGE_SIZE 4096ULL
#endif

/* ------------------------------------------------------------------------ */
/* RPC client types (opaque -- defined in their respective .c files)        */
/* We can only use pointers to these opaque types.                          */
/* ------------------------------------------------------------------------ */

typedef struct MetaRpcClient MetaRpcClient;
typedef struct MemRpcClient  MemRpcClient;

/* RPC client lifecycle */
extern int  meta_rpc_client_init(MetaRpcClient *client, const char *host, int port);
extern void meta_rpc_client_deinit(MetaRpcClient *client);

extern int  mem_rpc_client_init(MemRpcClient *client, const char *host, int port);
extern int  mem_rpc_client_init_ex(MemRpcClient *client, const char *host,
                                   int port, const char *rpc_token);
extern void mem_rpc_client_deinit(MemRpcClient *client);

/* Metadata RPC operations (v2.0 signatures -- no region_id, no flags) */
extern int meta_rpc_register_chunk(MetaRpcClient *c, const char *name, gpa_t gpa,
                                   uint64_t size, chunk_id_t *out);
extern int meta_rpc_lookup_chunk(MetaRpcClient *c, const char *name,
                                 ChunkMetadata *out);
extern int meta_rpc_lookup_chunk_by_id(MetaRpcClient *c, chunk_id_t chunk_id,
                                       ChunkMetadata *out);
extern int meta_rpc_unregister_chunk(MetaRpcClient *c, chunk_id_t chunk_id);
extern int meta_rpc_add_ref(MetaRpcClient *c, chunk_id_t chunk_id);
extern int meta_rpc_release_ref(MetaRpcClient *c, chunk_id_t chunk_id);

/* Phase 5: topology query RPC */
extern int meta_rpc_get_storage_topology(MetaRpcClient *c, node_id_t node,
                                          StorageTopology *out);

/* Memory RPC operations */
extern int mem_rpc_alloc(MemRpcClient *c, uint64_t size, uint32_t flags,
                         uint64_t *out_offset);
extern int mem_rpc_free(MemRpcClient *c, uint64_t offset, uint64_t size);

/* Phase 5: mem service topology query RPC */
extern int mem_rpc_get_topology(MemRpcClient *c, StorageTopology *out);
extern int meta_rpc_register_storage_resource(MetaRpcClient *c, node_id_t node,
                                               const StorageResource *res);

/* Tier-aware allocation RPC */
extern int mem_rpc_alloc_tiered(MemRpcClient *c, tier_id_t tier, uint64_t size,
                                uint32_t flags, uint64_t *out_offset);
extern int mem_rpc_alloc_tiered2(MemRpcClient *c, tier_id_t tier,
                                 uint64_t size, uint32_t flags,
                                 uint64_t *out_offset, uint8_t *out_owner);
extern int mem_rpc_alloc_on_device(MemRpcClient *c, tier_id_t tier,
                                    uint32_t device_idx, uint64_t size,
                                    uint32_t flags, uint64_t *out_offset,
                                    uint8_t *out_owner);
extern int mem_rpc_free_tiered(MemRpcClient *c, tier_id_t tier, uint64_t offset,
                               uint64_t size);

/* ------------------------------------------------------------------------ */
/* Logging helpers                                                          */
/* ------------------------------------------------------------------------ */

static void log_debug(const char *file, int line, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    umm_log(UMM_LOG_DEBUG, file, line, fmt, ap);
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

static void log_error(const char *file, int line, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    umm_log(UMM_LOG_ERROR, file, line, fmt, ap);
    va_end(ap);
}

/* Forward declaration of public API for use in umm_init error path */
extern void umm_deinit(void);

/* ------------------------------------------------------------------------ */
/* Global state                                                              */
/* ------------------------------------------------------------------------ */

typedef struct {
    int                initialized;
    UMMConfig          config;
    MemoryTransportVtbl *transport;
    void               *transport_ctx;

    /* RPC mode: opaque client backing buffers */
    void               *mem_client_buf;     /* sizeof(MemRpcClient)  bytes */
    void               *meta_client_buf;    /* sizeof(MetaRpcClient) bytes */

    /* Direct (embedded) mode: local service pointers */
    void               *local_mem_ctx;
    MemoryServiceVtbl  *local_mem_vtbl;
    void               *local_meta_ctx;
    MetadataServiceVtbl *local_meta_vtbl;

    /* Phase 5: tier-aware transport state */
    TierRouter         *tier_router;        /* NULL in legacy single-transport mode */
    void               *ssd_transport_ctx;  /* SSD context for cleanup */
    MemoryTransportVtbl *ssd_transport_vtbl; /* SSD vtable for cleanup */

    /* Phase 1: 跨节点远程数据面 */
    void               *remote_transport_ctx;
    MemoryTransportVtbl *remote_transport_vtbl;
    node_id_t           mem_server_node;    /* 学习到的 umms node（0xFF=未知） */
} UMMGlobalState;

static UMMGlobalState g_state = {0};

#define RPC_CLIENT_BUF_SIZE 512

/* ------------------------------------------------------------------------ */
/* Internal helpers                                                          */
/* ------------------------------------------------------------------------ */

static int is_direct_mode(void)
{
    return (g_state.config.meta_server_addr[0] == '\0' &&
            g_state.config.mem_server_addr[0] == '\0');
}

static int is_rpc_mode(void)
{
    return (g_state.config.meta_server_addr[0] != '\0' ||
            g_state.config.mem_server_addr[0] != '\0');
}

static int parse_host_port(const char *addr_str, char *out_host, size_t host_len,
                           int *out_port)
{
    if (!addr_str || !out_host || !out_port)
        return UMM_E_INVALID_ARG;

    out_host[0] = '\0';
    *out_port = 0;

    const char *colon = strrchr(addr_str, ':');
    if (!colon)
        return UMM_E_INVALID_ARG;

    size_t hlen = (size_t)(colon - addr_str);
    if (hlen >= host_len)
        hlen = host_len - 1;

    memcpy(out_host, addr_str, hlen);
    out_host[hlen] = '\0';
    *out_port = atoi(colon + 1);

    if (*out_port <= 0 || *out_port > 65535)
        return UMM_E_INVALID_ARG;

    return UMM_OK;
}

/* ------------------------------------------------------------------------ */
/* Phase 5: Topology-aware transport configuration                           */
/* ------------------------------------------------------------------------ */

/**
 * Check if we are in the new tier-aware mode (cfg->transport is empty).
 */
static int is_tier_aware_mode(const UMMConfig *cfg)
{
    return (cfg->transport[0] == '\0');
}

/**
 * Configure transports from topology information.
 *
 * Creates CXL transport (real or mock) and optional SSD transport,
 * then wraps them in a tier_router.
 */
static int configure_transports_from_topology(const UMMConfig *cfg,
                                               const StorageTopology *topo)
{
    int rc;

    /* Phase 1：本函数可能被多次调用（init / topology re-config /
     * register_storage_tier re-config），先自清旧 remote transport */
    if (g_state.remote_transport_ctx) {
        remote_transport_destroy(g_state.remote_transport_ctx);
        g_state.remote_transport_ctx  = NULL;
        g_state.remote_transport_vtbl = NULL;
    }

    /* --- Create local transport (CXL + mock unified) --- */
    MemoryTransportVtbl *cxl_vtbl = NULL;
    void *cxl_ctx = NULL;

    extern const MemoryTransportVtbl *umm_local_vtbl_get(void);
    cxl_vtbl = (MemoryTransportVtbl *)umm_local_vtbl_get();
    if (cxl_vtbl) {
        cxl_ctx = calloc(1, 4096);
        if (cxl_ctx) {
            rc = cxl_vtbl->init(cxl_ctx, cfg);
            if (rc != UMM_OK) {
                free(cxl_ctx);
                cxl_ctx = NULL;
                cxl_vtbl = NULL;
            }
        } else {
            cxl_vtbl = NULL;
        }
    }

    if (!cxl_vtbl) {
        log_error(__FILE__, __LINE__,
                  "topology: failed to create local transport");
        return UMM_E_UNKNOWN;
    }

    log_info(__FILE__, __LINE__,
             "topology: local transport created (backend=local)");

    /* --- Create SSD transport if topology indicates SSD tier --- */
    MemoryTransportVtbl *ssd_vtbl = NULL;
    void *ssd_ctx = NULL;
    int  ssd_local_skip_nds = 0;
    char ssd_local_skip_path[256] = {0};

    SsdDeviceConfig topo_devices[16] = {0};
    uint32_t topo_device_count = 0;
    if (topo && cfg->num_ssd_devices == 0) {
        for (uint32_t i = 0; i < UMM_MAX_TOPOLOGY_RESOURCES; i++) {
            const StorageResource *res = &topo->resources[i];
            if (!res->online || res->tier != UMM_TIER_SSD) continue;
            if (strncmp(res->device_path, "nds:", 4) == 0) {
                ssd_local_skip_nds = 1;
                snprintf(ssd_local_skip_path, sizeof(ssd_local_skip_path), "%s",
                         res->device_path);
                break;
            }
            if (topo_device_count >= 16) return UMM_E_INVALID_ARG;
            snprintf(topo_devices[topo_device_count].path,
                     sizeof(topo_devices[topo_device_count].path), "%s", res->device_path);
            topo_devices[topo_device_count++].size = res->capacity;
        }
    }
    if (topo_device_count > 1 && !ssd_local_skip_nds) {
        ssd_vtbl = ssd_transport_create_multi(topo_devices, topo_device_count, &ssd_ctx);
        if (!ssd_vtbl) {
            if (cxl_vtbl && cxl_vtbl->deinit) cxl_vtbl->deinit(cxl_ctx);
            free(cxl_ctx);
            return UMM_E_TRANSPORT_ERROR;
        }
    }

    if (topo && topo->num_resources > 0 && !ssd_vtbl && !ssd_local_skip_nds) {
        /* 遍历全部槽位而非 num_resources——兼容两种拓扑布局:
         * mem_service_direct 返回紧凑数组, ummD 按 tier 稀疏存放 */
        for (uint32_t i = 0; i < UMM_NUM_TIERS; i++) {
            const StorageResource *res = &topo->resources[i];
            if (!res->online)
                continue;

            if (res->tier == UMM_TIER_SSD) {
                /* cfg.ssd_devices 显式给出本机多设备数据面列表时，
                 * 跳过拓扑单设备路径，统一由循环后的 multi 版本创建 */
                if (cfg->num_ssd_devices > 0)
                    break;
                /* nds: 直驱设备不在客户端建本地数据面——单次 nds_init
                 * 纪律：全进程仅 worker 直连池 API 持有设备；客户端
                 * write_chunk 的 host buffer 语义对 NDS 本就错误。
                 * RPC 分配面（create_chunk）与拓扑可见性不受影响。
                 * 注意 "nds:" 不会误匹配 "nds-meta:"（第 4 字符为 '-'） */
                if (strncmp(res->device_path, "nds:", 4) == 0) {
                    log_info(__FILE__, __LINE__,
                             "客户端本地数据面跳过 NDS 直驱设备 %s：数据面"
                             "由 worker 直连池 API 持有，分配走 RPC",
                             res->device_path);
                    ssd_local_skip_nds = 1;
                    strncpy(ssd_local_skip_path, res->device_path,
                            sizeof(ssd_local_skip_path) - 1);
                    break;
                }
                ssd_vtbl = ssd_transport_create(res->device_path,
                                                 res->capacity,
                                                 &ssd_ctx);
                if (ssd_vtbl) {
                    log_info(__FILE__, __LINE__,
                             "topology: SSD transport created (path=%s, "
                             "capacity=%lu)",
                             res->device_path,
                             (unsigned long)res->capacity);
                } else {
                    log_warn(__FILE__, __LINE__,
                             "topology: failed to create SSD transport for %s",
                             res->device_path);
                }
                break;  /* Only one SSD transport supported */
            }
        }
    }

    /* 多设备共享池（如 QEMU 共享块设备实验）：cfg.ssd_devices 显式
     * 给出本机数据面设备列表时优先于拓扑单设备资源——pool 虚拟
     * 偏移按数组顺序拼接（与 umms 侧 ssd_devices 顺序必须一致）。
     * 任一设备打不开即失败暴露（configure 返回错误），不留空池。 */
    if (!ssd_vtbl && !ssd_local_skip_nds && cfg->num_ssd_devices > 0) {
        ssd_vtbl = ssd_transport_create_multi(
            cfg->ssd_devices, cfg->num_ssd_devices, &ssd_ctx);
        if (ssd_vtbl) {
            log_info(__FILE__, __LINE__,
                     "topology: multi-SSD transport created (%u device(s))",
                     cfg->num_ssd_devices);
        } else {
            log_error(__FILE__, __LINE__,
                      "topology: multi-SSD transport create FAILED "
                      "(%u device(s)) — check device paths/capacities",
                      cfg->num_ssd_devices);
            if (cxl_vtbl && cxl_vtbl->deinit)
                cxl_vtbl->deinit(cxl_ctx);
            free(cxl_ctx);
            return UMM_E_TRANSPORT_ERROR;
        }
    }

    /* --- Create tier router wrapping all transports --- */
    TierRouter *router = tier_router_create(cxl_vtbl, cxl_ctx,
                                             ssd_vtbl, ssd_ctx);
    if (!router) {
        log_error(__FILE__, __LINE__,
                  "topology: tier_router_create failed");
        /* Cleanup CXL */
        if (cxl_vtbl && cxl_vtbl->deinit)
            cxl_vtbl->deinit(cxl_ctx);
        free(cxl_ctx);
        /* Cleanup SSD */
        if (ssd_vtbl && ssd_ctx)
            ssd_transport_destroy(ssd_ctx);
        return UMM_E_UNKNOWN;
    }

    /* Phase 2 混合池：local_mem_as_dram 时本地 transport 的 mem_service
     * 只注册了 DRAM tier（transport_local 已按配置选择），把它同时挂进
     * router 的 DRAM 槽位，umm_alloc_tiered(size, UMM_TIER_DRAM) 的 GPA
     * 数据面即可路由到本地 malloc 后备。
     * CXL 槽位保留同一 transport：tier=1 的 GPA 会在 resolve 时因 CXL
     * 未注册而干净报错（本模式无 CXL，属预期）。 */
    if (cfg->local_mem_as_dram)
        tier_router_set_dram(router, cxl_vtbl, cxl_ctx);

    /* nds: 设备跳过本地数据面后，对该 tier 的 get/put 返回明确的
     * UMM_E_UNSUPPORTED（而非笼统 INVALID_ARG） */
    if (ssd_local_skip_nds)
        tier_router_mark_local_unsupported(router, UMM_TIER_SSD,
                                           ssd_local_skip_path);

    /* --- Phase 1: RPC 模式下挂载远端数据面 transport ---
     * 挂载后 owner != my_node 的 GPA 走 DATA_READ/WRITE RPC 到属主
     * 节点；owner == my_node 维持本地 tier 分派（单节点行为不变）。
     *
     * 显式 opt-in：仅当配置 peer_nodes 或 ssd_owner_node 时挂载。
     * 零配置的 RPC 部署 = 共享盘模型（各节点对本机盘做 I/O，如
     * QEMU/ SAN 共享块设备），挂载 remote 会把本机 I/O 错误地
     * 搬上网络——共享盘与 shared-nothing 必须由配置区分。 */
    if (cfg->mem_server_addr[0] != '\0' &&
        (cfg->peer_nodes[0] != '\0' ||
         cfg->ssd_owner_node != UMM_NODE_UNKNOWN)) {
        void *rctx = NULL;
        MemoryTransportVtbl *rvtbl = remote_transport_create(
            cfg->my_node_id, cfg->data_max_io,
            cfg->rpc_token[0] ? cfg->rpc_token : NULL, &rctx);
        if (rvtbl) {
            /* umms 回退地址：alloc 学习值 > ssd_owner_node 配置 */
            node_id_t fb = (g_state.mem_server_node != UMM_NODE_UNKNOWN)
                         ? g_state.mem_server_node
                         : cfg->ssd_owner_node;
            if (fb != UMM_NODE_UNKNOWN)
                remote_transport_set_fallback(rctx, fb,
                                              cfg->mem_server_addr);
            tier_router_set_remote(router, cfg->my_node_id, rvtbl, rctx);
            g_state.remote_transport_vtbl = rvtbl;
            g_state.remote_transport_ctx  = rctx;
        } else {
            /* 非致命：仅丧失远端数据面，本地路径不受影响 */
            log_warn(__FILE__, __LINE__,
                     "topology: remote transport create failed, "
                     "cross-node data plane disabled");
        }
    }

    g_state.tier_router     = router;
    g_state.ssd_transport_vtbl = ssd_vtbl;
    g_state.ssd_transport_ctx  = ssd_ctx;
    g_state.transport       = tier_router_get_vtbl(router);
    g_state.transport_ctx   = router;

    log_info(__FILE__, __LINE__,
             "topology: tier_router configured with %d resource(s)",
             topo ? (int)topo->num_resources : 0);
    return UMM_OK;
}

/**
 * Legacy transport initialization (single backend from cfg->transport).
 */
static int init_transport_legacy(const UMMConfig *cfg)
{
    int rc = umm_transport_lib_init(cfg, &g_state.transport, &g_state.transport_ctx);
    if (rc != UMM_OK) {
        log_error(__FILE__, __LINE__,
                  "umm_init: legacy transport init failed (rc=%d)", rc);
        return rc;
    }
    g_state.tier_router = NULL;
    g_state.ssd_transport_vtbl = NULL;
    g_state.ssd_transport_ctx = NULL;
    return UMM_OK;
}

/* ------------------------------------------------------------------------ */
/* umm_init                                                                  */
/* ------------------------------------------------------------------------ */

int umm_init(const UMMConfig *cfg)
{
    if (!cfg)
        return UMM_E_INVALID_ARG;

    if (g_state.initialized) {
        /* Auto-cleanup previous session to allow re-init with different node */
        umm_deinit();
    }

    /* In legacy mode, transport must be specified.
     * In tier-aware mode, transport is empty. */
    if (!is_tier_aware_mode(cfg) && cfg->transport[0] == '\0') {
        log_error(__FILE__, __LINE__, "umm_init: transport not specified");
        return UMM_E_INVALID_ARG;
    }

    /* Copy configuration */
    memcpy(&g_state.config, cfg, sizeof(UMMConfig));

    /* Phase 1：mem_server_node 初值 = ssd_owner_node 配置（0xFF = 未知，
     * 待首次 alloc 响应学习）；CIS 静态节点表初始化（此前为死代码）。
     * 失败非致命——远端地址解析还有 umms 回退。 */
    g_state.mem_server_node = cfg->ssd_owner_node;
    if (cis_router_init(cfg) != UMM_OK) {
        log_warn(__FILE__, __LINE__,
                 "umm_init: cis_router_init failed, peer_nodes ignored");
    }

    int rc;
    StorageTopology topo;
    int have_topology = 0;

    /* -- Direct mode: create local memory and metadata services -- */
    if (is_direct_mode()) {
        uint64_t local_mem_size = cfg->memory_size;
        if (local_mem_size == 0)
            local_mem_size = 64ULL * 1024 * 1024;  /* default 64 MiB */

        /* Local memory service */
        g_state.local_mem_vtbl = mem_service_direct_create(
            cfg->my_node_id, local_mem_size, 0, &g_state.local_mem_ctx);
        if (!g_state.local_mem_vtbl) {
            log_error(__FILE__, __LINE__,
                      "umm_init: failed to create local memory service");
            memset(&g_state, 0, sizeof(g_state));
            return UMM_E_NO_MEMORY;
        }

        /* Local metadata service */
        g_state.local_meta_vtbl = meta_service_direct_create(&g_state.local_meta_ctx);
        if (!g_state.local_meta_vtbl) {
            log_error(__FILE__, __LINE__,
                      "umm_init: failed to create local metadata service");
            mem_service_direct_destroy(g_state.local_mem_ctx);
            memset(&g_state, 0, sizeof(g_state));
            return UMM_E_NO_MEMORY;
        }

        log_info(__FILE__, __LINE__,
                 "direct mode: local services created (mem=%lu bytes)",
                 (unsigned long)local_mem_size);

        /* === Phase 5: Query topology from local mem_service === */
        if (is_tier_aware_mode(cfg) &&
            g_state.local_mem_vtbl &&
            g_state.local_mem_vtbl->get_topology) {
            memset(&topo, 0, sizeof(topo));
            rc = g_state.local_mem_vtbl->get_topology(g_state.local_mem_ctx, &topo);
            if (rc == UMM_OK && topo.num_resources > 0) {
                have_topology = 1;
                log_info(__FILE__, __LINE__,
                         "direct mode: got topology with %u resource(s)",
                         topo.num_resources);
            } else {
                log_warn(__FILE__, __LINE__,
                         "direct mode: get_topology failed (rc=%d) or empty, "
                         "using defaults", rc);
            }
        }
    }

    /* -- Initialize transport layer -- */
    if (is_tier_aware_mode(cfg)) {
        /* Phase 5: tier-aware transport initialization */
        if (have_topology) {
            rc = configure_transports_from_topology(cfg, &topo);
        } else {
            /* Fall back: create tier router with CXL/mock only (no SSD) */
            log_info(__FILE__, __LINE__,
                     "tier-aware mode: no topology, creating CXL-only transport");
            rc = configure_transports_from_topology(cfg, NULL);
        }
        if (rc != UMM_OK) {
            log_error(__FILE__, __LINE__,
                      "umm_init: tier-aware transport init failed (rc=%d)", rc);
            if (g_state.local_mem_vtbl) {
                mem_service_direct_destroy(g_state.local_mem_ctx);
                g_state.local_mem_vtbl = NULL;
                g_state.local_mem_ctx  = NULL;
            }
            if (g_state.local_meta_vtbl) {
                meta_service_direct_destroy(g_state.local_meta_ctx);
                g_state.local_meta_vtbl = NULL;
                g_state.local_meta_ctx  = NULL;
            }
            memset(&g_state, 0, sizeof(g_state));
            return rc;
        }
    } else {
        /* Legacy: single transport backend */
        rc = init_transport_legacy(cfg);
        if (rc != UMM_OK) {
            if (g_state.local_mem_vtbl) {
                mem_service_direct_destroy(g_state.local_mem_ctx);
                g_state.local_mem_vtbl = NULL;
                g_state.local_mem_ctx  = NULL;
            }
            if (g_state.local_meta_vtbl) {
                meta_service_direct_destroy(g_state.local_meta_ctx);
                g_state.local_meta_vtbl = NULL;
                g_state.local_meta_ctx  = NULL;
            }
            memset(&g_state, 0, sizeof(g_state));
            return rc;
        }
    }

    /* -- RPC mode: initialize RPC clients -- */
    if (is_rpc_mode()) {
        /* Metadata service RPC client (needed for topology query in tier-aware mode) */
        if (cfg->meta_server_addr[0] != '\0') {
            char meta_host[256];
            int  meta_port = 0;
            rc = parse_host_port(cfg->meta_server_addr, meta_host,
                                 sizeof(meta_host), &meta_port);
            if (rc != UMM_OK) {
                log_error(__FILE__, __LINE__,
                          "umm_init: invalid meta_server_addr '%s'",
                          cfg->meta_server_addr);
                goto fail;
            }
            g_state.meta_client_buf = calloc(1, RPC_CLIENT_BUF_SIZE);
            if (!g_state.meta_client_buf) {
                rc = UMM_E_NO_MEMORY;
                goto fail;
            }
            rc = meta_rpc_client_init(g_state.meta_client_buf, meta_host,
                                      meta_port);
            if (rc != UMM_OK) {
                log_error(__FILE__, __LINE__,
                          "umm_init: meta_rpc_client_init failed (rc=%d)", rc);
                free(g_state.meta_client_buf);
                g_state.meta_client_buf = NULL;
                goto fail;
            }
            log_info(__FILE__, __LINE__, "rpc mode: meta RPC client -> %s:%d",
                     meta_host, meta_port);

            /* === Phase 5: Query topology from ummd (RPC mode, tier-aware) === */
            if (is_tier_aware_mode(cfg)) {
                memset(&topo, 0, sizeof(topo));
                rc = meta_rpc_get_storage_topology(g_state.meta_client_buf,
                                                    cfg->my_node_id, &topo);
                if (rc == UMM_OK && topo.num_resources > 0) {
                    have_topology = 1;
                    log_info(__FILE__, __LINE__,
                             "rpc mode: got topology from ummd with %u resource(s)",
                             topo.num_resources);

                    /* Re-configure transports with the topology from ummd */
                    /* First clean up the fallback CXL-only transport */
                    if (g_state.tier_router) {
                        tier_router_destroy(g_state.tier_router);
                        g_state.tier_router = NULL;
                        g_state.transport = NULL;
                        g_state.transport_ctx = NULL;
                    }
                    if (g_state.ssd_transport_vtbl && g_state.ssd_transport_ctx) {
                        ssd_transport_destroy(g_state.ssd_transport_ctx);
                        g_state.ssd_transport_vtbl = NULL;
                        g_state.ssd_transport_ctx = NULL;
                    }

                    rc = configure_transports_from_topology(cfg, &topo);
                    if (rc != UMM_OK) {
                        log_error(__FILE__, __LINE__,
                                  "umm_init: transport re-config from topology "
                                  "failed (rc=%d)", rc);
                        goto fail;
                    }
                } else {
                    log_warn(__FILE__, __LINE__,
                             "rpc mode: get_storage_topology failed (rc=%d), "
                             "using fallback CXL-only transport", rc);
                }
            }
        }

        /* Memory service RPC client */
        if (cfg->mem_server_addr[0] != '\0') {
            char mem_host[256];
            int  mem_port = 0;
            rc = parse_host_port(cfg->mem_server_addr, mem_host,
                                 sizeof(mem_host), &mem_port);
            if (rc != UMM_OK) {
                log_error(__FILE__, __LINE__,
                          "umm_init: invalid mem_server_addr '%s'",
                          cfg->mem_server_addr);
                goto fail;
            }
            g_state.mem_client_buf = calloc(1, RPC_CLIENT_BUF_SIZE);
            if (!g_state.mem_client_buf) {
                rc = UMM_E_NO_MEMORY;
                goto fail;
            }
            rc = mem_rpc_client_init_ex(g_state.mem_client_buf, mem_host,
                                        mem_port,
                                        cfg->rpc_token[0]
                                            ? cfg->rpc_token : NULL);
            if (rc != UMM_OK) {
                log_error(__FILE__, __LINE__,
                          "umm_init: mem_rpc_client_init failed (rc=%d)", rc);
                free(g_state.mem_client_buf);
                g_state.mem_client_buf = NULL;
                goto fail;
            }
            log_info(__FILE__, __LINE__, "rpc mode: mem RPC client -> %s:%d",
                     mem_host, mem_port);
        }
    }

    g_state.initialized = 1;
    log_info(__FILE__, __LINE__,
             "umm_init: initialized successfully (mode=%s, transport=%s, node=%u)",
             is_tier_aware_mode(cfg) ? "tier-aware" : "legacy",
             is_tier_aware_mode(cfg) ? "tier_router" : cfg->transport,
             (unsigned)cfg->my_node_id);
    return UMM_OK;

fail:
    umm_deinit();
    return rc;
}

/* ------------------------------------------------------------------------ */
/* umm_deinit                                                                */
/* ------------------------------------------------------------------------ */

void umm_deinit(void)
{
    if (!g_state.initialized)
        return;

    log_info(__FILE__, __LINE__, "umm_deinit: shutting down");

    /* -- Deinit RPC clients (reverse order of init) -- */
    if (is_rpc_mode()) {
        if (g_state.meta_client_buf) {
            meta_rpc_client_deinit(g_state.meta_client_buf);
            free(g_state.meta_client_buf);
            g_state.meta_client_buf = NULL;
        }
        if (g_state.mem_client_buf) {
            mem_rpc_client_deinit(g_state.mem_client_buf);
            free(g_state.mem_client_buf);
            g_state.mem_client_buf = NULL;
        }
    }

    /* -- Deinit transport --
     *
     * In tier-aware mode, the tier_router owns the underlying transports.
     * Destroying the router also destroys CXL and SSD contexts.
     * In legacy mode, transport_lib_deinit handles cleanup.
     */
    if (g_state.tier_router) {
        tier_router_destroy(g_state.tier_router);
        g_state.tier_router = NULL;
        g_state.transport = NULL;
        g_state.transport_ctx = NULL;
        g_state.ssd_transport_vtbl = NULL;
        g_state.ssd_transport_ctx = NULL;
    } else if (g_state.transport) {
        umm_transport_lib_deinit(g_state.transport, g_state.transport_ctx);
        g_state.transport     = NULL;
        g_state.transport_ctx = NULL;
    }

    /* -- Destroy local metadata service -- */
    if (g_state.local_meta_vtbl) {
        meta_service_direct_destroy(g_state.local_meta_ctx);
        g_state.local_meta_vtbl = NULL;
        g_state.local_meta_ctx  = NULL;
    }

    /* -- Destroy local memory service -- */
    if (g_state.local_mem_vtbl) {
        mem_service_direct_destroy(g_state.local_mem_ctx);
        g_state.local_mem_vtbl = NULL;
        g_state.local_mem_ctx  = NULL;
    }

    /* -- Phase 1: remote transport & CIS router -- */
    if (g_state.remote_transport_ctx) {
        remote_transport_destroy(g_state.remote_transport_ctx);
        g_state.remote_transport_ctx  = NULL;
        g_state.remote_transport_vtbl = NULL;
    }
    cis_router_deinit();

    memset(&g_state, 0, sizeof(g_state));
}

/* ------------------------------------------------------------------------ */
/* umm_alloc  --  UNIFIED ALLOCATION PATH (v2.0)                            */
/* ------------------------------------------------------------------------ */

int umm_alloc(uint64_t size, ChunkDescriptor *out)
{
    if (!g_state.initialized)
        return UMM_E_NOT_INITIALIZED;

    if (size == 0 || !out)
        return UMM_E_INVALID_ARG;

    memset(out, 0, sizeof(ChunkDescriptor));

    uint64_t alloc_size = (size + UMM_PAGE_SIZE - 1) & ~(UMM_PAGE_SIZE - 1);
    uint64_t offset = 0;
    int rc;

    /* Step 1: Allocate physical memory from local umms */
    if (g_state.local_mem_vtbl) {
        /* Direct mode */
        rc = g_state.local_mem_vtbl->alloc_local(g_state.local_mem_ctx,
                                                  alloc_size, &offset);
    } else if (g_state.mem_client_buf) {
        /* RPC mode */
        rc = mem_rpc_alloc(g_state.mem_client_buf, alloc_size, 0, &offset);
    } else {
        return UMM_E_NOT_INITIALIZED;
    }
    if (rc != UMM_OK) {
        log_error(__FILE__, __LINE__,
                  "umm_alloc: memory allocation failed (rc=%d)", rc);
        return rc;
    }

    /* Step 2: Compose GPA (always CXL tier for standard allocations) */
    gpa_t gpa = make_gpa(g_state.config.my_node_id, UMM_TIER_CXL, offset);

    /* Step 3: Register with ummd (ALL chunks, no exceptions) */
    char name[64];
    snprintf(name, sizeof(name), "chunk_%u_%lu",
             (unsigned)g_state.config.my_node_id, (unsigned long)offset);
    chunk_id_t cid = 0;

    if (g_state.local_meta_vtbl) {
        /* Direct mode */
        rc = g_state.local_meta_vtbl->register_chunk(g_state.local_meta_ctx,
                                                      name, gpa, alloc_size, &cid);
    } else if (g_state.meta_client_buf) {
        /* RPC mode */
        rc = meta_rpc_register_chunk(g_state.meta_client_buf, name,
                                      gpa, alloc_size, &cid);
    } else {
        /* Rollback memory allocation */
        if (g_state.local_mem_vtbl) {
            g_state.local_mem_vtbl->free_local(g_state.local_mem_ctx,
                                                offset, alloc_size);
        } else if (g_state.mem_client_buf) {
            mem_rpc_free(g_state.mem_client_buf, offset, alloc_size);
        }
        return UMM_E_NOT_INITIALIZED;
    }
    if (rc != UMM_OK) {
        log_error(__FILE__, __LINE__,
                  "umm_alloc: chunk registration failed (rc=%d), rolling back",
                  rc);
        /* Rollback memory allocation */
        if (g_state.local_mem_vtbl) {
            g_state.local_mem_vtbl->free_local(g_state.local_mem_ctx,
                                                offset, alloc_size);
        } else if (g_state.mem_client_buf) {
            mem_rpc_free(g_state.mem_client_buf, offset, alloc_size);
        }
        return rc;
    }

    /* Step 4: Fill descriptor */
    out->chunk_id  = cid;
    out->base_gpa  = gpa;
    out->user_size = alloc_size;

    log_debug(__FILE__, __LINE__,
              "umm_alloc: chunk=%lu, gpa=0x%lx, size=%lu, node=%u",
              (unsigned long)cid, (unsigned long)gpa,
              (unsigned long)alloc_size,
              (unsigned)g_state.config.my_node_id);
    return UMM_OK;
}

/* ------------------------------------------------------------------------ */
/* umm_alloc_tiered  --  Allocate on a specific storage tier                 */
/* ------------------------------------------------------------------------ */

static int alloc_tier_impl(uint64_t size, tier_id_t tier, int on_device,
                           uint32_t device_idx, ChunkDescriptor *out)
{
    if (!g_state.initialized)
        return UMM_E_NOT_INITIALIZED;

    if (size == 0 || !out || tier >= UMM_NUM_TIERS ||
        size > UINT64_MAX - (UMM_PAGE_SIZE - 1))
        return UMM_E_INVALID_ARG;

    memset(out, 0, sizeof(ChunkDescriptor));
    if (on_device && tier != UMM_TIER_SSD)
        return UMM_E_UNSUPPORTED;

    uint64_t alloc_size = (size + UMM_PAGE_SIZE - 1) & ~(UMM_PAGE_SIZE - 1);
    uint64_t offset = 0;
    int rc;

    /* Phase 1：GPA node 位 = 数据属主节点。SSD tier 在 RPC 模式下属主是
     * umms（数据在其池中）；其余情况维持 my_node_id（CXL 数据面本就是
     * client 进程本地，direct 模式也属本节点）。 */
    node_id_t gpa_node = g_state.config.my_node_id;

    /* Step 1: Allocate physical memory from the specified tier */
    if (on_device && g_state.local_mem_vtbl) {
        if (!g_state.local_mem_vtbl->alloc_on_device)
            return UMM_E_UNSUPPORTED;
        rc = g_state.local_mem_vtbl->alloc_on_device(g_state.local_mem_ctx,
                                                      tier, device_idx,
                                                      alloc_size, &offset);
    } else if (g_state.local_mem_vtbl && g_state.local_mem_vtbl->alloc_tiered) {
        /* Direct mode with tier support */
        rc = g_state.local_mem_vtbl->alloc_tiered(g_state.local_mem_ctx,
                                                   tier, alloc_size, &offset);
    } else if (g_state.local_mem_vtbl) {
        /* Fallback: use alloc_local (legacy, works for CXL tier) */
        if (tier != UMM_TIER_CXL) {
            log_error(__FILE__, __LINE__,
                      "umm_alloc_tiered: umms does not support tier %u allocations",
                      (unsigned)tier);
            return UMM_E_INVALID_ARG;
        }
        rc = g_state.local_mem_vtbl->alloc_local(g_state.local_mem_ctx,
                                                  alloc_size, &offset);
    } else if (g_state.mem_client_buf) {
        /* RPC mode: use tiered alloc RPC（v2 响应携带属主 node） */
        uint8_t owner = UMM_NODE_UNKNOWN;
        if (on_device)
            rc = mem_rpc_alloc_on_device(g_state.mem_client_buf, tier, device_idx,
                                          alloc_size, 0, &offset, &owner);
        else
            rc = mem_rpc_alloc_tiered2(g_state.mem_client_buf, tier,
                                        alloc_size, 0, &offset, &owner);
        if (rc == UMM_OK && tier == UMM_TIER_SSD) {
            /* 属主解析：新服务端响应 > ssd_owner_node 配置 > my_node_id
             *（旧服务端/旧行为） */
            if (owner != UMM_NODE_UNKNOWN) {
                gpa_node = owner;
                if (g_state.mem_server_node != owner) {
                    g_state.mem_server_node = owner;
                    if (g_state.remote_transport_ctx) {
                        remote_transport_set_fallback(
                            g_state.remote_transport_ctx, owner,
                            g_state.config.mem_server_addr);
                    }
                }
            } else if (g_state.config.ssd_owner_node != UMM_NODE_UNKNOWN) {
                gpa_node = g_state.config.ssd_owner_node;
            }
        }
    } else {
        return UMM_E_NOT_INITIALIZED;
    }
    if (rc != UMM_OK) {
        log_error(__FILE__, __LINE__,
                  "umm_alloc_tiered: tier=%u alloc failed (rc=%d)",
                  (unsigned)tier, rc);
        return rc;
    }

    /* Step 2: Compose GPA with the specified tier（node 位 = 数据属主） */
    gpa_t gpa = make_gpa(gpa_node, tier, offset);

    /* Step 3: Register with ummd */
    char name[64];
    snprintf(name, sizeof(name), "chunk_%u_%lu_tier%u",
             (unsigned)g_state.config.my_node_id,
             (unsigned long)offset, (unsigned)tier);
    chunk_id_t cid = 0;

    if (g_state.local_meta_vtbl) {
        rc = g_state.local_meta_vtbl->register_chunk(g_state.local_meta_ctx,
                                                      name, gpa, alloc_size, &cid);
    } else if (g_state.meta_client_buf) {
        rc = meta_rpc_register_chunk(g_state.meta_client_buf, name,
                                      gpa, alloc_size, &cid);
    } else {
        /* Rollback */
        if (g_state.local_mem_vtbl && g_state.local_mem_vtbl->free_tiered) {
            g_state.local_mem_vtbl->free_tiered(g_state.local_mem_ctx,
                                                 tier, offset, alloc_size);
        } else if (g_state.local_mem_vtbl) {
            g_state.local_mem_vtbl->free_local(g_state.local_mem_ctx,
                                                offset, alloc_size);
        } else if (g_state.mem_client_buf) {
            mem_rpc_free_tiered(g_state.mem_client_buf, tier, offset, alloc_size);
        }
        return UMM_E_NOT_INITIALIZED;
    }
    if (rc != UMM_OK) {
        log_error(__FILE__, __LINE__,
                  "umm_alloc_tiered: chunk registration failed (rc=%d), "
                  "rolling back", rc);
        /* Rollback */
        if (g_state.local_mem_vtbl && g_state.local_mem_vtbl->free_tiered) {
            g_state.local_mem_vtbl->free_tiered(g_state.local_mem_ctx,
                                                 tier, offset, alloc_size);
        } else if (g_state.local_mem_vtbl) {
            g_state.local_mem_vtbl->free_local(g_state.local_mem_ctx,
                                                offset, alloc_size);
        } else if (g_state.mem_client_buf) {
            mem_rpc_free_tiered(g_state.mem_client_buf, tier, offset, alloc_size);
        }
        return rc;
    }

    /* Step 4: Fill descriptor */
    out->chunk_id  = cid;
    out->base_gpa  = gpa;
    out->user_size = alloc_size;

    log_info(__FILE__, __LINE__,
             "umm_alloc_tiered: tier=%s chunk=%lu gpa=0x%lx size=%lu",
             umm_tier_name(tier), (unsigned long)cid,
             (unsigned long)gpa, (unsigned long)alloc_size);
    return UMM_OK;
}

int umm_alloc_tiered(uint64_t size, tier_id_t tier, ChunkDescriptor *out)
{
    return alloc_tier_impl(size, tier, 0, 0, out);
}

int umm_alloc_on_device(uint64_t size, tier_id_t tier, uint32_t device_idx,
                        ChunkDescriptor *out)
{
    return alloc_tier_impl(size, tier, 1, device_idx, out);
}


/* ------------------------------------------------------------------------ */
/* umm_free                                                                  */
/* ------------------------------------------------------------------------ */

int umm_free(ChunkDescriptor *desc)
{
    if (!g_state.initialized)
        return UMM_E_NOT_INITIALIZED;

    int rc = umm_desc_validate(desc);
    if (rc != UMM_OK)
        return rc;

    uint64_t offset = gpa_to_offset(desc->base_gpa);
    int final_rc = UMM_OK;

    /* Step 1: Unregister chunk from ummd */
    if (desc->chunk_id != 0) {
        if (g_state.local_meta_vtbl) {
            int rc2 = g_state.local_meta_vtbl->unregister_chunk(
                                                g_state.local_meta_ctx,
                                                desc->chunk_id);
            if (rc2 != UMM_OK) {
                log_warn(__FILE__, __LINE__,
                         "umm_free: unregister_chunk failed (rc=%d)", rc2);
                if (final_rc == UMM_OK)
                    final_rc = rc2;
            }
        } else if (g_state.meta_client_buf) {
            int rc2 = meta_rpc_unregister_chunk(g_state.meta_client_buf,
                                                 desc->chunk_id);
            if (rc2 != UMM_OK) {
                log_warn(__FILE__, __LINE__,
                         "umm_free: meta_rpc_unregister_chunk failed (rc=%d)",
                         rc2);
                if (final_rc == UMM_OK)
                    final_rc = rc2;
            }
        }
    }

    /* Step 2: Free physical memory（按 GPA 中的 tier 选择释放路径） */
    tier_id_t tier = gpa_to_tier(desc->base_gpa);
    if (g_state.local_mem_vtbl) {
        int rc2;
        if (tier != UMM_TIER_CXL && g_state.local_mem_vtbl->free_tiered) {
            rc2 = g_state.local_mem_vtbl->free_tiered(
                      g_state.local_mem_ctx, tier, offset, desc->user_size);
        } else {
            rc2 = g_state.local_mem_vtbl->free_local(
                      g_state.local_mem_ctx, offset, desc->user_size);
        }
        if (rc2 != UMM_OK) {
            log_error(__FILE__, __LINE__,
                      "umm_free: local free failed (rc=%d)", rc2);
            if (final_rc == UMM_OK)
                final_rc = rc2;
        }
    } else if (g_state.mem_client_buf) {
        int rc2;
        if (tier != UMM_TIER_CXL) {
            rc2 = mem_rpc_free_tiered(g_state.mem_client_buf, tier,
                                       offset, desc->user_size);
        } else {
            rc2 = mem_rpc_free(g_state.mem_client_buf,
                               offset, desc->user_size);
        }
        if (rc2 != UMM_OK) {
            log_error(__FILE__, __LINE__,
                      "umm_free: mem_rpc_free failed (rc=%d)", rc2);
            if (final_rc == UMM_OK)
                final_rc = rc2;
        }
    } else {
        return UMM_E_NOT_INITIALIZED;
    }

    /* Zero the descriptor to prevent use-after-free */
    umm_desc_init(desc);
    return final_rc;
}

/* ------------------------------------------------------------------------ */
/* Data movement                                                            */
/* ------------------------------------------------------------------------ */

int umm_read(const ChunkDescriptor *desc, uint64_t offset,
             uint64_t len, void *out_buf)
{
    if (!g_state.initialized)
        return UMM_E_NOT_INITIALIZED;

    if (!desc || !out_buf || len == 0)
        return UMM_E_INVALID_ARG;

    if (desc->user_size == 0)
        return UMM_E_INVALID_ARG;

    /* Bounds check */
    if (offset + len < offset || offset + len > desc->user_size)
        return UMM_E_INVALID_ARG;

    if (!g_state.transport || !g_state.transport->get)
        return UMM_E_NOT_INITIALIZED;

    return g_state.transport->get(g_state.transport_ctx,
                                   desc->base_gpa + offset, len, out_buf);
}

int umm_write(const ChunkDescriptor *desc, uint64_t offset,
              uint64_t len, const void *buf)
{
    if (!g_state.initialized)
        return UMM_E_NOT_INITIALIZED;

    if (!desc || !buf || len == 0)
        return UMM_E_INVALID_ARG;

    if (desc->user_size == 0)
        return UMM_E_INVALID_ARG;

    /* Bounds check */
    if (offset + len < offset || offset + len > desc->user_size)
        return UMM_E_INVALID_ARG;

    if (!g_state.transport || !g_state.transport->put)
        return UMM_E_NOT_INITIALIZED;

    return g_state.transport->put(g_state.transport_ctx,
                                   desc->base_gpa + offset, len, buf);
}

/* ------------------------------------------------------------------------ */
/* Atomic operations                                                         */
/* ------------------------------------------------------------------------ */

int umm_atomic_cas(const ChunkDescriptor *desc, uint64_t offset,
                   uint64_t expected, uint64_t desired, uint64_t *old)
{
    if (!g_state.initialized)
        return UMM_E_NOT_INITIALIZED;

    if (!desc)
        return UMM_E_INVALID_ARG;

    /* Bounds check: need 8 bytes for uint64_t */
    if (offset + sizeof(uint64_t) < offset ||
        offset + sizeof(uint64_t) > desc->user_size)
        return UMM_E_INVALID_ARG;

    if (!g_state.transport || !g_state.transport->atomic_cas)
        return UMM_E_NOT_INITIALIZED;

    return g_state.transport->atomic_cas(g_state.transport_ctx,
                                          desc->base_gpa + offset,
                                          expected, desired, old);
}

int umm_atomic_fetch_add(const ChunkDescriptor *desc, uint64_t offset,
                         uint64_t value, uint64_t *result)
{
    if (!g_state.initialized)
        return UMM_E_NOT_INITIALIZED;

    if (!desc)
        return UMM_E_INVALID_ARG;

    /* Bounds check: need 8 bytes for uint64_t */
    if (offset + sizeof(uint64_t) < offset ||
        offset + sizeof(uint64_t) > desc->user_size)
        return UMM_E_INVALID_ARG;

    if (!g_state.transport || !g_state.transport->atomic_fetch_add)
        return UMM_E_NOT_INITIALIZED;

    return g_state.transport->atomic_fetch_add(g_state.transport_ctx,
                                                desc->base_gpa + offset,
                                                value, result);
}

int umm_atomic_set(const ChunkDescriptor *desc, uint64_t offset,
                   uint64_t value)
{
    if (!g_state.initialized)
        return UMM_E_NOT_INITIALIZED;

    if (!desc)
        return UMM_E_INVALID_ARG;

    /* Bounds check: need 8 bytes for uint64_t */
    if (offset + sizeof(uint64_t) < offset ||
        offset + sizeof(uint64_t) > desc->user_size)
        return UMM_E_INVALID_ARG;

    if (!g_state.transport || !g_state.transport->atomic_set)
        return UMM_E_NOT_INITIALIZED;

    return g_state.transport->atomic_set(g_state.transport_ctx,
                                          desc->base_gpa + offset, value);
}

/* ------------------------------------------------------------------------ */
/* umm_register_storage_tier -- register a storage device for a tier       */
/* ------------------------------------------------------------------------ */

int umm_register_storage_tier(tier_id_t tier, const char *device_path,
                               uint64_t capacity)
{
    if (!g_state.initialized)
        return UMM_E_NOT_INITIALIZED;
    if (tier >= UMM_NUM_TIERS || !device_path || capacity == 0)
        return UMM_E_INVALID_ARG;

    StorageResource res;
    memset(&res, 0, sizeof(res));
    res.tier     = tier;
    res.capacity = capacity;
    res.online   = 1;
    strncpy(res.device_path, device_path, sizeof(res.device_path) - 1);

    /* Direct mode: register with local mem_service */
    if (g_state.local_mem_vtbl && g_state.local_mem_vtbl->register_storage) {
        int rc = g_state.local_mem_vtbl->register_storage(g_state.local_mem_ctx,
                                                           &res);
        if (rc != UMM_OK)
            return rc;

        /* Re-configure transports with the new topology */
        if (g_state.tier_router) {
            StorageTopology topo;
            memset(&topo, 0, sizeof(topo));
            rc = g_state.local_mem_vtbl->get_topology(g_state.local_mem_ctx, &topo);
            if (rc == UMM_OK && topo.num_resources > 0) {
                /* Tear down old router */
                tier_router_destroy(g_state.tier_router);
                g_state.tier_router = NULL;
                g_state.transport = NULL;
                g_state.transport_ctx = NULL;

                /* Re-create transports from updated topology */
                rc = configure_transports_from_topology(&g_state.config, &topo);
                if (rc != UMM_OK) {
                    log_error(__FILE__, __LINE__,
                              "umm_register_storage_tier: transport re-config "
                              "failed (rc=%d)", rc);
                    return rc;
                }
                log_info(__FILE__, __LINE__,
                         "transport re-configured after registering %s tier",
                         umm_tier_name(tier));
            }
        }
        return UMM_OK;
    }

    /* RPC mode: 上报 ummD 全局拓扑 + 重建本地数据面 transport。
     * 架构事实: RPC 仅承载分配/元数据, SSD 数据面在 client 进程本地
     * (tier_router → transport_ssd → 本地 ssd_pool → 设备)。
     * 应在 umm_init 之后、任何数据 I/O 之前调用（router 整体重建）。 */
    if (is_rpc_mode()) {
        /* 1) 上报 ummD（供本 client 重查及其他 client 发现拓扑）。
         *    失败仅告警——本地数据面不依赖 ummD 是否记录 */
        if (g_state.meta_client_buf) {
            int rrc = meta_rpc_register_storage_resource(
                          g_state.meta_client_buf,
                          g_state.config.my_node_id, &res);
            if (rrc != UMM_OK)
                log_warn(__FILE__, __LINE__,
                         "umm_register_storage_tier: report to ummd failed "
                         "(rc=%d), local datapath continues", rrc);
        }

        /* 1.5) SSD 设备累加进本机数据面设备表（按路径去重）——
         *      共享池实验里每个 VM 依次 enable 全部共享盘，
         *      re-config 据此建多设备 pool（顺序须与 umms 一致） */
        if (tier == UMM_TIER_SSD && device_path[0] != '\0' &&
            strncmp(device_path, "nds:", 4) != 0) {
            UMMConfig *c   = &g_state.config;
            uint32_t   idx = 0;
            while (idx < c->num_ssd_devices &&
                   strncmp(c->ssd_devices[idx].path, device_path,
                           sizeof(c->ssd_devices[idx].path)) != 0)
                idx++;
            if (idx == c->num_ssd_devices) {
                if (idx >= UMM_MAX_SSD_DEVICES) {
                    log_error(__FILE__, __LINE__,
                              "umm_register_storage_tier: too many SSD "
                              "devices (max=%d)", UMM_MAX_SSD_DEVICES);
                    return UMM_E_INVALID_ARG;
                }
                strncpy(c->ssd_devices[idx].path, device_path,
                        sizeof(c->ssd_devices[idx].path) - 1);
                c->ssd_devices[idx].size = capacity;
                c->num_ssd_devices++;
            } else {
                c->ssd_devices[idx].size = capacity;  /* 容量以最新为准 */
            }
        }

        /* 2) 重建本地 tier router（含新 SSD transport） */
        if (g_state.tier_router) {
            StorageTopology topo;
            memset(&topo, 0, sizeof(topo));
            topo.node_id = g_state.config.my_node_id;
            topo.resources[res.tier] = res;   /* 稀疏槽位, 遍历侧已兼容 */
            topo.num_resources = 1;

            /* 若此前已从 ummD 拿到拓扑, 合并已有 SSD 之外的资源 */
            /* （Phase 3 单 SSD 设备场景: 直接以本次注册为准） */

            tier_router_destroy(g_state.tier_router);
            g_state.tier_router = NULL;
            g_state.transport = NULL;
            g_state.transport_ctx = NULL;
            if (g_state.ssd_transport_vtbl && g_state.ssd_transport_ctx) {
                ssd_transport_destroy(g_state.ssd_transport_ctx);
                g_state.ssd_transport_vtbl = NULL;
                g_state.ssd_transport_ctx = NULL;
            }

            int crc = configure_transports_from_topology(&g_state.config,
                                                          &topo);
            if (crc != UMM_OK) {
                log_error(__FILE__, __LINE__,
                          "umm_register_storage_tier: transport re-config "
                          "failed (rc=%d)", crc);
                return crc;
            }
            log_info(__FILE__, __LINE__,
                     "rpc mode: local transport re-configured for %s tier "
                     "(dev=%s, cap=%lu)",
                     umm_tier_name(tier), device_path,
                     (unsigned long)capacity);
        }
        return UMM_OK;
    }

    return UMM_E_INVALID_ARG;
}

/* ------------------------------------------------------------------------ */
/* umm_get_topology -- query storage topology (direct: local; rpc: ummD)   */
/* ------------------------------------------------------------------------ */

int umm_get_topology(StorageTopology *out)
{
    if (!g_state.initialized)
        return UMM_E_NOT_INITIALIZED;
    if (!out)
        return UMM_E_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    if (g_state.local_mem_vtbl && g_state.local_mem_vtbl->get_topology)
        return g_state.local_mem_vtbl->get_topology(g_state.local_mem_ctx,
                                                     out);
    if (g_state.mem_client_buf) {
        int rc = mem_rpc_get_topology(g_state.mem_client_buf, out);
        if (rc != UMM_E_UNSUPPORTED) return rc;
    }
    if (g_state.meta_client_buf) {
        int rc = meta_rpc_get_storage_topology(g_state.meta_client_buf,
                                                g_state.config.my_node_id,
                                                out);
        /* ummD 尚无本节点记录 → 语义上是"空拓扑"而非错误 */
        if (rc != UMM_OK) {
            memset(out, 0, sizeof(*out));
            out->node_id = g_state.config.my_node_id;
            return UMM_OK;
        }
        return rc;
    }
    return UMM_E_NOT_INITIALIZED;
}

/* ------------------------------------------------------------------------ */
/* umm_tier_name -- human-readable tier name                               */
/* ------------------------------------------------------------------------ */

const char* umm_tier_name(tier_id_t tier)
{
    switch (tier) {
    case UMM_TIER_DRAM: return "DRAM";
    case UMM_TIER_CXL:  return "CXL";
    case UMM_TIER_SSD:  return "SSD";
    default:            return "???";
    }
}

/* ------------------------------------------------------------------------ */
/* Chunk lookup (cross-node sharing)                                        */
/* ------------------------------------------------------------------------ */

int umm_lookup_chunk(const char *name, ChunkMetadata *out)
{
    if (!g_state.initialized)
        return UMM_E_NOT_INITIALIZED;

    if (!name || !out)
        return UMM_E_INVALID_ARG;

    memset(out, 0, sizeof(ChunkMetadata));

    if (g_state.local_meta_vtbl) {
        /* Direct mode */
        return g_state.local_meta_vtbl->lookup_chunk(g_state.local_meta_ctx,
                                                      name, out);
    } else if (g_state.meta_client_buf) {
        /* RPC mode */
        return meta_rpc_lookup_chunk(g_state.meta_client_buf, name, out);
    }

    return UMM_E_NOT_INITIALIZED;
}

/* ------------------------------------------------------------------------ */
/* Synchronization                                                           */
/* ------------------------------------------------------------------------ */

void umm_fence(void)
{
    if (!g_state.initialized || !g_state.transport)
        return;

    if (g_state.transport->fence)
        g_state.transport->fence(g_state.transport_ctx);
}

/*
 * umm_invalidate — 丢弃本进程对指定 chunk 区域的缓存页视图（仅 SSD tier）。
 *
 * 共享盘读共享场景：另一节点 umm_write + umm_fence 落盘后，本节点须先
 * invalidate 再读，否则 mmap 页缓存返回旧数据（guest/OS 页缓存不会自动
 * 感知外部经同一后备存储的写入——这是共享盘模型最隐蔽的坑）。
 *
 * DRAM/CXL 等易失层无此问题（数据面本就是本进程内存），返回 UMM_OK no-op。
 * SSD tier 但本地无 SSD 数据面（如 nds: 直驱）返回 UMM_E_UNSUPPORTED。
 *
 * 注意：会丢弃本进程映射在该区域的脏页——调用方须保证该区域本进程
 * 没有未 fence 的写。
 */
int umm_invalidate(const ChunkDescriptor *desc, uint64_t offset, uint64_t len)
{
    if (!g_state.initialized)
        return UMM_E_NOT_INITIALIZED;
    if (!desc || len == 0)
        return UMM_E_INVALID_ARG;
    if (desc->user_size == 0)
        return UMM_E_INVALID_ARG;
    if (offset + len < offset || offset + len > desc->user_size)
        return UMM_E_INVALID_ARG;

    tier_id_t tier = gpa_to_tier(desc->base_gpa);
    if (tier != UMM_TIER_SSD)
        return UMM_OK;  /* 易失层：无缓存视图问题，no-op */

    if (!g_state.ssd_transport_ctx)
        return UMM_E_UNSUPPORTED;

    return ssd_transport_invalidate(g_state.ssd_transport_ctx,
                                    desc->base_gpa + offset, len);
}

void umm_barrier_all(void)
{
    if (!g_state.initialized || !g_state.transport)
        return;

    if (g_state.transport->barrier_all)
        g_state.transport->barrier_all(g_state.transport_ctx);
}

void umm_quiet(void)
{
    if (!g_state.initialized || !g_state.transport)
        return;

    if (g_state.transport->quiet)
        g_state.transport->quiet(g_state.transport_ctx);
}
