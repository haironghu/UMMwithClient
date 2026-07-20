#ifndef META_SERVER_H
#define META_SERVER_H

#include "../metadata_service/meta_service.h"

typedef struct MetaServer MetaServer;

MetaServer* meta_server_create(const char *bind_addr, int port,
                                MetadataServiceVtbl *vtbl, void *service_ctx);
void meta_server_destroy(MetaServer *server);
int meta_server_start(MetaServer *server);
void meta_server_stop(MetaServer *server);
int meta_server_is_running(MetaServer *server);

#endif
