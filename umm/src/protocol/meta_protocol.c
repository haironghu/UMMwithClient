/* ========================================================================
 * meta_protocol.c — Metadata service RPC protocol (v2.0)
 *
 * Flat chunk directory. No Region.
 * ======================================================================== */

#include "meta_protocol.h"
#include "protocol_common.h"
#include <string.h>

/* ==================================================================== */
/* Helper: pack/unpack a single StorageResource                         */
/* ==================================================================== */

#define STORAGE_RESOURCE_SERIALIZED_SIZE 274  /* 1 + 256 + 8 + 8 + 1 */
#define STORAGE_TOPOLOGY_HEADER_SIZE 12       /* 4 + 4 + 4 */

static size_t pack_storage_resource(uint8_t *buf, size_t p, const StorageResource *r)
{
    proto_write_u8 (buf, &p, r->tier);
    proto_write_str(buf, &p, r->device_path, 256);
    proto_write_u64(buf, &p, r->capacity);
    proto_write_u64(buf, &p, r->base_offset);
    proto_write_u8 (buf, &p, (uint8_t)r->online);
    return p;
}

static size_t unpack_storage_resource(const uint8_t *buf, size_t p, StorageResource *r)
{
    r->tier        = proto_read_u8(buf, &p);
    proto_read_str(buf, &p, r->device_path, 256);
    r->capacity    = proto_read_u64(buf, &p);
    r->base_offset = proto_read_u64(buf, &p);
    r->online      = (int)proto_read_u8(buf, &p);
    return p;
}

/* ==================================================================== */
/* Pack: C struct -> UmmProtoBody                                       */
/* ==================================================================== */

int meta_pack_register_chunk(const char *name, gpa_t gpa, uint64_t size,
                              UmmProtoBody *body)
{
    if (!name || !body)
        return UMM_E_INVALID_ARG;
    memset(body->data, 0, sizeof(body->data));
    size_t p = 0;
    proto_write_str(body->data, &p, name, 64);
    proto_write_u64(body->data, &p, gpa);
    proto_write_u64(body->data, &p, size);
    body->len = (uint16_t)p;
    return UMM_OK;
}

int meta_pack_lookup_chunk(const char *name, UmmProtoBody *body)
{
    if (!name || !body)
        return UMM_E_INVALID_ARG;
    memset(body->data, 0, sizeof(body->data));
    size_t p = 0;
    proto_write_str(body->data, &p, name, 64);
    body->len = (uint16_t)p;
    return UMM_OK;
}

int meta_pack_lookup_chunk_by_id(chunk_id_t chunk_id, UmmProtoBody *body)
{
    if (!body)
        return UMM_E_INVALID_ARG;
    memset(body->data, 0, sizeof(body->data));
    size_t p = 0;
    proto_write_u64(body->data, &p, chunk_id);
    body->len = (uint16_t)p;
    return UMM_OK;
}

int meta_pack_unregister_chunk(chunk_id_t chunk_id, UmmProtoBody *body)
{
    if (!body)
        return UMM_E_INVALID_ARG;
    memset(body->data, 0, sizeof(body->data));
    size_t p = 0;
    proto_write_u64(body->data, &p, chunk_id);
    body->len = (uint16_t)p;
    return UMM_OK;
}

int meta_pack_add_ref(chunk_id_t chunk_id, UmmProtoBody *body)
{
    if (!body)
        return UMM_E_INVALID_ARG;
    memset(body->data, 0, sizeof(body->data));
    size_t p = 0;
    proto_write_u64(body->data, &p, chunk_id);
    body->len = (uint16_t)p;
    return UMM_OK;
}

int meta_pack_release_ref(chunk_id_t chunk_id, UmmProtoBody *body)
{
    if (!body)
        return UMM_E_INVALID_ARG;
    memset(body->data, 0, sizeof(body->data));
    size_t p = 0;
    proto_write_u64(body->data, &p, chunk_id);
    body->len = (uint16_t)p;
    return UMM_OK;
}

