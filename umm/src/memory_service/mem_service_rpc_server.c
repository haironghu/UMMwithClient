#include "mem_service.h"
#include "mem_service_rpc_server.h"

#include "../protocol/mem_protocol.h"
#include "../protocol/protocol_common.h"
#include "../common/types.h"
#include "../common/error_codes.h"
#include "../common/umm_network.h"
#include "../common/log.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ========================================================================
 * Phase 1 服务端运行时配置（进程级单例：一个进程只跑一个 umms）
 * ======================================================================== */

#define UMM_DATA_MAX_IO_DEFAULT  (1u * 1024 * 1024)   /* 1MB */
#define UMM_DATA_MAX_IO_MIN      (4u * 1024)
#define UMM_DATA_MAX_IO_MAX      (16u * 1024 * 1024)

static node_id_t g_srv_node_id      = 0;
static int       g_srv_token_on     = 0;
static uint8_t   g_srv_token[6]     = {0};
static uint32_t  g_srv_data_max_io  = UMM_DATA_MAX_IO_DEFAULT;

void mem_rpc_server_configure(node_id_t node_id, const char *rpc_token,
                              uint32_t data_max_io)
{
    g_srv_node_id = node_id;

    if (rpc_token && rpc_token[0] != '\0') {
        umm_token_digest(rpc_token, g_srv_token);
        g_srv_token_on = 1;
    } else {
        memset(g_srv_token, 0, sizeof(g_srv_token));
        g_srv_token_on = 0;
    }

    if (data_max_io == 0)
        g_srv_data_max_io = UMM_DATA_MAX_IO_DEFAULT;
    else if (data_max_io < UMM_DATA_MAX_IO_MIN)
        g_srv_data_max_io = UMM_DATA_MAX_IO_MIN;
    else if (data_max_io > UMM_DATA_MAX_IO_MAX)
        g_srv_data_max_io = UMM_DATA_MAX_IO_MAX;
    else
        g_srv_data_max_io = data_max_io;

    umm_log_info("mem_rpc_server: configured node=%u token=%s data_max_io=%u",
                 (unsigned)g_srv_node_id,
                 g_srv_token_on ? "ON" : "off", g_srv_data_max_io);
}

/* ========================================================================
 * Response packing helpers (not exported by mem_protocol.h)
 * ======================================================================== */

/**
 * Pack an alloc response: status(i32) + offset(u64)  => 12 bytes
 */
static int pack_alloc_resp(int32_t status, uint64_t offset, UmmProtoBody *body)
{
    if (!body)
        return UMM_E_INVALID_ARG;
    memset(body->data, 0, sizeof(body->data));
    size_t p = 0;
    proto_write_i32(body->data, &p, status);
    proto_write_u64(body->data, &p, offset);
    body->len = (uint16_t)p;
    return UMM_OK;
}

/**
 * Phase 1: alloc_tiered 响应追加数据属主 node(u8) => 13 bytes。
 * 新客户端按 body_len 自适应（旧客户端忽略第 13 字节，无兼容性问题）。
 */
static int pack_alloc_resp2(int32_t status, uint64_t offset, uint8_t owner,
                            UmmProtoBody *body)
{
    if (!body)
        return UMM_E_INVALID_ARG;
    memset(body->data, 0, sizeof(body->data));
    size_t p = 0;
    proto_write_i32(body->data, &p, status);
    proto_write_u64(body->data, &p, offset);
    proto_write_u8(body->data, &p, owner);
    body->len = (uint16_t)p;
    return UMM_OK;
}

/**
 * Pack a free response: status(i32)  => 4 bytes
 */
static int pack_free_resp(int32_t status, UmmProtoBody *body)
{
    if (!body)
        return UMM_E_INVALID_ARG;
    memset(body->data, 0, sizeof(body->data));
    size_t p = 0;
    proto_write_i32(body->data, &p, status);
    body->len = (uint16_t)p;
    return UMM_OK;
}

/**
 * Pack a stats response: total(u64) + used(u64) + free(u64)  => 24 bytes
 */
static int pack_stats_resp(uint64_t total, uint64_t used, uint64_t free_mem,
                           UmmProtoBody *body)
{
    if (!body)
        return UMM_E_INVALID_ARG;
    memset(body->data, 0, sizeof(body->data));
    size_t p = 0;
    proto_write_u64(body->data, &p, total);
    proto_write_u64(body->data, &p, used);
    proto_write_u64(body->data, &p, free_mem);
    body->len = (uint16_t)p;
    return UMM_OK;
}

/**
 * Pack an empty heartbeat response (status = OK).
 */
