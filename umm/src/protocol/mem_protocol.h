#ifndef UMM_MEM_PROTOCOL_H
#define UMM_MEM_PROTOCOL_H

#include "../common/types.h"
#include "../common/error_codes.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Opcodes for memory service RPC                                      */
/* ------------------------------------------------------------------ */
enum {
    MEM_OP_ALLOC            = 1,
    MEM_OP_FREE             = 2,
    MEM_OP_GET_STATS        = 3,
    MEM_OP_HEARTBEAT        = 4,
    /* --- new --- */
    MEM_OP_ALLOC_TIERED     = 5,   /* tier(u8) + size(u64) + flags(u32) */
    MEM_OP_FREE_TIERED      = 6,   /* tier(u8) + offset(u64) + size(u64) */
    MEM_OP_GET_TIER_STATS   = 7,   /* tier(u8) */
    MEM_OP_REGISTER_STORAGE = 8,   /* StorageResource serialized */
    MEM_OP_GET_TOPOLOGY     = 9,   /* no request body */
};

/* ------------------------------------------------------------------ */
/* Request / Response structs                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t size;
    uint32_t flags;
} MemAllocReq;

typedef struct {
    int32_t  status;
    uint64_t offset;
} MemAllocResp;

typedef struct {
    uint64_t offset;
    uint64_t size;
} MemFreeReq;

typedef struct {
    int32_t status;
} MemFreeResp;

typedef struct {
    uint64_t total;
    uint64_t used;
    uint64_t free;
} MemStatsResp;

/* --- new: tiered request/response structs --- */

typedef struct {
    uint8_t  tier;
    uint64_t size;
    uint32_t flags;
} MemAllocTieredReq;

typedef struct {
    int32_t  status;
    uint64_t offset;
} MemAllocTieredResp;

typedef struct {
    uint8_t  tier;
    uint64_t offset;
    uint64_t size;
} MemFreeTieredReq;

typedef struct {
    uint8_t  tier;
} MemGetTierStatsReq;

typedef struct {
    int32_t  status;
    uint64_t total;
    uint64_t used;
    uint64_t free;
} MemGetTierStatsResp;

typedef struct {
    int32_t  status;
    StorageTopology topology;
} MemGetTopologyResp;

/* ------------------------------------------------------------------ */
/* Pack: C struct  ->  UmmProtoBody                                    */
/* ------------------------------------------------------------------ */

int mem_pack_alloc     (uint64_t size, uint32_t flags, UmmProtoBody *body);
int mem_pack_free      (uint64_t offset, uint64_t size, UmmProtoBody *body);
int mem_pack_get_stats (UmmProtoBody *body);
int mem_pack_heartbeat (UmmProtoBody *body);

/* ------------------------------------------------------------------ */
/* Unpack: UmmProtoBody  ->  C struct (requests)                       */
/* ------------------------------------------------------------------ */

int mem_unpack_alloc(const UmmProtoBody *body, MemAllocReq *out);
int mem_unpack_free (const UmmProtoBody *body, MemFreeReq  *out);

/* ------------------------------------------------------------------ */
/* Unpack: UmmProtoBody  ->  C struct (responses)                      */
/* ------------------------------------------------------------------ */

int mem_unpack_alloc_resp(const UmmProtoBody *body, MemAllocResp *out);
int mem_unpack_free_resp (const UmmProtoBody *body, MemFreeResp  *out);
int mem_unpack_stats_resp(const UmmProtoBody *body, MemStatsResp *out);

/* ------------------------------------------------------------------ */
/* Pack / Unpack — Tiered alloc                                       */
/* ------------------------------------------------------------------ */
int mem_pack_alloc_tiered(uint8_t tier, uint64_t size, uint32_t flags, UmmProtoBody *body);
int mem_unpack_alloc_tiered(const UmmProtoBody *body, MemAllocTieredReq *out);
int mem_unpack_alloc_tiered_resp(const UmmProtoBody *body, MemAllocTieredResp *out);

/* ------------------------------------------------------------------ */
/* Pack / Unpack — Tiered free                                        */
/* ------------------------------------------------------------------ */
int mem_pack_free_tiered(uint8_t tier, uint64_t offset, uint64_t size, UmmProtoBody *body);
int mem_unpack_free_tiered(const UmmProtoBody *body, MemFreeTieredReq *out);

/* ------------------------------------------------------------------ */
/* Pack / Unpack — Tier stats                                         */
/* ------------------------------------------------------------------ */
int mem_pack_get_tier_stats(uint8_t tier, UmmProtoBody *body);
int mem_unpack_get_tier_stats(const UmmProtoBody *body, MemGetTierStatsReq *out);
int mem_unpack_get_tier_stats_resp(const UmmProtoBody *body, MemGetTierStatsResp *out);

/* ------------------------------------------------------------------ */
/* Pack / Unpack — Storage resource registration                      */
/* ------------------------------------------------------------------ */
int mem_pack_register_storage(const StorageResource *res, UmmProtoBody *body);
int mem_unpack_register_storage(const UmmProtoBody *body, StorageResource *out);

/* ------------------------------------------------------------------ */
/* Pack / Unpack — Topology query                                     */
/* ------------------------------------------------------------------ */
int mem_pack_get_topology(UmmProtoBody *body);
int mem_pack_get_topology_resp(const StorageTopology *topo, UmmProtoBody *body);
int mem_unpack_get_topology_resp(const UmmProtoBody *body, StorageTopology *out);

#ifdef __cplusplus
}
#endif

#endif /* UMM_MEM_PROTOCOL_H */
