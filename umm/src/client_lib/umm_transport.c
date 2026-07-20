/* ========================================================================
 * umm_transport.c  --  Transport initialization and management (impl)
 *
 * Implements the transport factory that creates the right backend
 * (mock or cxl) based on the configuration string.
 *
 * Phase 5: When cfg->transport is empty (tier-aware mode), this layer
 * defers transport creation to umm_api.c which uses the topology from
 * ummd to configure multiple tier transports via tier_router.
 * ======================================================================== */

#include "umm_transport.h"
#include "../common/log.h"
#include "../common/error_codes.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* External vtable getters (provided by each transport backend)             */
/* ------------------------------------------------------------------------ */

extern const MemoryTransportVtbl *umm_local_vtbl_get(void);

/* ------------------------------------------------------------------------ */
/* Internal helpers                                                         */
/* ------------------------------------------------------------------------ */

static void log_info(const char *file, int line, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    umm_log(UMM_LOG_INFO, file, line, fmt, ap);
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
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

int umm_transport_lib_init(const UMMConfig *cfg,
                           MemoryTransportVtbl **out_vtbl,
                           void **out_ctx)
{
    if (!cfg || !out_vtbl || !out_ctx)
        return UMM_E_INVALID_ARG;

    /* Phase 5: tier-aware mode -- transport selection deferred to umm_api.c
     * which configures transports from topology via tier_router. */
    if (cfg->transport[0] == '\0') {
        *out_vtbl = NULL;
        *out_ctx  = NULL;
        log_info(__FILE__, __LINE__,
                 "transport: tier-aware mode, deferring to topology setup");
        return UMM_OK;
    }

    const MemoryTransportVtbl *vtbl = NULL;

    /* Select transport backend ("mock" and "cxl" are aliases for "local") */
    if (strcmp(cfg->transport, "mock") == 0 ||
        strcmp(cfg->transport, "cxl") == 0 ||
        strcmp(cfg->transport, "local") == 0) {
        vtbl = umm_local_vtbl_get();
        log_info(__FILE__, __LINE__, "transport: selected local backend (%s)",
                 cfg->transport);
    } else {
        log_error(__FILE__, __LINE__, "transport: unknown transport type '%s', "
                  "expected 'local', 'mock', or 'cxl'", cfg->transport);
        return UMM_E_INVALID_ARG;
    }

    if (!vtbl)
        return UMM_E_UNKNOWN;

    /* Allocate context -- must be large enough for the biggest
     * transport backend context (CXLTransportCtx with FallbackCtx).
     * A 4 KiB buffer covers all current backends comfortably. */
    void *ctx = calloc(1, 4096);
    if (!ctx)
        return UMM_E_NO_MEMORY;

    /* Initialize backend */
    int rc = vtbl->init(ctx, cfg);
    if (rc != UMM_OK) {
        free(ctx);
        return rc;
    }

    *out_vtbl = (MemoryTransportVtbl *)vtbl;
    *out_ctx  = ctx;
    return UMM_OK;
}

void umm_transport_lib_deinit(MemoryTransportVtbl *vtbl, void *ctx)
{
    if (!vtbl || !ctx)
        return;

    if (vtbl->deinit)
        vtbl->deinit(ctx);

    free(ctx);
}

/* ------------------------------------------------------------------------ */
/* Aliases for transport.h declared names                                   */
/* ------------------------------------------------------------------------ */

int umm_transport_create(const UMMConfig *cfg,
                         MemoryTransportVtbl **out_vtbl,
                         void **out_ctx)
{
    return umm_transport_lib_init(cfg, out_vtbl, out_ctx);
}

void umm_transport_destroy(MemoryTransportVtbl *vtbl, void *ctx)
{
    umm_transport_lib_deinit(vtbl, ctx);
}
