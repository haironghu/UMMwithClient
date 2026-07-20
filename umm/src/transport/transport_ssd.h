/* ========================================================================
 * transport_ssd.h — SSD transport factory
 *
 * Public interface for creating an SSD-backed MemoryTransportVtbl.
 *
 * Usage:
 *   void *ssd_ctx = NULL;
 *   MemoryTransportVtbl *ssd_vtbl = ssd_transport_create("/tmp/umm_ssd",
 *                                                         1ULL << 40,
 *                                                         &ssd_ctx);
 *   ssd_vtbl->get(ssd_ctx, gpa, len, buf);
 *   ...
 *   ssd_transport_destroy(ssd_ctx);  // also frees vtbl
 * ======================================================================== */

#ifndef TRANSPORT_SSD_H
#define TRANSPORT_SSD_H

#include "../../include/umm.h"
#include "transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ssd_transport_create — Create an SSD transport backend.
 *
 * @param base_dir   Directory for chunk files (created if not exists).
 * @param max_bytes  Maximum total capacity in bytes.
 * @param out_ctx    Receives the transport context (for vtbl->get(ctx, ...)).
 * @return           MemoryTransportVtbl with SSD-backed implementations,
 *                   or NULL on error.
 */
MemoryTransportVtbl* ssd_transport_create(const char *base_dir,
                                           uint64_t max_bytes,
                                           void **out_ctx);

/**
 * ssd_transport_destroy — Destroy SSD transport and free all resources.
 *
 * This frees both the context and the vtable returned by ssd_transport_create.
 * Do NOT call vtbl->deinit(ctx) directly — use this function instead.
 */
void ssd_transport_destroy(void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* TRANSPORT_SSD_H */