int meta_pack_list_chunks(node_id_t node, UmmProtoBody *body)
{
    if (!body)
        return UMM_E_INVALID_ARG;
    memset(body->data, 0, sizeof(body->data));
    size_t p = 0;
    proto_write_u32(body->data, &p, node);
    body->len = (uint16_t)p;
    return UMM_OK;
}

int meta_pack_heartbeat(UmmProtoBody *body)
{
    if (!body)
        return UMM_E_INVALID_ARG;
    body->len = 0;
    return UMM_OK;
}

/* ==================================================================== */
/* Unpack: UmmProtoBody -> C struct (requests)                          */
/* ==================================================================== */

int meta_unpack_register_chunk(const UmmProtoBody *body,
                                MetaRegisterChunkReq *out)
{
    if (!body || !out || body->len < 72)
        return UMM_E_INVALID_ARG;
    size_t p = 0;
    proto_read_str(body->data, &p, out->name, 64);
    out->gpa  = proto_read_u64(body->data, &p);
    out->size = proto_read_u64(body->data, &p);
    return UMM_OK;
}

int meta_unpack_lookup_chunk(const UmmProtoBody *body,
                              MetaLookupChunkReq *out)
{
    if (!body || !out || body->len < 64)
        return UMM_E_INVALID_ARG;
    size_t p = 0;
    proto_read_str(body->data, &p, out->name, 64);
    return UMM_OK;
}

int meta_unpack_lookup_chunk_by_id(const UmmProtoBody *body,
                                    MetaLookupChunkByIdReq *out)
{
    if (!body || !out || body->len < 8)
        return UMM_E_INVALID_ARG;
    size_t p = 0;
    out->chunk_id = proto_read_u64(body->data, &p);
    return UMM_OK;
}

int meta_unpack_unregister_chunk(const UmmProtoBody *body,
                                  MetaUnregisterChunkReq *out)
{
    if (!body || !out || body->len < 8)
        return UMM_E_INVALID_ARG;
    size_t p = 0;
    out->chunk_id = proto_read_u64(body->data, &p);
    return UMM_OK;
}

int meta_unpack_add_ref(const UmmProtoBody *body, chunk_id_t *out)
{
    if (!body || !out || body->len < 8)
        return UMM_E_INVALID_ARG;
    size_t p = 0;
    *out = proto_read_u64(body->data, &p);
    return UMM_OK;
}

int meta_unpack_release_ref(const UmmProtoBody *body, chunk_id_t *out)
{
    if (!body || !out || body->len < 8)
        return UMM_E_INVALID_ARG;
    size_t p = 0;
    *out = proto_read_u64(body->data, &p);
    return UMM_OK;
}

int meta_unpack_list_chunks(const UmmProtoBody *body,
                             MetaListChunksReq *out)
{
    if (!body || !out || body->len < 4)
        return UMM_E_INVALID_ARG;
    size_t p = 0;
    out->node = proto_read_u32(body->data, &p);
    return UMM_OK;
}

/* ==================================================================== */
/* Unpack: UmmProtoBody -> C struct (responses)                         */
/* ==================================================================== */

int meta_unpack_register_chunk_resp(const UmmProtoBody *body,
                                     MetaRegisterChunkResp *out)
{
    if (!body || !out || body->len < 8)
        return UMM_E_INVALID_ARG;
    size_t p = 0;
    out->status   = proto_read_i32(body->data, &p);
    out->chunk_id = proto_read_u64(body->data, &p);
    return UMM_OK;
}

/* Helper: pack a single ChunkMetadata */
static size_t pack_chunk_meta(uint8_t *buf, size_t p, const ChunkMetadata *m)
{
    proto_write_u64(buf, &p, m->chunk_id);
    proto_write_str(buf, &p, m->name, 64);
    proto_write_u64(buf, &p, m->gpa);
    proto_write_u64(buf, &p, m->size);
    return p;
}

/* Helper: unpack a single ChunkMetadata */
static size_t unpack_chunk_meta(const uint8_t *buf, size_t p, ChunkMetadata *m)
{
    m->chunk_id = proto_read_u64(buf, &p);
    proto_read_str(buf, &p, m->name, 64);
    m->gpa  = proto_read_u64(buf, &p);
    m->size = proto_read_u64(buf, &p);
    return p;
}

