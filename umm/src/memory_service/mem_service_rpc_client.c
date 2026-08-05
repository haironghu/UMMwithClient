#include "../common/types.h"
#include "../common/error_codes.h"
#include "../common/umm_network.h"
#include "../protocol/mem_protocol.h"
#include "../protocol/protocol_common.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

/* ========================================================================
 * MemRpcClient definition
 * ======================================================================== */

typedef struct {
    char      host[256];
    int       port;
    int       sock;
    pthread_mutex_t lock;
    /* Phase 1 鉴权：header.reserved[6] 填充的 FNV-1a 摘要（token_on=0 时全 0） */
    int       token_on;
    uint8_t   token[6];
} MemRpcClient;

/* Phase 1 扩展接口前向声明（定义见本文件下部） */
int mem_rpc_client_init_ex(MemRpcClient *client, const char *host, int port,
                           const char *rpc_token);
int mem_rpc_alloc_tiered2(MemRpcClient *c, tier_id_t tier, uint64_t size,
                          uint32_t flags, uint64_t *out_offset,
                          uint8_t *out_owner);

/* ========================================================================
 * Internal helpers
 * ======================================================================== */

/**
 * Connect (or reconnect) the underlying TCP socket.
 */
static int memrpc_ensure_connected(MemRpcClient *c)
{
    if (c->sock >= 0)
        return UMM_OK;

    int new_sock = -1;
    int rc = umm_tcp_connect(c->host, c->port, &new_sock);
    if (rc != UMM_OK)
        return UMM_E_TRANSPORT_ERROR;

    c->sock = new_sock;
    return UMM_OK;
}

/**
 * Close the socket (used on error to force reconnection next time).
 */
static void memrpc_disconnect(MemRpcClient *c)
{
    if (c->sock >= 0) {
        umm_tcp_close(c->sock);
        c->sock = -1;
    }
}

/**
 * Send a request and wait for the response.
 *
 * Lock must be held by caller.
 */
static int memrpc_do_call(MemRpcClient *c, uint8_t opcode,
                          const UmmProtoBody *req_body,
                          UmmProtoHeader *out_hdr,
                          UmmProtoBody   *out_body)
{
    /* --- ensure connected --- */
    int rc = memrpc_ensure_connected(c);
    if (rc != UMM_OK)
        return rc;

    /* --- build and send header --- */
    UmmProtoHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, UMM_PROTO_MAGIC, 4);
    hdr.version  = UMM_PROTO_VERSION;
    hdr.opcode   = opcode;
    hdr.flags    = UMM_FLAG_REQUEST;
    hdr.body_len = req_body ? req_body->len : 0;
    if (c->token_on)
        memcpy(hdr.reserved, c->token, sizeof(hdr.reserved));

    rc = umm_tcp_send(c->sock, &hdr, sizeof(hdr));
    if (rc != UMM_OK) {
        memrpc_disconnect(c);
        return UMM_E_TRANSPORT_ERROR;
    }

    /* --- send body if present --- */
    if (req_body && req_body->len > 0) {
        rc = umm_tcp_send(c->sock, req_body->data, req_body->len);
        if (rc != UMM_OK) {
            memrpc_disconnect(c);
            return UMM_E_TRANSPORT_ERROR;
        }
    }

    /* --- receive response header --- */
    rc = umm_tcp_recv_exact(c->sock, out_hdr, sizeof(*out_hdr));
    if (rc != UMM_OK) {
        memrpc_disconnect(c);
        return UMM_E_TRANSPORT_ERROR;
    }

    /* --- validate response header --- */
    if (memcmp(out_hdr->magic, UMM_PROTO_MAGIC, 4) != 0) {
        memrpc_disconnect(c);
        return UMM_E_RPC_ERROR;
    }
    if (out_hdr->version != UMM_PROTO_VERSION) {
        memrpc_disconnect(c);
        return UMM_E_RPC_ERROR;
    }
    if (out_hdr->body_len > UMM_PROTO_MAX_BODY) {
        memrpc_disconnect(c);
        return UMM_E_RPC_ERROR;
    }

    /* --- receive response body --- */
    memset(out_body, 0, sizeof(*out_body));
    if (out_hdr->body_len > 0) {
        rc = umm_tcp_recv_exact(c->sock, out_body->data, out_hdr->body_len);
        if (rc != UMM_OK) {
            memrpc_disconnect(c);
            return UMM_E_TRANSPORT_ERROR;
        }
    }
    out_body->len = out_hdr->body_len;

    return UMM_OK;
}

