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

/* Tier-aware allocation RPC */
extern int mem_rpc_alloc_tiered(MemRpcClient *c, tier_id_t tier, uint64_t size,
                                uint32_t flags, uint64_t *out_offset);
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

    if (topo && topo->num_resources > 0) {
        for (uint32_t i = 0; i < topo->num_resources; i++) {
            const StorageResource *res = &topo->resources[i];
            if (!res->online)
                continue;

            if (res->tier == UMM_TIER_SSD) {
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
            rc = mem_rpc_client_init(g_state.mem_client_buf, mem_host, mem_port);
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

int umm_alloc_tiered(uint64_t size, tier_id_t tier, ChunkDescriptor *out)
{
    if (!g_state.initialized)
        return UMM_E_NOT_INITIALIZED;

    if (size == 0 || !out || tier >= UMM_NUM_TIERS)
        return UMM_E_INVALID_ARG;

    memset(out, 0, sizeof(ChunkDescriptor));

    uint64_t alloc_size = (size + UMM_PAGE_SIZE - 1) & ~(UMM_PAGE_SIZE - 1);
    uint64_t offset = 0;
    int rc;

    /* Step 1: Allocate physical memory from the specified tier */
    if (g_state.local_mem_vtbl && g_state.local_mem_vtbl->alloc_tiered) {
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
        /* RPC mode: use tiered alloc RPC */
        rc = mem_rpc_alloc_tiered(g_state.mem_client_buf, tier,
                                   alloc_size, 0, &offset);
    } else {
        return UMM_E_NOT_INITIALIZED;
    }
    if (rc != UMM_OK) {
        log_error(__FILE__, __LINE__,
                  "umm_alloc_tiered: tier=%u alloc failed (rc=%d)",
                  (unsigned)tier, rc);
        return rc;
    }

    /* Step 2: Compose GPA with the specified tier */
    gpa_t gpa = make_gpa(g_state.config.my_node_id, tier, offset);

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

    /* Step 2: Free physical memory */
    if (g_state.local_mem_vtbl) {
        int rc2 = g_state.local_mem_vtbl->free_local(g_state.local_mem_ctx,
                                                      offset, desc->user_size);
        if (rc2 != UMM_OK) {
            log_error(__FILE__, __LINE__,
                      "umm_free: local free failed (rc=%d)", rc2);
            if (final_rc == UMM_OK)
                final_rc = rc2;
        }
    } else if (g_state.mem_client_buf) {
        int rc2 = mem_rpc_free(g_state.mem_client_buf,
                               offset, desc->user_size);
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

    /* RPC mode not yet supported for client-side registration */
    return UMM_E_INVALID_ARG;
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