int meta_unpack_lookup_chunk_resp(const UmmProtoBody *body,
                                   MetaLookupChunkResp *out)
{
    if (!body || !out || body->len < 80)
        return UMM_E_INVALID_ARG;
    size_t p = 0;
    out->status = proto_read_i32(body->data, &p);
    p = unpack_chunk_meta(body->data, p, &out->meta);
    return UMM_OK;
}

int meta_unpack_lookup_chunk_by_id_resp(const UmmProtoBody *body,
                                         MetaLookupChunkByIdResp *out)
{
    if (!body || !out || body->len < 80)
        return UMM_E_INVALID_ARG;
    size_t p = 0;
    out->status = proto_read_i32(body->data, &p);
    p = unpack_chunk_meta(body->data, p, &out->meta);
    return UMM_OK;
}

int meta_unpack_unregister_chunk_resp(const UmmProtoBody *body,
                                       MetaUnregisterChunkResp *out)
{
    if (!body || !out || body->len < 4)
        return UMM_E_INVALID_ARG;
    size_t p = 0;
    out->status = proto_read_i32(body->data, &p);
    return UMM_OK;
}

int meta_unpack_add_ref_resp(const UmmProtoBody *body, MetaAddRefResp *out)
{
    if (!body || !out || body->len < 8)
        return UMM_E_INVALID_ARG;
    size_t p = 0;
    out->status   = proto_read_i32(body->data, &p);
    out->ref_count= proto_read_u32(body->data, &p);
    return UMM_OK;
}

int meta_unpack_release_ref_resp(const UmmProtoBody *body,
                                  MetaReleaseRefResp *out)
{
    if (!body || !out || body->len < 8)
        return UMM_E_INVALID_ARG;
    size_t p = 0;
    out->status   = proto_read_i32(body->data, &p);
    out->ref_count= proto_read_u32(body->data, &p);
    return UMM_OK;
}

/* ==================================================================== */
/* List chunks response (variable-length array)                         */
/* ==================================================================== */

#define CHUNK_META_SERIALIZED_SIZE 88  /* 8 + 64 + 8 + 8 = 88 bytes */

int meta_pack_list_chunks_resp(const ChunkMetadata *metas, uint32_t count,
                                UmmProtoBody *body)
{
    if (!body || (count > 0 && !metas))
        return UMM_E_INVALID_ARG;

    /* Fit as many as possible into UMM_PROTO_MAX_BODY */
    uint32_t max_fit = (UMM_PROTO_MAX_BODY - 8) / CHUNK_META_SERIALIZED_SIZE;
    if (count > max_fit)
        count = max_fit;

    memset(body->data, 0, sizeof(body->data));
    size_t p = 0;
    proto_write_i32(body->data, &p, 0);         /* status = OK */
    proto_write_u32(body->data, &p, count);

    for (uint32_t i = 0; i < count; i++)
        p = pack_chunk_meta(body->data, p, &metas[i]);

    body->len = (uint16_t)p;
    return UMM_OK;
}

int meta_unpack_list_chunks_resp(const UmmProtoBody *body,
                                  ChunkMetadata *out_metas,
                                  uint32_t max_count, uint32_t *out_count)
{
    if (!body || !out_count || body->len < 8)
        return UMM_E_INVALID_ARG;

    size_t p = 0;
    int32_t status = proto_read_i32(body->data, &p);
    if (status != 0) {
        *out_count = 0;
        return UMM_E_RPC_ERROR;
    }

    uint32_t count = proto_read_u32(body->data, &p);
    if (count > max_count)
        count = max_count;

    uint32_t avail = (body->len - 8) / CHUNK_META_SERIALIZED_SIZE;
    if (count > avail)
        count = avail;

    for (uint32_t i = 0; i < count && out_metas; i++)
        p = unpack_chunk_meta(body->data, p, &out_metas[i]);

    *out_count = count;
    return UMM_OK;
}

