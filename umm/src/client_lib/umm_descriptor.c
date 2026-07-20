/* ========================================================================
 * umm_descriptor.c  --  ChunkDescriptor utilities (implementation)
 * ======================================================================== */

#include "umm_descriptor.h"
#include "../common/error_codes.h"

void umm_desc_init(ChunkDescriptor *desc)
{
    if (!desc)
        return;
    desc->chunk_id  = 0;
    desc->base_gpa  = 0;
    desc->user_size = 0;
}

int umm_desc_validate(const ChunkDescriptor *desc)
{
    if (!desc)
        return UMM_E_INVALID_ARG;

    /* A valid descriptor must have either a chunk_id or a base_gpa set.
     * user_size must be > 0 for any real allocation. */
    if (desc->chunk_id == 0 && desc->base_gpa == 0)
        return UMM_E_INVALID_ARG;

    if (desc->user_size == 0)
        return UMM_E_INVALID_ARG;

    return UMM_OK;
}
