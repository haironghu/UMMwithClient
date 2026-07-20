#ifndef META_SERVICE_DIRECT_H
#define META_SERVICE_DIRECT_H

#include "meta_service.h"

#ifdef __cplusplus
extern "C" {
#endif

MetadataServiceVtbl* meta_service_direct_create(void **out_ctx);
void meta_service_direct_destroy(void *ctx);

#ifdef __cplusplus
}
#endif

#endif
