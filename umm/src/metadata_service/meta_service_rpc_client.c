/* ========================================================================
 * meta_service_rpc_client.c — Metadata service RPC client (v2.0)
 *
 * Flat chunk namespace.  No Region concept.
 * ======================================================================== */

#include "meta_service.h"
#include "../protocol/meta_protocol.h"
#include "../protocol/protocol_common.h"
#include "../common/types.h"
#include "../common/error_codes.h"
#include "../common/log.h"
#include "../common/umm_network.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

/* ==================================================================== */
/* Client state                                                         */
/* ==================================================================== */

typedef struct MetaRpcClient {
    char host[256];
    int  port;
    int  sock;              /* -1 if not connected */
    pthread_mutex_t lock;
} MetaRpcClient;

/* ==================================================================== */
/* Internal helpers                                                     */
/* ==================================================================== */

/**
 * Ensure the client has an open TCP connection.
 */
static int ensure_connected(MetaRpcClient *c)
{
    if (c->sock >= 0)
        return UMM_OK;

    int rc = umm_tcp_connect(c->host, c->port, &c->sock);
    if (rc != UMM_OK) {
        c->sock = -1;
        return UMM_E_TRANSPORT_ERROR;
    }
    return UMM_OK;
}

/**
 * Close the socket (called on errors to force reconnection next time).
 */
static void disconnect(MetaRpcClient *c)
{
    if (c->sock >= 0) {
        umm_tcp_close(c->sock);
        c->sock = -1;
    }
}

/**
 * Send a request and receive its response.
 * Caller must hold c->lock.
 */
static int do_rpc(MetaRpcClient *c, uint8_t opcode,
                  const UmmProtoBody *req_body,
                  UmmProtoBody *resp_body)
{
    int rc = ensure_connected(c);
    if (rc != UMM_OK)
        return rc;

    /* --- build & send header --- */
    UmmProtoHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, UMM_PROTO_MAGIC, 4);
    hdr.version  = UMM_PROTO_VERSION;
    hdr.opcode   = opcode;
    hdr.flags    = UMM_FLAG_REQUEST;
    hdr.body_len = req_body ? req_body->len : 0;

    rc = umm_tcp_send(c->sock, &hdr, sizeof(hdr));
    if (rc != UMM_OK) {
        disconnect(c);
        return UMM_E_TRANSPORT_ERROR;
    }

    /* --- send body --- */
    if (req_body && req_body->len > 0) {
        rc = umm_tcp_send(c->sock, req_body->data, req_body->len);
        if (rc != UMM_OK) {
            disconnect(c);
            return UMM_E_TRANSPORT_ERROR;
        }
    }

    /* --- receive response header --- */
    UmmProtoHeader resp_hdr;
    rc = umm_tcp_recv_exact(c->sock, &resp_hdr, sizeof(resp_hdr));
    if (rc != UMM_OK) {
        disconnect(c);
        return UMM_E_TRANSPORT_ERROR;
    }

    /* --- validate response header --- */
    if (memcmp(resp_hdr.magic, UMM_PROTO_MAGIC, 4) != 0 ||
        resp_hdr.version != UMM_PROTO_VERSION ||
        resp_hdr.opcode != opcode) {
        disconnect(c);
        return UMM_E_RPC_ERROR;
    }

    /* --- receive response body --- */
    memset(resp_body, 0, sizeof(*resp_body));
    if (resp_hdr.body_len > 0) {
        if (resp_hdr.body_len > UMM_PROTO_MAX_BODY) {
            disconnect(c);
            return UMM_E_RPC_ERROR;
        }
        rc = umm_tcp_recv_exact(c->sock, resp_body->data, resp_hdr.body_len);
        if (rc != UMM_OK) {
            disconnect(c);
            return UMM_E_TRANSPORT_ERROR;
        }
        resp_body->len = resp_hdr.body_len;
    }

    return UMM_OK;
}

/* ==================================================================== */
/* Public API                                                           */
/* ==================================================================== */

int meta_rpc_client_init(MetaRpcClient *client, const char *host, int port)
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

    if (pthread_mutex_init(&client->lock, NULL) != 0)
        return UMM_E_UNKNOWN;

    return UMM_OK;
}

void meta_rpc_client_deinit(MetaRpcClient *client)
{
    if (!client)
        return;

    pthread_mutex_lock(&client->lock);
    disconnect(client);
    pthread_mutex_unlock(&client->lock);

    pthread_mutex_destroy(&client->lock);
}

/* ==================================================================== */
/* Chunk RPC methods                                                    */
/* ==================================================================== */

int meta_rpc_register_chunk(MetaRpcClient *c, const char *name,
                            gpa_t gpa, uint64_t size, chunk_id_t *out)
{
    if (!c || !name || !out)
        return UMM_E_INVALID_ARG;

    pthread_mutex_lock(&c->lock);

    UmmProtoBody req_body;
    int rc = meta_pack_register_chunk(name, gpa, size, &req_body);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    UmmProtoBody resp_body;
    rc = do_rpc(c, META_OP_REGISTER_CHUNK, &req_body, &resp_body);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    MetaRegisterChunkResp resp;
    rc = meta_unpack_register_chunk_resp(&resp_body, &resp);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    if (resp.status == UMM_OK)
        *out = resp.chunk_id;

    pthread_mutex_unlock(&c->lock);
    return resp.status;
}

