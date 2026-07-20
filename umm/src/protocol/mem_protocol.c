#include "mem_protocol.h"
#include "protocol_common.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* Helper: init body                                                   */
/* ------------------------------------------------------------------ */
static inline void body_init(UmmProtoBody *body)
{
    memset(body->data, 0, sizeof(body->data));
    body->len = 0;
}

/* ------------------------------------------------------------------ */
/* MEM_OP_ALLOC  (1)                                                   */
/*   Req:  size(u64) + flags(u32)                     = 8 + 4 = 12    */
/*   Resp: status(i32) + offset(u64)                  = 4 + 8 = 12    */
/* ------------------------------------------------------------------ */

int mem_pack_alloc(uint64_t size, uint32_t flags, UmmProtoBody *body)
{
    size_t p = 0;
    body_init(body);
    proto_write_u64(body->data, &p, size);
    proto_write_u32(body->data, &p, flags);
    body->len = (uint16_t)p;
    return UMM_OK;
}

int mem_unpack_alloc(const UmmProtoBody *body, MemAllocReq *out)
{
    if (!body || !out || body->len < 12)
        return UMM_E_INVALID_ARG;
    size_t p = 0;
    out->size  = proto_read_u64(body->data, &p);
    out->flags = proto_read_u32(body->data, &p);
    return UMM_OK;
}

int mem_unpack_alloc_resp(const UmmProtoBody *body, MemAllocResp *out)
{
    if (!body || !out || body->len < 12)
        return UMM_E_INVALID_ARG;
    size_t p = 0;
    out->status = proto_read_i32(body->data, &p);
    out->offset = proto_read_u64(body->data, &p);
    return UMM_OK;
}

/* ------------------------------------------------------------------ */
/* MEM_OP_FREE  (2)                                                    */
/*   Req:  offset(u64) + size(u64)                    = 8 + 8 = 16    */
/*   Resp: status(i32)                                = 4             */
/* ------------------------------------------------------------------ */

int mem_pack_free(uint64_t offset, uint64_t size, UmmProtoBody *body)
{
    size_t p = 0;
    body_init(body);
    proto_write_u64(body->data, &p, offset);
    proto_write_u64(body->data, &p, size);
    body->len = (uint16_t)p;
    return UMM_OK;
}

int mem_unpack_free(const UmmProtoBody *body, MemFreeReq *out)
{
    if (!body || !out || body->len < 16)
        return UMM_E_INVALID_ARG;
    size_t p = 0;
    out->offset = proto_read_u64(body->data, &p);
    out->size   = proto_read_u64(body->data, &p);
    return UMM_OK;
}

int mem_unpack_free_resp(const UmmProtoBody *body, MemFreeResp *out)
{
    if (!body || !out || body->len < 4)
        return UMM_E_INVALID_ARG;
    size_t p = 0;
    out->status = proto_read_i32(body->data, &p);
    return UMM_OK;
}

/* ------------------------------------------------------------------ */
/* MEM_OP_GET_STATS  (3)                                               */
/*   No request payload                                                */
/*   Resp: total(u64) + used(u64) + free(u64)         = 24            */
/* ------------------------------------------------------------------ */

int mem_pack_get_stats(UmmProtoBody *body)
{
    body_init(body);
    body->len = 0;
    return UMM_OK;
}

int mem_unpack_stats_resp(const UmmProtoBody *body, MemStatsResp *out)
{
    if (!body || !out || body->len < 24)
        return UMM_E_INVALID_ARG;
    size_t p = 0;
    out->total = proto_read_u64(body->data, &p);
    out->used  = proto_read_u64(body->data, &p);
    out->free  = proto_read_u64(body->data, &p);
    return UMM_OK;
}

/* ------------------------------------------------------------------ */
/* MEM_OP_HEARTBEAT  (4)                                               */
/*   No payload                                                        */
/* ------------------------------------------------------------------ */

int mem_pack_heartbeat(UmmProtoBody *body)
{
    body_init(body);
    body->len = 0;
    return UMM_OK;
}

/* ------------------------------------------------------------------ */
/* Helper: pack/unpack a single StorageResource                        */
/* ------------------------------------------------------------------ */

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
/* MEM_OP_ALLOC_TIERED  (5)                                             */
/*   Req:  tier(u8) + size(u64) + flags(u32)          = 1 + 8 + 4 = 13  */
/*   Resp: status(i32) + offset(u64)                  = 4 + 8 = 12      */
/* ==================================================================== */

int mem_pack_alloc_tiered(uint8_t tier, uint64_t size, uint32_t flags, UmmProtoBody *body)
{
    size_t p = 0;
    body_init(body);
    proto_write_u8 (body->data, &p, tier);
    proto_write_u64(body->data, &p, size);
    proto_write_u32(body->data, &p, flags);
    body->len = (uint16_t)p;
    return UMM_OK;
}

