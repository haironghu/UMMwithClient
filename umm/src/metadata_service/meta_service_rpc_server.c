/* ========================================================================
 * meta_service_rpc_server.c — Metadata service RPC request dispatcher (v2.0)
 *
 * Flat chunk namespace.  No Region concept.
 * ======================================================================== */

#include "meta_service.h"
#include "../protocol/meta_protocol.h"
#include "../protocol/protocol_common.h"
#include "../common/types.h"
#include "../common/log.h"
#include "../common/error_codes.h"
#include "../common/umm_network.h"

#include <string.h>
#include <stdio.h>
#include <unistd.h>

/* ==================================================================== */
/* Helpers                                                              */
/* ==================================================================== */

/**
 * Build and send a complete response (header + optional body).
 */
static int send_response(int sock, uint8_t opcode, const UmmProtoBody *body)
{
    UmmProtoHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, UMM_PROTO_MAGIC, 4);
    hdr.version  = UMM_PROTO_VERSION;
    hdr.opcode   = opcode;
    hdr.flags    = UMM_FLAG_RESPONSE;
    hdr.body_len = body ? body->len : 0;

    int rc = umm_tcp_send(sock, &hdr, sizeof(hdr));
    if (rc != UMM_OK)
        return rc;

    if (body && body->len > 0) {
        rc = umm_tcp_send(sock, body->data, body->len);
        if (rc != UMM_OK)
            return rc;
    }
    return UMM_OK;
}

/**
 * Read a full request (header + body) from the client.
 */
static int recv_request(int client_sock, UmmProtoHeader *hdr, UmmProtoBody *body)
{
    int rc = umm_tcp_recv_exact(client_sock, hdr, sizeof(*hdr));
    if (rc != UMM_OK)
        return rc;

    if (memcmp(hdr->magic, UMM_PROTO_MAGIC, 4) != 0 ||
        hdr->version != UMM_PROTO_VERSION)
        return UMM_E_RPC_ERROR;

    if (hdr->body_len > UMM_PROTO_MAX_BODY)
        return UMM_E_RPC_ERROR;

    memset(body, 0, sizeof(*body));
    if (hdr->body_len > 0) {
        rc = umm_tcp_recv_exact(client_sock, body->data, hdr->body_len);
        if (rc != UMM_OK)
            return rc;
    }
    body->len = hdr->body_len;
    return UMM_OK;
}

/**
 * Pack a ChunkMetadata into a buffer at offset p.  Returns new offset.
 */
static size_t pack_chunk_metadata(uint8_t *buf, size_t p, const ChunkMetadata *m)
{
    proto_write_u64(buf, &p, m->chunk_id);
    proto_write_str(buf, &p, m->name, 64);
    proto_write_u64(buf, &p, m->gpa);
    proto_write_u64(buf, &p, m->size);
    return p;
}

/**
 * Pack a simple status-only body.
 */
static void pack_status_body(UmmProtoBody *body, int32_t status)
{
    memset(body->data, 0, sizeof(body->data));
    size_t p = 0;
    proto_write_i32(body->data, &p, status);
    body->len = (uint16_t)p;
}

/* ==================================================================== */
/* Public dispatcher                                                    */
/* ==================================================================== */

