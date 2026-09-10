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
    MEM_OP_ALLOC_ON_DEVICE  = 12, /* tier(u8), device(u32), size(u64), flags(u32) */
    MEM_OP_ALLOC            = 1,
    MEM_OP_FREE             = 2,
    MEM_OP_GET_STATS        = 3,
    MEM_OP_HEARTBEAT        = 4,
    /* --- new --- */
    MEM_OP_ALLOC_TIERED     = 5,   /* tier(u8) + size(u64) + flags(u32) */
    MEM_OP_FREE_TIERED      = 6,   /* tier(u8) + offset(u64) + size(u64) */
    MEM_OP_GET_TIER_STATS   = 7,   /* tier(u8) */
    MEM_OP_REGISTER_STORAGE = 8,   /* StorageResource serialized */
    MEM_OP_GET_TOPOLOGY     = 9,   /* empty=first page; optional start(u32) */
    /* --- Phase 1: 跨节点数据面（payload 走流式传输，不占 4KB body） --- */
    MEM_OP_DATA_READ        = 10,  /* req: gpa(u64)+len(u64)；
                                      resp: status(i32)+len(u64)，随后 len 字节
                                      原始数据流（仅 status==OK 时存在） */
    MEM_OP_DATA_WRITE       = 11,  /* req: gpa(u64)+len(u64)，随后 len 字节
                                      原始数据流；resp: status(i32) */
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
typedef struct {
    uint8_t tier;
    uint32_t device_idx;
    uint64_t size;
    uint32_t flags;
} MemAllocOnDeviceReq;
int mem_pack_alloc_on_device(uint8_t tier, uint32_t device_idx,
                             uint64_t size, uint32_t flags, UmmProtoBody *body);
int mem_unpack_alloc_on_device(const UmmProtoBody *body, MemAllocOnDeviceReq *out);

int mem_pack_alloc_tiered(uint8_t tier, uint64_t size, uint32_t flags, UmmProtoBody *body);
int mem_unpack_alloc_tiered(const UmmProtoBody *body, MemAllocTieredReq *out);
int mem_unpack_alloc_tiered_resp(const UmmProtoBody *body, MemAllocTieredResp *out);

/* v2：新服务端响应追加属主 node(u8)（13 字节）；旧服务端 12 字节时
 * *out_owner 置 UMM_NODE_UNKNOWN(0xFF)，由调用方按配置回退。 */
int mem_unpack_alloc_tiered_resp2(const UmmProtoBody *body,
                                  MemAllocTieredResp *out,
                                  uint8_t *out_owner);

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
/* Existing response format, paginated to fit a control frame. */
#define MEM_TOPOLOGY_PAGE_CAP ((UMM_PROTO_MAX_BODY - 12) / 274)
int mem_pack_get_topology(UmmProtoBody *body);
int mem_pack_get_topology_resp(const StorageTopology *topo, UmmProtoBody *body);
int mem_unpack_get_topology_resp(const UmmProtoBody *body, StorageTopology *out);

/* ------------------------------------------------------------------ */
/* Pack / Unpack — Phase 1 数据面 op                                   */
/* 请求 body 固定 16 字节：gpa(u64) + len(u64)；payload 流式跟随。      */
/* ------------------------------------------------------------------ */
int mem_pack_data_req(gpa_t gpa, uint64_t len, UmmProtoBody *body);
int mem_unpack_data_req(const UmmProtoBody *body, gpa_t *out_gpa,
                        uint64_t *out_len);
/* DATA_READ 响应 body：status(i32) + len(u64) = 12 字节 */
int mem_unpack_data_read_resp(const UmmProtoBody *body, int32_t *out_status,
                              uint64_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* UMM_MEM_PROTOCOL_H */