int mem_unpack_alloc_tiered(const UmmProtoBody *body, MemAllocTieredReq *out)
{
    if (!body || !out || body->len < 13)
        return UMM_E_INVALID_ARG;
    size_t p = 0;
    out->tier  = proto_read_u8(body->data, &p);
    out->size  = proto_read_u64(body->data, &p);
    out->flags = proto_read_u32(body->data, &p);
    return UMM_OK;
}

int mem_unpack_alloc_tiered_resp(const UmmProtoBody *body, MemAllocTieredResp *out)
{
    if (!body || !out || body->len < 12)
        return UMM_E_INVALID_ARG;
    size_t p = 0;
    out->status = proto_read_i32(body->data, &p);
    out->offset = proto_read_u64(body->data, &p);
    return UMM_OK;
}

/* ==================================================================== */
/* MEM_OP_FREE_TIERED  (6)                                              */
/*   Req:  tier(u8) + offset(u64) + size(u64)         = 1 + 8 + 8 = 17  */
/*   Resp: status(i32)                                = 4               */
/* ==================================================================== */

int mem_pack_free_tiered(uint8_t tier, uint64_t offset, uint64_t size, UmmProtoBody *body)
{
    size_t p = 0;
    body_init(body);
    proto_write_u8 (body->data, &p, tier);
    proto_write_u64(body->data, &p, offset);
    proto_write_u64(body->data, &p, size);
    body->len = (uint16_t)p;
    return UMM_OK;
}

int mem_unpack_free_tiered(const UmmProtoBody *body, MemFreeTieredReq *out)
{
    if (!body || !out || body->len < 17)
        return UMM_E_INVALID_ARG;
    size_t p = 0;
    out->tier   = proto_read_u8(body->data, &p);
    out->offset = proto_read_u64(body->data, &p);
    out->size   = proto_read_u64(body->data, &p);
    return UMM_OK;
}

/* ==================================================================== */
/* MEM_OP_GET_TIER_STATS  (7)                                           */
/*   Req:  tier(u8)                                   = 1                */
/*   Resp: status(i32) + total(u64) + used(u64) + free(u64) = 4 + 24    */
/* ==================================================================== */

int mem_pack_get_tier_stats(uint8_t tier, UmmProtoBody *body)
{
    size_t p = 0;
    body_init(body);
    proto_write_u8(body->data, &p, tier);
    body->len = (uint16_t)p;
    return UMM_OK;
}

int mem_unpack_get_tier_stats(const UmmProtoBody *body, MemGetTierStatsReq *out)
{
    if (!body || !out || body->len < 1)
        return UMM_E_INVALID_ARG;
    size_t p = 0;
    out->tier = proto_read_u8(body->data, &p);
    return UMM_OK;
}

int mem_unpack_get_tier_stats_resp(const UmmProtoBody *body, MemGetTierStatsResp *out)
{
    if (!body || !out || body->len < 28)
        return UMM_E_INVALID_ARG;
    size_t p = 0;
    out->status = proto_read_i32(body->data, &p);
    out->total  = proto_read_u64(body->data, &p);
    out->used   = proto_read_u64(body->data, &p);
    out->free   = proto_read_u64(body->data, &p);
    return UMM_OK;
}

/* ==================================================================== */
/* MEM_OP_REGISTER_STORAGE  (8)                                         */
/*   Req:  StorageResource                              = 274 bytes      */
/*   Resp: status(i32)                                = 4                */
/* ==================================================================== */

#define STORAGE_RESOURCE_SERIALIZED_SIZE 274  /* 1 + 256 + 8 + 8 + 1 */

int mem_pack_register_storage(const StorageResource *res, UmmProtoBody *body)
{
    if (!res || !body)
        return UMM_E_INVALID_ARG;
    size_t p = 0;
    body_init(body);
    p = pack_storage_resource(body->data, p, res);
    body->len = (uint16_t)p;
    return UMM_OK;
}

int mem_unpack_register_storage(const UmmProtoBody *body, StorageResource *out)
{
    if (!body || !out || body->len < STORAGE_RESOURCE_SERIALIZED_SIZE)
        return UMM_E_INVALID_ARG;
    size_t p = 0;
    p = unpack_storage_resource(body->data, p, out);
    return UMM_OK;
}

/* ==================================================================== */
/* MEM_OP_GET_TOPOLOGY  (9)                                             */
/*   Req:  no payload                                                    */
/*   Resp: status(i32) + node_id(u32) + num_resources(u32) + N*274      */
/* ==================================================================== */

#define STORAGE_TOPOLOGY_HEADER_SIZE 12  /* 4 + 4 + 4 */

int mem_pack_get_topology(UmmProtoBody *body)
{
    body_init(body);
    body->len = 0;
    return UMM_OK;
}

int mem_pack_get_topology_resp(const StorageTopology *topo, UmmProtoBody *body)
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

int mem_unpack_get_topology_resp(const UmmProtoBody *body, StorageTopology *out)
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