int meta_service_rpc_handle(int client_sock, void *ctx,
                            MetadataServiceVtbl *vtbl)
{
    if (client_sock < 0 || !ctx || !vtbl)
        return UMM_E_INVALID_ARG;

    UmmProtoHeader hdr;
    UmmProtoBody   req_body;
    UmmProtoBody   resp_body;

    int rc = recv_request(client_sock, &hdr, &req_body);
    if (rc != UMM_OK)
        return rc;

    int32_t status = UMM_OK;
    memset(&resp_body, 0, sizeof(resp_body));

    switch (hdr.opcode) {

    /* ---------------------------------------------------------------- */
    /* META_OP_REGISTER_CHUNK (1)                                       */
    /*   Req:  name(64) + gpa(u64) + size(u64)            = 80 bytes    */
    /*   Resp: status(i32) + chunk_id(u64)                = 12 bytes    */
    /* ---------------------------------------------------------------- */
    case META_OP_REGISTER_CHUNK: {
        MetaRegisterChunkReq req;
        rc = meta_unpack_register_chunk(&req_body, &req);
        if (rc != UMM_OK) {
            status = rc;
            pack_status_body(&resp_body, status);
            break;
        }

        chunk_id_t cid = 0;
        status = vtbl->register_chunk(ctx, req.name, req.gpa, req.size, &cid);

        umm_log_info("ummd: REGISTER '%s' gpa=0x%lx size=%lu -> status=%d cid=%lu",
                     req.name, (unsigned long)req.gpa,
                     (unsigned long)req.size, status, (unsigned long)cid);

        memset(resp_body.data, 0, sizeof(resp_body.data));
        size_t p = 0;
        proto_write_i32(resp_body.data, &p, status);
        proto_write_u64(resp_body.data, &p, cid);
        resp_body.len = (uint16_t)p;
        break;
    }

    /* ---------------------------------------------------------------- */
    /* META_OP_LOOKUP_CHUNK (2)                                         */
    /*   Req:  name(64)                                   = 64 bytes    */
    /*   Resp: status(i32) + ChunkMetadata                = 4 + 88      */
    /* ---------------------------------------------------------------- */
    case META_OP_LOOKUP_CHUNK: {
        MetaLookupChunkReq req;
        rc = meta_unpack_lookup_chunk(&req_body, &req);
        if (rc != UMM_OK) {
            status = rc;
            pack_status_body(&resp_body, status);
            break;
        }

        ChunkMetadata meta;
        memset(&meta, 0, sizeof(meta));
        status = vtbl->lookup_chunk(ctx, req.name, &meta);

        umm_log_info("ummd: LOOKUP '%s' -> status=%d (%s)",
                     req.name, status,
                     status == UMM_OK ? "FOUND" : "NOT_FOUND");

        memset(resp_body.data, 0, sizeof(resp_body.data));
        size_t p = 0;
        proto_write_i32(resp_body.data, &p, status);
        p = pack_chunk_metadata(resp_body.data, p, &meta);
        resp_body.len = (uint16_t)p;
        break;
    }

    /* ---------------------------------------------------------------- */
    /* META_OP_LOOKUP_CHUNK_BY_ID (3)                                   */
    /*   Req:  chunk_id(u64)                              = 8 bytes     */
    /*   Resp: status(i32) + ChunkMetadata                = 4 + 88      */
    /* ---------------------------------------------------------------- */
    case META_OP_LOOKUP_CHUNK_BY_ID: {
        MetaLookupChunkByIdReq req;
        rc = meta_unpack_lookup_chunk_by_id(&req_body, &req);
        if (rc != UMM_OK) {
            status = rc;
            pack_status_body(&resp_body, status);
            break;
        }

        ChunkMetadata meta;
        memset(&meta, 0, sizeof(meta));
        status = vtbl->lookup_chunk_by_id(ctx, req.chunk_id, &meta);

        memset(resp_body.data, 0, sizeof(resp_body.data));
        size_t p = 0;
        proto_write_i32(resp_body.data, &p, status);
        p = pack_chunk_metadata(resp_body.data, p, &meta);
        resp_body.len = (uint16_t)p;
        break;
    }

    /* ---------------------------------------------------------------- */
    /* META_OP_UNREGISTER_CHUNK (4)                                     */
    /*   Req:  chunk_id(u64)                              = 8 bytes     */
    /*   Resp: status(i32)                                = 4 bytes     */
    /* ---------------------------------------------------------------- */
    case META_OP_UNREGISTER_CHUNK: {
        MetaUnregisterChunkReq req;
        rc = meta_unpack_unregister_chunk(&req_body, &req);
        if (rc != UMM_OK) {
            status = rc;
            pack_status_body(&resp_body, status);
            break;
        }

        status = vtbl->unregister_chunk(ctx, req.chunk_id);
        pack_status_body(&resp_body, status);
        break;
    }

    /* ---------------------------------------------------------------- */
    /* META_OP_ADD_REF (5)                                              */
    /*   Req:  chunk_id(u64)                              = 8 bytes     */
    /*   Resp: status(i32) + ref_count(u32)               = 8 bytes     */
    /* ---------------------------------------------------------------- */
    case META_OP_ADD_REF: {
        chunk_id_t cid = 0;
        rc = meta_unpack_add_ref(&req_body, &cid);
        if (rc != UMM_OK) {
            status = rc;
            pack_status_body(&resp_body, status);
            break;
        }

        status = vtbl->add_ref(ctx, cid);

        memset(resp_body.data, 0, sizeof(resp_body.data));
        size_t p = 0;
        proto_write_i32(resp_body.data, &p, status);
        proto_write_u32(resp_body.data, &p, 0);   /* ref_count placeholder */
        resp_body.len = (uint16_t)p;
        break;
    }

    /* ---------------------------------------------------------------- */
    /* META_OP_RELEASE_REF (6)                                          */
    /*   Req:  chunk_id(u64)                              = 8 bytes     */
    /*   Resp: status(i32) + ref_count(u32)               = 8 bytes     */
    /* ---------------------------------------------------------------- */
    case META_OP_RELEASE_REF: {
        chunk_id_t cid = 0;
        rc = meta_unpack_release_ref(&req_body, &cid);
        if (rc != UMM_OK) {
            status = rc;
            pack_status_body(&resp_body, status);
            break;
        }

        status = vtbl->release_ref(ctx, cid);

        memset(resp_body.data, 0, sizeof(resp_body.data));
        size_t p = 0;
        proto_write_i32(resp_body.data, &p, status);
        proto_write_u32(resp_body.data, &p, 0);   /* ref_count placeholder */
        resp_body.len = (uint16_t)p;
        break;
    }

    /* ---------------------------------------------------------------- */
    /* META_OP_LIST_CHUNKS_BY_NODE (7)                                  */
    /*   Req:  node_id(u32)                               = 4 bytes     */
    /*   Resp: status(i32) + count(u32) + N*ChunkMetadata               */
    /* ---------------------------------------------------------------- */
    case META_OP_LIST_CHUNKS_BY_NODE: {
        MetaListChunksReq req;
        rc = meta_unpack_list_chunks(&req_body, &req);
        if (rc != UMM_OK) {
            status = rc;
            pack_status_body(&resp_body, status);
            break;
        }

        /* Temporary buffer on stack for returned chunks */
        ChunkMetadata tmp_metas[32];
        uint32_t count = 32;
        memset(tmp_metas, 0, sizeof(tmp_metas));
        status = vtbl->list_chunks_by_node(ctx, req.node, tmp_metas, &count);

        if (status != UMM_OK) {
            pack_status_body(&resp_body, status);
            break;
        }

        meta_pack_list_chunks_resp(tmp_metas, count, &resp_body);
        break;
    }

    /* ---------------------------------------------------------------- */
    /* META_OP_HEARTBEAT (8)                                            */
    /*   Req:  (empty)                                                  */
    /*   Resp: (empty OK)                                               */
    /* ---------------------------------------------------------------- */
    case META_OP_HEARTBEAT: {
        resp_body.len = 0;
        break;
    }

    /* ---------------------------------------------------------------- */
    /* META_OP_REGISTER_STORAGE_RESOURCE (9)                            */
    /*   Req:  node(u32) + StorageResource                              */
    /*   Resp: status(i32)                                = 4 bytes     */
    /* ---------------------------------------------------------------- */
    case META_OP_REGISTER_STORAGE_RESOURCE: {
        MetaRegisterStorageReq req;
        rc = meta_unpack_register_storage_resource(&req_body, &req);
        if (rc != UMM_OK) {
            status = rc;
            pack_status_body(&resp_body, status);
            break;
        }

        status = vtbl->register_storage_resource(ctx, req.node, &req.res);
        pack_status_body(&resp_body, status);
        break;
    }

    /* ---------------------------------------------------------------- */
    /* META_OP_GET_STORAGE_TOPOLOGY (10)                                */
    /*   Req:  node(u32)                                  = 4 bytes     */
    /*   Resp: status(i32) + StorageTopology                            */
    /* ---------------------------------------------------------------- */
    case META_OP_GET_STORAGE_TOPOLOGY: {
        MetaGetTopologyReq req;
        rc = meta_unpack_get_storage_topology(&req_body, &req);
        if (rc != UMM_OK) {
            status = rc;
            pack_status_body(&resp_body, status);
            break;
        }

        StorageTopology topo;
        memset(&topo, 0, sizeof(topo));
        status = vtbl->get_storage_topology(ctx, req.node, &topo);

        if (status != UMM_OK) {
            pack_status_body(&resp_body, status);
        } else {
            meta_pack_get_storage_topology_resp(&topo, &resp_body);
        }
        break;
    }

    default: {
        fprintf(stderr,
                "[WARN] meta_service_rpc_server: unknown opcode %u\n",
                hdr.opcode);
        pack_status_body(&resp_body, UMM_E_UNKNOWN);
        break;
    }
    }

    /* ---- Send response ---- */
    return send_response(client_sock, hdr.opcode, &resp_body);
}
