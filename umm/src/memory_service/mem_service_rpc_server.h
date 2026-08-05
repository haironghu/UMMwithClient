#ifndef UMM_MEM_SERVICE_RPC_SERVER_H
#define UMM_MEM_SERVICE_RPC_SERVER_H

#include "../common/types.h"
#include "mem_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Phase 1 服务端运行时配置（进程级，mem_server 启动时调用一次）。
 *
 * @param node_id      本节点 ID（ALLOC_TIERED 响应中作为数据属主返回）。
 * @param rpc_token    共享密钥；NULL/空 = 不校验（旧行为）。非空时
 *                     逐请求校验 header.reserved[6] 的 FNV-1a 摘要。
 * @param data_max_io  单数据面 RPC payload 上限；0 = 默认 1MB，
 *                     钳位 [4KB, 16MB]。
 */
void mem_rpc_server_configure(node_id_t node_id, const char *rpc_token,
                              uint32_t data_max_io);

/** 单请求处理（mem_server 每连接循环调用）。 */
int mem_service_rpc_handle(int client_sock, void *ctx,
                           MemoryServiceVtbl *vtbl);

#ifdef __cplusplus
}
#endif

#endif /* UMM_MEM_SERVICE_RPC_SERVER_H */