/* ==================================================================== */
/* META_OP_REGISTER_STORAGE_RESOURCE  (9)                               */
/*   Req:  node(u32) + StorageResource                = 4 + 274 = 278  */
/*   Resp: status(i32)                                = 4                */
/* ==================================================================== */

int meta_pack_register_storage_resource(node_id_t node, const StorageResource *res,
                                         UmmProtoBody *body)
{
    if (!res || !body)
        return UMM_E_INVALID_ARG;
    memset(body->data, 0, sizeof(body->data));
    size_t p = 0;
    proto_write_u32(body->data, &p, node);
    p = pack_storage_resource(body->data, p, res);
    body->len = (uint16_t)p;
    return UMM_OK;
}

int meta_unpack_register_storage_resource(const UmmProtoBody *body,
                                           MetaRegisterStorageReq *out)
{
    if (!body || !out || body->len < 278)
        return UMM_E_INVALID_ARG;
    size_t p = 0;
    out->node = proto_read_u32(body->data, &p);
    p = unpack_storage_resource(body->data, p, &out->res);
    return UMM_OK;
}

/* ==================================================================== */
/* META_OP_GET_STORAGE_TOPOLOGY  (10)                                   */
/*   Req:  node(u32)                                  = 4                */
/*   Resp: status(i32) + node_id(u32) + num_resources(u32) + N*274      */
/* ==================================================================== */

int meta_pack_get_storage_topology(node_id_t node, UmmProtoBody *body)
{
    if (!body)
        return UMM_E_INVALID_ARG;
    memset(body->data, 0, sizeof(body->data));
    size_t p = 0;
    proto_write_u32(body->data, &p, node);
    body->len = (uint16_t)p;
    return UMM_OK;
}

int meta_unpack_get_storage_topology(const UmmProtoBody *body,
                                      MetaGetTopologyReq *out)
{
    if (!body || !out || body->len < 4)
        return UMM_E_INVALID_ARG;
    size_t p = 0;
    out->node = proto_read_u32(body->data, &p);
    return UMM_OK;
}

int meta_pack_get_storage_topology_resp(const StorageTopology *topo,
                                         UmmProtoBody *body)
{
    if (!topo || !body)
        return UMM_E_INVALID_ARG;

    uint32_t count = topo->num_resources;
    uint32_t max_fit = (UMM_PROTO_MAX_BODY - STORAGE_TOPOLOGY_HEADER_SIZE)
                       / STORAGE_RESOURCE_SERIALIZED_SIZE;
    if (count > max_fit)
        count = max_fit;

    memset(body->data, 0, sizeof(body->data));
    size_t p = 0;
    proto_write_i32(body->data, &p, 0);            /* status = OK */
    proto_write_u32(body->data, &p, topo->node_id);
    proto_write_u32(body->data, &p, count);

    for (uint32_t i = 0; i < count; i++)
        p = pack_storage_resource(body->data, p, &topo->resources[i]);

    body->len = (uint16_t)p;
    return UMM_OK;
}

int meta_unpack_get_storage_topology_resp(const UmmProtoBody *body,
                                           StorageTopology *out)
{
    if (!body || !out || body->len < STORAGE_TOPOLOGY_HEADER_SIZE)
        return UMM_E_INVALID_ARG;

    size_t p = 0;
    int32_t status = proto_read_i32(body->data, &p);
    if (status != 0) {
        out->num_resources = 0;
        return UMM_E_RPC_ERROR;
    }

    out->node_id       = proto_read_u32(body->data, &p);
    uint32_t count     = proto_read_u32(body->data, &p);

    uint32_t avail = (body->len - STORAGE_TOPOLOGY_HEADER_SIZE)
                     / STORAGE_RESOURCE_SERIALIZED_SIZE;
    if (count > avail)
        count = avail;
    if (count > UMM_NUM_TIERS)
        count = UMM_NUM_TIERS;

    out->num_resources = count;
    for (uint32_t i = 0; i < count; i++)
        p = unpack_storage_resource(body->data, p, &out->resources[i]);

    return UMM_OK;
}
