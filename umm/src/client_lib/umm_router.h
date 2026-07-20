/* ========================================================================
 * umm_router.h  --  GPA utility wrappers
 *
 * Thin convenience wrappers around the inline GPA helpers in types.h.
 * ======================================================================== */

#ifndef UMM_ROUTER_H
#define UMM_ROUTER_H

#include "../common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Construct a GPA from a node ID and local offset.
 */
gpa_t umm_router_gpa_from_local(node_id_t node, uint64_t offset);

/**
 * Extract the node ID from a GPA.
 */
node_id_t umm_router_node_from_gpa(gpa_t gpa);

/**
 * Extract the local offset from a GPA.
 */
uint64_t umm_router_offset_from_gpa(gpa_t gpa);

#ifdef __cplusplus
}
#endif

#endif /* UMM_ROUTER_H */