int meta_rpc_lookup_chunk(MetaRpcClient *c, const char *name, ChunkMetadata *out)
{
    if (!c || !name || !out)
        return UMM_E_INVALID_ARG;

    pthread_mutex_lock(&c->lock);

    UmmProtoBody req_body;
    int rc = meta_pack_lookup_chunk(name, &req_body);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    UmmProtoBody resp_body;
    rc = do_rpc(c, META_OP_LOOKUP_CHUNK, &req_body, &resp_body);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    MetaLookupChunkResp resp;
    rc = meta_unpack_lookup_chunk_resp(&resp_body, &resp);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    if (resp.status == UMM_OK)
        *out = resp.meta;

    pthread_mutex_unlock(&c->lock);
    return resp.status;
}

int meta_rpc_lookup_chunk_by_id(MetaRpcClient *c, chunk_id_t chunk_id,
                                ChunkMetadata *out)
{
    if (!c || !out)
        return UMM_E_INVALID_ARG;

    pthread_mutex_lock(&c->lock);

    UmmProtoBody req_body;
    int rc = meta_pack_lookup_chunk_by_id(chunk_id, &req_body);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    UmmProtoBody resp_body;
    rc = do_rpc(c, META_OP_LOOKUP_CHUNK_BY_ID, &req_body, &resp_body);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    MetaLookupChunkByIdResp resp;
    rc = meta_unpack_lookup_chunk_by_id_resp(&resp_body, &resp);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    if (resp.status == UMM_OK)
        *out = resp.meta;

    pthread_mutex_unlock(&c->lock);
    return resp.status;
}

int meta_rpc_unregister_chunk(MetaRpcClient *c, chunk_id_t chunk_id)
{
    if (!c)
        return UMM_E_INVALID_ARG;

    pthread_mutex_lock(&c->lock);

    UmmProtoBody req_body;
    int rc = meta_pack_unregister_chunk(chunk_id, &req_body);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    UmmProtoBody resp_body;
    rc = do_rpc(c, META_OP_UNREGISTER_CHUNK, &req_body, &resp_body);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    MetaUnregisterChunkResp resp;
    rc = meta_unpack_unregister_chunk_resp(&resp_body, &resp);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    pthread_mutex_unlock(&c->lock);
    return resp.status;
}

int meta_rpc_add_ref(MetaRpcClient *c, chunk_id_t chunk_id)
{
    if (!c)
        return UMM_E_INVALID_ARG;

    pthread_mutex_lock(&c->lock);

    UmmProtoBody req_body;
    int rc = meta_pack_add_ref(chunk_id, &req_body);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    UmmProtoBody resp_body;
    rc = do_rpc(c, META_OP_ADD_REF, &req_body, &resp_body);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    MetaAddRefResp resp;
    rc = meta_unpack_add_ref_resp(&resp_body, &resp);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    pthread_mutex_unlock(&c->lock);
    return resp.status;
}

int meta_rpc_release_ref(MetaRpcClient *c, chunk_id_t chunk_id)
{
    if (!c)
        return UMM_E_INVALID_ARG;

    pthread_mutex_lock(&c->lock);

    UmmProtoBody req_body;
    int rc = meta_pack_release_ref(chunk_id, &req_body);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    UmmProtoBody resp_body;
    rc = do_rpc(c, META_OP_RELEASE_REF, &req_body, &resp_body);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    MetaReleaseRefResp resp;
    rc = meta_unpack_release_ref_resp(&resp_body, &resp);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    pthread_mutex_unlock(&c->lock);
    return resp.status;
}

int meta_rpc_list_chunks_by_node(MetaRpcClient *c, node_id_t node,
                                 ChunkMetadata *out_array, uint32_t *inout_count)
{
    if (!c || !inout_count)
        return UMM_E_INVALID_ARG;

    pthread_mutex_lock(&c->lock);

    UmmProtoBody req_body;
    int rc = meta_pack_list_chunks(node, &req_body);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    UmmProtoBody resp_body;
    rc = do_rpc(c, META_OP_LIST_CHUNKS_BY_NODE, &req_body, &resp_body);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    uint32_t returned_count = *inout_count;
    rc = meta_unpack_list_chunks_resp(&resp_body, out_array, returned_count,
                                      inout_count);

    pthread_mutex_unlock(&c->lock);
    return rc;
}

/* ======================================================================== */
/* Phase 5: Storage topology query                                         */
/* ======================================================================== */

int meta_rpc_get_storage_topology(MetaRpcClient *c, node_id_t node,
                                   StorageTopology *out)
{
    if (!c || !out)
        return UMM_E_INVALID_ARG;

    memset(out, 0, sizeof(StorageTopology));

    pthread_mutex_lock(&c->lock);

    /* Pack request */
    UmmProtoBody req_body;
    int rc = meta_pack_get_storage_topology(node, &req_body);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    /* Send RPC */
    UmmProtoBody resp_body;
    rc = do_rpc(c, META_OP_GET_STORAGE_TOPOLOGY, &req_body, &resp_body);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    /* Unpack response */
    rc = meta_unpack_get_storage_topology_resp(&resp_body, out);

    pthread_mutex_unlock(&c->lock);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Storage resource registration (status-only response)                */
/* ------------------------------------------------------------------ */
int meta_rpc_register_storage_resource(MetaRpcClient *c, node_id_t node,
                                        const StorageResource *res)
{
    if (!c || !res)
        return UMM_E_INVALID_ARG;

    pthread_mutex_lock(&c->lock);

    UmmProtoBody req_body;
    int rc = meta_pack_register_storage_resource(node, res, &req_body);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    UmmProtoBody resp_body;
    rc = do_rpc(c, META_OP_REGISTER_STORAGE_RESOURCE, &req_body, &resp_body);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&c->lock);
        return rc;
    }

    /* status-only 响应体: i32 */
    size_t p = 0;
    int32_t status = proto_read_i32(resp_body.data, &p);

    pthread_mutex_unlock(&c->lock);
    return status;
}
