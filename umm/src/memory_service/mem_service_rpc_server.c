#include "mem_service.h"

#include "../protocol/mem_protocol.h"
#include "../protocol/protocol_common.h"
#include "../common/types.h"
#include "../common/error_codes.h"
#include "../common/umm_network.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

    uint8_t opcode = hdr.opcode;

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
            pack_alloc_resp((int32_t)svc_rc, offset, &resp_body);
        } else {
            pack_alloc_resp((int32_t)svc_rc, 0, &resp_body);
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

    return svc_rc;
}
