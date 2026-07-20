/* ========================================================================
 * umm_transport.h  --  Transport initialization and management
 *
 * Wraps the low-level transport factory to give the client library a
 * convenient init/deinit interface.
 * ======================================================================== */

#ifndef UMM_TRANSPORT_H
#define UMM_TRANSPORT_H

#include "../transport/transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the transport layer based on the configuration.
 *
 * @param cfg       UMM configuration (transport field selects the backend).
 * @param[out] out_vtbl  Receives the transport vtable pointer.
 * @param[out] out_ctx   Receives the transport context pointer.
 * @return          UMM_OK on success, error code otherwise.
 */
int umm_transport_lib_init(const UMMConfig *cfg,
                           MemoryTransportVtbl **out_vtbl,
                           void **out_ctx);

/**
 * Shut down the transport layer and release its resources.
 *
 * @param vtbl  Transport vtable pointer (from umm_transport_lib_init).
 * @param ctx   Transport context pointer (from umm_transport_lib_init).
 */
void umm_transport_lib_deinit(MemoryTransportVtbl *vtbl, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* UMM_TRANSPORT_H */