static int pack_heartbeat_resp(UmmProtoBody *body)
{
    if (!body)
        return UMM_E_INVALID_ARG;
    memset(body->data, 0, sizeof(body->data));
    body->len = 0;
    return UMM_OK;
}

/* ========================================================================
 * Helper: build and send a complete response (header + body)
 * ======================================================================== */

static int send_response(int client_sock, uint8_t opcode, uint8_t flags,
                         const UmmProtoBody *body)
{
    UmmProtoHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, UMM_PROTO_MAGIC, 4);
    hdr.version  = UMM_PROTO_VERSION;
    hdr.opcode   = opcode;
    hdr.flags    = flags;
    hdr.body_len = body ? body->len : 0;

    int rc = umm_tcp_send(client_sock, &hdr, sizeof(hdr));
    if (rc != UMM_OK)
        return rc;

    if (body && body->len > 0) {
        rc = umm_tcp_send(client_sock, body->data, body->len);
        if (rc != UMM_OK)
            return rc;
    }

    return UMM_OK;
}

/* ========================================================================
 * Helper: read a complete request (header + body) from the client
 * ======================================================================== */

static int recv_request(int client_sock, UmmProtoHeader *hdr, UmmProtoBody *body)
{
    /* --- read header --- */
    int rc = umm_tcp_recv_exact(client_sock, hdr, sizeof(*hdr));
    if (rc != UMM_OK)
        return rc;

    /* --- validate header --- */
    if (memcmp(hdr->magic, UMM_PROTO_MAGIC, 4) != 0)
        return UMM_E_RPC_ERROR;
    if (hdr->version != UMM_PROTO_VERSION)
        return UMM_E_RPC_ERROR;
    if (hdr->body_len > UMM_PROTO_MAX_BODY)
        return UMM_E_RPC_ERROR;

    /* --- read body if present --- */
    memset(body, 0, sizeof(*body));
    if (hdr->body_len > 0) {
        rc = umm_tcp_recv_exact(client_sock, body->data, hdr->body_len);
        if (rc != UMM_OK)
            return rc;
    }
    body->len = hdr->body_len;

    return UMM_OK;
}

/* ========================================================================
 * Phase 1 数据面 helpers：payload 流式收发 + 校验
 * ======================================================================== */

/** 校验数据面请求参数；返回 UMM_OK 或错误码（同时作为响应 status）。 */
static int validate_data_req(gpa_t gpa, uint64_t len)
{
    if (gpa_to_tier(gpa) != UMM_TIER_SSD) {
        umm_log_warn("mem_rpc_server: data op on non-SSD tier %u rejected",
                     (unsigned)gpa_to_tier(gpa));
        return UMM_E_INVALID_ARG;
    }
    if (len == 0 || len > g_srv_data_max_io) {
        umm_log_warn("mem_rpc_server: data op len=%lu rejected "
                     "(cap=%u)", (unsigned long)len, g_srv_data_max_io);
        return UMM_E_INVALID_ARG;
    }
    return UMM_OK;
}

/**
 * DATA_READ：status(i32)+len(u64) 响应体，成功后跟随 len 字节 payload 流。
 */
static int handle_data_read(int client_sock, void *ctx,
                            MemoryServiceVtbl *vtbl,
                            const UmmProtoBody *body)
{
    gpa_t    gpa = 0;
    uint64_t len = 0;
    int32_t  svc_rc = mem_unpack_data_req(body, &gpa, &len);

    uint8_t *buf = NULL;
    if (svc_rc == UMM_OK)
        svc_rc = validate_data_req(gpa, len);

    if (svc_rc == UMM_OK) {
        if (!vtbl->ssd_read) {
            svc_rc = UMM_E_UNSUPPORTED;
        } else {
            buf = malloc((size_t)len);
            if (!buf) {
                svc_rc = UMM_E_NO_MEMORY;
            } else {
                svc_rc = vtbl->ssd_read(ctx, UMM_TIER_SSD, gpa_to_node(gpa),
                                        gpa_to_offset(gpa), len, buf);
            }
        }
    }

    /* 响应 body：status + len（len 仅成功时有意义） */
    UmmProtoBody resp;
    memset(&resp, 0, sizeof(resp));
    size_t p = 0;
    proto_write_i32(resp.data, &p, svc_rc);
    proto_write_u64(resp.data, &p, (svc_rc == UMM_OK) ? len : 0);
    resp.len = (uint16_t)p;

    int rc = send_response(client_sock, MEM_OP_DATA_READ,
                           UMM_FLAG_RESPONSE, &resp);
    if (rc == UMM_OK && svc_rc == UMM_OK && len > 0) {
        rc = umm_tcp_send(client_sock, buf, (size_t)len);
        if (rc != UMM_OK)
            svc_rc = rc;
    }

    free(buf);
    /* 业务错误（NOT_FOUND / INVALID_ARG…）已随响应体 status 返回客户端；
     * 只有传输层失败（响应没发出去 / payload 没发完）才返回非 0 让上层
     * 关闭连接——否则客户端收到合法错误响应后连接也被断开，后续 RPC
     * 全部 -7（共享池负路径实测复现）。 */
    return rc;
}

