#ifndef MEM_SERVICE_DIRECT_H
#define MEM_SERVICE_DIRECT_H

#include "mem_service.h"
#include "../common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

MemoryServiceVtbl* mem_service_direct_create(node_id_t node_id, uint64_t memory_size, uint64_t base_gpa, void **out_ctx);
MemoryServiceVtbl* mem_service_direct_create_v2(node_id_t node_id, void **out_ctx);
void mem_service_direct_destroy(void *ctx);

#ifdef __cplusplus
}
#endif

#endif
