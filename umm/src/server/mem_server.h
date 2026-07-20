#ifndef MEM_SERVER_H
#define MEM_SERVER_H

#include "../common/types.h"

typedef struct MemServer MemServer;

MemServer* mem_server_create(const char *bind_addr, int port,
                              node_id_t node_id, uint64_t memory_size,
                              uint64_t base_gpa,
                              const char *ssd_backend_dir);

/* Multi-device version: create server with explicit device list */
MemServer* mem_server_create_multi(const char *bind_addr, int port,
                                    node_id_t node_id, uint64_t memory_size,
                                    uint64_t base_gpa,
                                    const char *ssd_backend_dir,
                                    const void *ssd_devices,
                                    uint32_t num_ssd_devices);
void mem_server_destroy(MemServer *server);
int mem_server_start(MemServer *server);
void mem_server_stop(MemServer *server);
int mem_server_is_running(MemServer *server);

#endif
