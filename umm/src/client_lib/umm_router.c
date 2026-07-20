/* ========================================================================
 * umm_router.c  --  GPA utility wrappers (implementation)
 * ======================================================================== */

#include "umm_router.h"

gpa_t umm_router_gpa_from_local(node_id_t node, uint64_t offset)
{
    /* Local allocations always use CXL tier (Tier 1) */
    return make_gpa(node, UMM_TIER_CXL, offset);
}

node_id_t umm_router_node_from_gpa(gpa_t gpa)
{
    return gpa_to_node(gpa);
}

uint64_t umm_router_offset_from_gpa(gpa_t gpa)
{
    return gpa_to_offset(gpa);
}
