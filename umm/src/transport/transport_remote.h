/* ========================================================================
 * transport_remote.h — 远端数据面 transport（Phase 1）
 *
 * 把"非本节点 GPA"的 get/put 封装为 mem 协议数据面 RPC
 * （MEM_OP_DATA_READ/WRITE），发往数据属主节点的 umms，
 * 由属主节点对本池执行 ssd_pool_pread/pwrite。
 *
 * 地址解析顺序：
 *   1. fallback（mem_server_addr，当 owner == 已知 umms node 时）；
 *   2. cis_router 静态节点表（UMMConfig.peer_nodes）；
 *   3. 均 miss → UMM_E_NOT_FOUND。
 * ======================================================================== */

#ifndef UMM_TRANSPORT_REMOTE_H
#define UMM_TRANSPORT_REMOTE_H

#include "transport.h"
#include "../common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * remote_transport_create — 创建远端数据面 transport。
 *
 * @param my_node      本节点 ID（仅日志用；node 位判断在 tier_router）。
 * @param data_max_io  单 RPC payload 上限（0 = 默认 1MB，钳位 [4KB,16MB]）。
 *                     必须 ≤ 服务端配置，否则超限帧被拒绝。
 * @param rpc_token    共享密钥（与服务端一致）；NULL/空 = 不启用。
 * @param[out] out_ctx 输出 transport 上下文。
 * @return             vtbl（失败返回 NULL）。
 */
MemoryTransportVtbl* remote_transport_create(node_id_t my_node,
                                             uint32_t data_max_io,
                                             const char *rpc_token,
                                             void **out_ctx);

void remote_transport_destroy(void *ctx);

/**
 * remote_transport_set_fallback — 设置/更新 umms 回退地址。
 * alloc 响应学习到服务端 node 后由 umm_api 调用；也可在创建时由
 * 配置 ssd_owner_node 预设。addr 形如 "host:port"。
 */
int remote_transport_set_fallback(void *ctx, node_id_t node,
                                  const char *addr);

#ifdef __cplusplus
}
#endif

#endif /* UMM_TRANSPORT_REMOTE_H */