/* ========================================================================
 * Public API
 * ======================================================================== */

int mem_rpc_client_init(MemRpcClient *client, const char *host, int port)
{
    return mem_rpc_client_init_ex(client, host, port, NULL);
}

int mem_rpc_client_init_ex(MemRpcClient *client, const char *host, int port,
                           const char *rpc_token)
{
    if (!client || !host || port <= 0 || port > 65535)
        return UMM_E_INVALID_ARG;

    memset(client, 0, sizeof(*client));

    size_t hlen = strlen(host);
    if (hlen >= sizeof(client->host))
        hlen = sizeof(client->host) - 1;
    memcpy(client->host, host, hlen);
    client->host[hlen] = '\0';

    client->port = port;
    client->sock = -1;
    pthread_mutex_init(&client->lock, NULL);

    if (rpc_token && rpc_token[0] != '\0') {
        umm_token_digest(rpc_token, client->token);
        client->token_on = 1;
    }

    return UMM_OK;
}

void mem_rpc_client_deinit(MemRpcClient *client)
{
    if (!client)
        return;

    pthread_mutex_lock(&client->lock);

    if (client->sock >= 0) {
        umm_tcp_close(client->sock);
        client->sock = -1;
    }

    pthread_mutex_unlock(&client->lock);
    pthread_mutex_destroy(&client->lock);
}

int mem_rpc_alloc(MemRpcClient *c, uint64_t size, uint32_t flags,
                  uint64_t *out_offset)
{
    if (!c || !out_offset || size == 0)
        return UMM_E_INVALID_ARG;

    pthread_mutex_lock(&c->lock);

    /* Pack request */
    UmmProtoBody req_body;
    int rc = mem_pack_alloc(size, flags, &req_body);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    /* Send and receive */
    UmmProtoHeader resp_hdr;
    UmmProtoBody   resp_body;
    rc = memrpc_do_call(c, MEM_OP_ALLOC, &req_body, &resp_hdr, &resp_body);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    /* Unpack response */
    MemAllocResp resp;
    rc = mem_unpack_alloc_resp(&resp_body, &resp);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    if (resp.status != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return resp.status;
    }

    *out_offset = resp.offset;

    pthread_mutex_unlock(&c->lock);
    return UMM_OK;
}

int mem_rpc_free(MemRpcClient *c, uint64_t offset, uint64_t size)
{
    if (!c || size == 0)
        return UMM_E_INVALID_ARG;

    pthread_mutex_lock(&c->lock);

    /* Pack request */
    UmmProtoBody req_body;
    int rc = mem_pack_free(offset, size, &req_body);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    /* Send and receive */
    UmmProtoHeader resp_hdr;
    UmmProtoBody   resp_body;
    rc = memrpc_do_call(c, MEM_OP_FREE, &req_body, &resp_hdr, &resp_body);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    /* Unpack response */
    MemFreeResp resp;
    rc = mem_unpack_free_resp(&resp_body, &resp);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    pthread_mutex_unlock(&c->lock);
    return resp.status;
}

int mem_rpc_get_stats(MemRpcClient *c, uint64_t *total, uint64_t *used,
                      uint64_t *free_mem)
{
    if (!c || !total || !used || !free_mem)
        return UMM_E_INVALID_ARG;

    pthread_mutex_lock(&c->lock);

    /* Pack request (empty body for GET_STATS) */
    UmmProtoBody req_body;
    int rc = mem_pack_get_stats(&req_body);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    /* Send and receive */
    UmmProtoHeader resp_hdr;
    UmmProtoBody   resp_body;
    rc = memrpc_do_call(c, MEM_OP_GET_STATS, &req_body, &resp_hdr, &resp_body);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    /* Unpack response */
    MemStatsResp resp;
    rc = mem_unpack_stats_resp(&resp_body, &resp);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    *total    = resp.total;
    *used     = resp.used;
    *free_mem = resp.free;

    pthread_mutex_unlock(&c->lock);
    return UMM_OK;
}

