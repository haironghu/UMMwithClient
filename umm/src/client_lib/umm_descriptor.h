/* ========================================================================
 * umm_descriptor.h  --  ChunkDescriptor utilities
 * ======================================================================== */

#ifndef UMM_DESCRIPTOR_H
#define UMM_DESCRIPTOR_H

#include "../common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Zero-initialize a chunk descriptor and set safe defaults.
 */
void umm_desc_init(ChunkDescriptor *desc);

/**
 * Validate that a descriptor looks reasonable (has been filled after alloc).
 *
 * @return UMM_OK if valid, UMM_E_INVALID_ARG otherwise.
 */
int umm_desc_validate(const ChunkDescriptor *desc);

#ifdef __cplusplus
}
#endif

#endif /* UMM_DESCRIPTOR_H */
