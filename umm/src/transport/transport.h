#ifndef TRANSPORT_H
#define TRANSPORT_H

#include "../../include/umm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * MemoryTransportVtbl — Abstract transport interface
 *
 * All storage tiers (DRAM, CXL, SSD) implement this interface.
 * The TierRouter wraps multiple backends and dispatches based on GPA tier_id.
 * ======================================================================== */

typedef struct MemoryTransportVtbl MemoryTransportVtbl;

struct MemoryTransportVtbl {
    int  (*get)(void *ctx, gpa_t gpa, uint64_t len, void *out_buf);
    int  (*put)(void *ctx, gpa_t gpa, uint64_t len, const void *buf);
    int  (*atomic_cas)(void *ctx, gpa_t gpa, uint64_t expected,
                        uint64_t desired, uint64_t *old);
    int  (*atomic_fetch_add)(void *ctx, gpa_t gpa, uint64_t value,
                              uint64_t *result);
    int  (*atomic_set)(void *ctx, gpa_t gpa, uint64_t value);
    void (*fence)(void *ctx);
    void (*barrier_all)(void *ctx);
    void (*quiet)(void *ctx);
    int  (*register_node)(void *ctx, node_id_t node, uint64_t base,
                          uint64_t size, const char *device);
    int  (*init)(void *ctx, const UMMConfig *cfg);
    void (*deinit)(void *ctx);
};

/* Factory: creates the right transport based on cfg->transport */
int umm_transport_create(const UMMConfig *cfg,
                          MemoryTransportVtbl **out_vtbl, void **out_ctx);
void umm_transport_destroy(MemoryTransportVtbl *vtbl, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* TRANSPORT_H */