/**
 * DATA_WRITE：请求体后跟随 len 字节 payload 流，先收齐再落盘。
 */
static int handle_data_write(int client_sock, void *ctx,
                             MemoryServiceVtbl *vtbl,
                             const UmmProtoBody *body)
{
    gpa_t    gpa = 0;
    uint64_t len = 0;
    int32_t  svc_rc = mem_unpack_data_req(body, &gpa, &len);

    if (svc_rc == UMM_OK)
        svc_rc = validate_data_req(gpa, len);

    uint8_t *buf = NULL;
    if (svc_rc == UMM_OK) {
        if (!vtbl->ssd_write) {
            svc_rc = UMM_E_UNSUPPORTED;
        } else {
            buf = malloc((size_t)len);
            if (!buf)
                svc_rc = UMM_E_NO_MEMORY;
        }
    }

    /* 参数非法时不存在 payload（client 协议约定：非法帧不发 payload），
     * 直接回错误响应即可，连接保持可继续复用。 */
    if (svc_rc == UMM_OK) {
        int rc = umm_tcp_recv_exact(client_sock, buf, (size_t)len);
        if (rc != UMM_OK) {
            free(buf);
            return rc;   /* 半包/断连：连接不可复用，交给上层关闭 */
        }
        svc_rc = vtbl->ssd_write(ctx, UMM_TIER_SSD, gpa_to_node(gpa),
                                 gpa_to_offset(gpa), len, buf);
        free(buf);
    }

    UmmProtoBody resp;
    memset(&resp, 0, sizeof(resp));
    size_t p = 0;
    proto_write_i32(resp.data, &p, svc_rc);
    resp.len = (uint16_t)p;

    int rc = send_response(client_sock, MEM_OP_DATA_WRITE,
                           UMM_FLAG_RESPONSE, &resp);
    return (rc != UMM_OK) ? rc : svc_rc;
}

/* ========================================================================
 * Public dispatcher
 * ======================================================================== */

