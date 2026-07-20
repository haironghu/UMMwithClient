/* ========================================================================
 * cis_router.c  --  Cluster routing placeholder (implementation)
 * ======================================================================== */

#include "cis_router.h"
#include "../common/log.h"
#include "../common/error_codes.h"

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ------------------------------------------------------------------------ */
/* Logging helpers                                                          */
/* ------------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------------ */
/* Internal state                                                            */
/* ------------------------------------------------------------------------ */

typedef struct {
    int        initialized;
    node_id_t  local_node;
    node_id_t  next_node;   /* round-robin counter */
} CisRouterState;

static CisRouterState g_router = {0};

/* ------------------------------------------------------------------------ */
/* Public API                                                                */
/* ------------------------------------------------------------------------ */

int cis_router_init(const UMMConfig *cfg)
{
    if (!cfg)
        return UMM_E_INVALID_ARG;

    if (g_router.initialized)
        return UMM_E_ALREADY_EXISTS;

    g_router.local_node = cfg->my_node_id;
    g_router.next_node  = cfg->my_node_id;
    g_router.initialized = 1;

    log_warn(__FILE__, __LINE__,
             "CIS router not fully implemented -- using local-node routing");
    return UMM_OK;
}

void cis_router_deinit(void)
{
    if (!g_router.initialized)
        return;

    memset(&g_router, 0, sizeof(g_router));
}

node_id_t cis_route_alloc_node(uint64_t size)
{
    (void)size;

    if (!g_router.initialized) {
        log_error(__FILE__, __LINE__,
                  "cis_route_alloc_node: router not initialized");
        return 0;
    }

    /* Placeholder: always route to local node.
     * A real implementation would consider:
     *   - Available memory on each node
     *   - Network topology / NUMA affinity
     *   - Current load / queue depths
     *   - Replication requirements
     */
    return g_router.local_node;
}

int cis_router_register_node(node_id_t node, const char *addr)
{
    if (!g_router.initialized)
        return UMM_E_NOT_INITIALIZED;

    (void)node;
    (void)addr;

    log_warn(__FILE__, __LINE__,
             "cis_router_register_node: stub -- node not actually registered");
    return UMM_OK;
}

int cis_router_get_node_addr(node_id_t node, char *out_addr, size_t len)
{
    if (!g_router.initialized)
        return UMM_E_NOT_INITIALIZED;

    if (!out_addr || len == 0)
        return UMM_E_INVALID_ARG;

    (void)node;

    /* Placeholder: no addresses known */
    log_warn(__FILE__, __LINE__,
             "cis_router_get_node_addr: stub -- no address for node %u",
             (unsigned)node);

    if (len > 0)
        out_addr[0] = '\0';

    return UMM_E_NOT_FOUND;
}
