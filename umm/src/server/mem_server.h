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

/* Phase 2 混合池：multi-device + 内存层 tier 可选。
 * mem_tier = UMM_TIER_CXL（默认旧行为）/ UMM_TIER_DRAM（无 CXL 硬件，
 * 内存层注册即 malloc 后备）。
 * Phase 2.5：mem_device = 内存层后备设备（共享内存窗口，如 virtio-pmem
 * /dev/pmem0；NULL = 旧行为）。 */
MemServer* mem_server_create_multi_tiered(const char *bind_addr, int port,
                                           node_id_t node_id,
                                           uint64_t memory_size,
                                           uint64_t base_gpa,
                                           tier_id_t mem_tier,
                                           const char *mem_device,
                                           const void *ssd_devices,
                                           uint32_t num_ssd_devices);
void mem_server_destroy(MemServer *server);
int mem_server_start(MemServer *server);
void mem_server_stop(MemServer *server);
int mem_server_is_running(MemServer *server);

/* Phase 1 安全配置（start 之前调用；全部参数可选，NULL/0 = 不启用）。
 * rpc_token:    非空时逐请求校验 FNV-1a 摘要（client 需配同一密钥）；
 * allow_cidrs:  非空时 accept 按 CIDR 白名单拦截（IPv4）；
 * data_max_io:  单数据面 RPC payload 上限（0 = 默认 1MB）。 */
int mem_server_set_security(MemServer *server, const char *rpc_token,
                            const char *allow_cidrs, uint32_t data_max_io);

#endif