int mem_service_rpc_handle(int client_sock, void *ctx, MemoryServiceVtbl *vtbl)
{
    if (client_sock < 0 || !ctx || !vtbl)
        return UMM_E_INVALID_ARG;

    UmmProtoHeader hdr;
    UmmProtoBody   body;
    UmmProtoBody   resp_body;

    int rc = recv_request(client_sock, &hdr, &body);
    if (rc != UMM_OK)
        return rc;

    /* Phase 1 鉴权：服务端配置 token 后逐请求校验 header.reserved[6]。
     * 校验失败：记日志并断开（不回响应——不给未授权方任何协议信息）。 */
    if (g_srv_token_on &&
        memcmp(hdr.reserved, g_srv_token, sizeof(g_srv_token)) != 0) {
        umm_log_warn("mem_rpc_server: token mismatch on op=%u, "
                     "closing connection", (unsigned)hdr.opcode);
        return UMM_E_RPC_ERROR;
    }

    uint8_t opcode = hdr.opcode;

    /* 数据面 op 走独立 handler（流式 payload，不经 4KB body） */
    if (opcode == MEM_OP_DATA_READ)
        return handle_data_read(client_sock, ctx, vtbl, &body);
    if (opcode == MEM_OP_DATA_WRITE)
        return handle_data_write(client_sock, ctx, vtbl, &body);

    int svc_rc = UMM_OK;
    memset(&resp_body, 0, sizeof(resp_body));

    switch (opcode) {
    case MEM_OP_ALLOC: {
        MemAllocReq req;
        svc_rc = mem_unpack_alloc(&body, &req);
        if (svc_rc == UMM_OK) {
            uint64_t offset = 0;
            svc_rc = vtbl->alloc_local(ctx, req.size, &offset);
            pack_alloc_resp((int32_t)svc_rc, offset, &resp_body);
        } else {
            pack_alloc_resp((int32_t)svc_rc, 0, &resp_body);
        }
        break;
    }

    case MEM_OP_FREE: {
        MemFreeReq req;
        svc_rc = mem_unpack_free(&body, &req);
        if (svc_rc == UMM_OK) {
            svc_rc = vtbl->free_local(ctx, req.offset, req.size);
        }
        pack_free_resp((int32_t)svc_rc, &resp_body);
        break;
    }

    case MEM_OP_GET_STATS: {
        uint64_t total = 0, used = 0, free_mem = 0;
        svc_rc = vtbl->get_stats(ctx, &total, &used, &free_mem);
        pack_stats_resp(total, used, free_mem, &resp_body);
        break;
    }

    case MEM_OP_HEARTBEAT: {
        svc_rc = UMM_OK;
        pack_heartbeat_resp(&resp_body);
        break;
    }

    case MEM_OP_GET_TOPOLOGY: {
        StorageTopology topo = {0};
        uint32_t start = 0;
        if (body.len != 0 && body.len != 4) {
            svc_rc = UMM_E_INVALID_ARG;
        } else {
            if (body.len == 4) {
                size_t p = 0;
                start = proto_read_u32(body.data, &p);
            }
            svc_rc = vtbl->get_topology ? vtbl->get_topology(ctx, &topo)
                                        : UMM_E_UNSUPPORTED;
            if (svc_rc == UMM_OK) {
                if (start > topo.num_resources) {
                    svc_rc = UMM_E_INVALID_ARG;
                } else {
                    topo.num_resources -= start;
                    memmove(topo.resources, topo.resources + start,
                            topo.num_resources * sizeof(StorageResource));
                    svc_rc = mem_pack_get_topology_resp(&topo, &resp_body);
                }
            }
        }
        if (svc_rc != UMM_OK) {
            size_t p = 0;
            proto_write_i32(resp_body.data, &p, svc_rc);
            proto_write_u32(resp_body.data, &p, g_srv_node_id);
            proto_write_u32(resp_body.data, &p, 0);
            resp_body.len = (uint16_t)p;
        }
        break;
    }

    case MEM_OP_ALLOC_ON_DEVICE: {
        MemAllocOnDeviceReq req;
        uint64_t offset = 0;
        svc_rc = mem_unpack_alloc_on_device(&body, &req);
        if (svc_rc == UMM_OK) {
            if (req.flags != 0)
                svc_rc = UMM_E_INVALID_ARG;
            else if (vtbl->alloc_on_device)
                svc_rc = vtbl->alloc_on_device(ctx, req.tier, req.device_idx,
                                               req.size, &offset);
            else
                svc_rc = UMM_E_UNSUPPORTED;
        }
        pack_alloc_resp2(svc_rc, offset, g_srv_node_id, &resp_body);
        break;
    }

    case MEM_OP_ALLOC_TIERED: {
        MemAllocTieredReq req;
        svc_rc = mem_unpack_alloc_tiered(&body, &req);
        if (svc_rc == UMM_OK) {
            uint64_t offset = 0;
            if (vtbl->alloc_tiered) {
                svc_rc = vtbl->alloc_tiered(ctx, req.tier, req.size, &offset);
            } else {
                /* Fallback to alloc_local for backward compatibility */
                if (req.tier == UMM_TIER_CXL) {
                    svc_rc = vtbl->alloc_local(ctx, req.size, &offset);
                } else {
                    svc_rc = UMM_E_INVALID_ARG;
                }
            }
            /* Phase 1：响应追加数据属主 node（本 umms 的 node_id） */
            pack_alloc_resp2((int32_t)svc_rc, offset, g_srv_node_id,
                             &resp_body);
        } else {
            pack_alloc_resp2((int32_t)svc_rc, 0, g_srv_node_id, &resp_body);
        }
        break;
    }

    case MEM_OP_FREE_TIERED: {
        MemFreeTieredReq req;
        svc_rc = mem_unpack_free_tiered(&body, &req);
        if (svc_rc == UMM_OK) {
            if (vtbl->free_tiered) {
                svc_rc = vtbl->free_tiered(ctx, req.tier, req.offset, req.size);
            } else {
                /* Fallback to free_local for backward compatibility */
                if (req.tier == UMM_TIER_CXL) {
                    svc_rc = vtbl->free_local(ctx, req.offset, req.size);
                } else {
                    svc_rc = UMM_E_INVALID_ARG;
                }
            }
        }
        pack_free_resp((int32_t)svc_rc, &resp_body);
        break;
    }

    default:
        svc_rc = UMM_E_UNKNOWN;
        resp_body.len = 0;
        break;
    }

    /* Send response with RESPONSE flag */
    rc = send_response(client_sock, opcode, UMM_FLAG_RESPONSE, &resp_body);
    if (rc != UMM_OK)
        return rc;

    /* 业务错误（NO_MEMORY/重复释放/参数非法…）已随响应体返回客户端；
     * 只有传输层失败才返回非 0 让连接关闭——此前业务错误也原样
     * return，服务端会把正常客户端连接一并断开（共享池负路径实测：
     * alloc 超额得到 -3 响应后连接被关，后续请求全部 -7） */
    return UMM_OK;
}