/* ======================================================================== */
/* Phase 5: Storage topology query (MEM_OP_GET_TOPOLOGY = 9)               */
/* ======================================================================== */

/* ======================================================================== */
/* Tier-aware allocation (MEM_OP_ALLOC_TIERED = 5)                         */
/* ======================================================================== */

int mem_rpc_alloc_tiered(MemRpcClient *c, tier_id_t tier, uint64_t size,
                         uint32_t flags, uint64_t *out_offset)
{
    return mem_rpc_alloc_tiered2(c, tier, size, flags, out_offset, NULL);
}

int mem_rpc_alloc_tiered2(MemRpcClient *c, tier_id_t tier, uint64_t size,
                          uint32_t flags, uint64_t *out_offset,
                          uint8_t *out_owner)
{
    if (!c || !out_offset || size == 0 || tier >= UMM_NUM_TIERS)
        return UMM_E_INVALID_ARG;

    if (out_owner)
        *out_owner = UMM_NODE_UNKNOWN;

    pthread_mutex_lock(&c->lock);

    UmmProtoBody req_body;
    int rc = mem_pack_alloc_tiered((uint8_t)tier, size, flags, &req_body);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    UmmProtoHeader resp_hdr;
    UmmProtoBody   resp_body;
    rc = memrpc_do_call(c, MEM_OP_ALLOC_TIERED, &req_body, &resp_hdr, &resp_body);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    MemAllocTieredResp resp;
    rc = mem_unpack_alloc_tiered_resp2(&resp_body, &resp, out_owner);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    if (resp.status != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return resp.status;
    }

    *out_offset = resp.offset;
    pthread_mutex_unlock(&c->lock);
    return UMM_OK;
}

/* ======================================================================== */
/* Tier-aware free (MEM_OP_FREE_TIERED = 6)                                */
/* ======================================================================== */

int mem_rpc_free_tiered(MemRpcClient *c, tier_id_t tier, uint64_t offset,
                        uint64_t size)
{
    if (!c || size == 0 || tier >= UMM_NUM_TIERS)
        return UMM_E_INVALID_ARG;

    pthread_mutex_lock(&c->lock);

    UmmProtoBody req_body;
    int rc = mem_pack_free_tiered((uint8_t)tier, offset, size, &req_body);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    UmmProtoHeader resp_hdr;
    UmmProtoBody   resp_body;
    rc = memrpc_do_call(c, MEM_OP_FREE_TIERED, &req_body, &resp_hdr, &resp_body);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    /* Free response: just status (i32) */
    if (resp_body.len >= 4) {
        size_t pos = 0;
        int32_t status = proto_read_i32(resp_body.data, &pos);
        rc = status;
    } else {
        rc = UMM_OK;
    }

    pthread_mutex_unlock(&c->lock);
    return rc;
}

/* ======================================================================== */
/* Phase 5: Storage topology query (MEM_OP_GET_TOPOLOGY = 9)               */
/* ======================================================================== */

int mem_rpc_get_topology(MemRpcClient *c, StorageTopology *out)
{
    if (!c || !out)
        return UMM_E_INVALID_ARG;

    memset(out, 0, sizeof(StorageTopology));

    pthread_mutex_lock(&c->lock);

    /* Pack request (empty body for GET_TOPOLOGY) */
    UmmProtoBody req_body;
    int rc = mem_pack_get_topology(&req_body);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    /* Send and receive */
    UmmProtoHeader resp_hdr;
    UmmProtoBody   resp_body;
    rc = memrpc_do_call(c, MEM_OP_GET_TOPOLOGY, &req_body, &resp_hdr, &resp_body);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    /* Unpack response */
    rc = mem_unpack_get_topology_resp(&resp_body, out);

    pthread_mutex_unlock(&c->lock);
    return rc;
}
