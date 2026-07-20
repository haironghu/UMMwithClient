#ifndef UMM_META_PROTOCOL_H
#define UMM_META_PROTOCOL_H

#include "../common/types.h"
#include "../common/error_codes.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Opcodes for metadata service RPC (v2.0)                             */
/*                                                                     */
/* All chunks are flat. No Region.                                     */
/* ------------------------------------------------------------------ */
enum {
    META_OP_REGISTER_CHUNK            = 1,
    META_OP_LOOKUP_CHUNK              = 2,
    META_OP_LOOKUP_CHUNK_BY_ID        = 3,
    META_OP_UNREGISTER_CHUNK          = 4,
    META_OP_ADD_REF                   = 5,
    META_OP_RELEASE_REF               = 6,
    META_OP_LIST_CHUNKS_BY_NODE       = 7,
    META_OP_HEARTBEAT                 = 8,
    /* --- new --- */
    META_OP_REGISTER_STORAGE_RESOURCE = 9,  /* node(u32) + StorageResource */
    META_OP_GET_STORAGE_TOPOLOGY      = 10, /* node(u32) */
};

/* ------------------------------------------------------------------ */
/* Request / Response structs                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    char     name[64];
    gpa_t    gpa;
    uint64_t size;
} MetaRegisterChunkReq;

typedef struct {
    int32_t    status;
    chunk_id_t chunk_id;
} MetaRegisterChunkResp;

typedef struct {
    char name[64];
} MetaLookupChunkReq;

typedef struct {
    int32_t       status;
    ChunkMetadata meta;
} MetaLookupChunkResp;

typedef struct {
    chunk_id_t chunk_id;
} MetaLookupChunkByIdReq;

typedef struct {
    int32_t       status;
    ChunkMetadata meta;
} MetaLookupChunkByIdResp;

typedef struct {
    chunk_id_t chunk_id;
} MetaUnregisterChunkReq;

typedef struct {
    int32_t status;
} MetaUnregisterChunkResp;

typedef struct {
    chunk_id_t chunk_id;
} MetaAddRefReq;

typedef struct {
    int32_t  status;
    uint32_t ref_count;
} MetaAddRefResp;

typedef struct {
    chunk_id_t chunk_id;
} MetaReleaseRefReq;

typedef struct {
    int32_t  status;
    uint32_t ref_count;
} MetaReleaseRefResp;

typedef struct {
    node_id_t node;
} MetaListChunksReq;

typedef struct {
    int32_t  status;
    uint32_t count;
} MetaListChunksRespHeader;

/* --- new: storage topology request/response structs --- */

typedef struct {
    uint32_t        node;
    StorageResource res;
} MetaRegisterStorageReq;

typedef struct {
    uint32_t          node;
} MetaGetTopologyReq;

typedef struct {
    int32_t           status;
    StorageTopology   topology;
} MetaGetTopologyResp;

/* ------------------------------------------------------------------ */
/* Pack: C struct  ->  UmmProtoBody                                    */
/* ------------------------------------------------------------------ */

int meta_pack_register_chunk (const char *name, gpa_t gpa, uint64_t size,
                               UmmProtoBody *body);
int meta_pack_lookup_chunk   (const char *name, UmmProtoBody *body);
int meta_pack_lookup_chunk_by_id(chunk_id_t chunk_id, UmmProtoBody *body);
int meta_pack_unregister_chunk(chunk_id_t chunk_id, UmmProtoBody *body);
int meta_pack_add_ref        (chunk_id_t chunk_id, UmmProtoBody *body);
int meta_pack_release_ref    (chunk_id_t chunk_id, UmmProtoBody *body);
int meta_pack_list_chunks    (node_id_t node, UmmProtoBody *body);
int meta_pack_heartbeat      (UmmProtoBody *body);

/* ------------------------------------------------------------------ */
/* Unpack: UmmProtoBody  ->  C struct (requests)                       */
/* ------------------------------------------------------------------ */

int meta_unpack_register_chunk  (const UmmProtoBody *body,
                                  MetaRegisterChunkReq *out);
int meta_unpack_lookup_chunk    (const UmmProtoBody *body,
                                  MetaLookupChunkReq *out);
int meta_unpack_lookup_chunk_by_id(const UmmProtoBody *body,
                                    MetaLookupChunkByIdReq *out);
int meta_unpack_unregister_chunk(const UmmProtoBody *body,
                                  MetaUnregisterChunkReq *out);
int meta_unpack_add_ref         (const UmmProtoBody *body, chunk_id_t *out);
int meta_unpack_release_ref     (const UmmProtoBody *body, chunk_id_t *out);
int meta_unpack_list_chunks     (const UmmProtoBody *body,
                                  MetaListChunksReq *out);

/* ------------------------------------------------------------------ */
/* Unpack: UmmProtoBody  ->  C struct (responses)                      */
/* ------------------------------------------------------------------ */

int meta_unpack_register_chunk_resp (const UmmProtoBody *body,
                                      MetaRegisterChunkResp *out);
int meta_unpack_lookup_chunk_resp   (const UmmProtoBody *body,
                                      MetaLookupChunkResp *out);
int meta_unpack_lookup_chunk_by_id_resp(const UmmProtoBody *body,
                                         MetaLookupChunkByIdResp *out);
int meta_unpack_unregister_chunk_resp(const UmmProtoBody *body,
                                        MetaUnregisterChunkResp *out);
int meta_unpack_add_ref_resp          (const UmmProtoBody *body,
                                        MetaAddRefResp *out);
int meta_unpack_release_ref_resp      (const UmmProtoBody *body,
                                        MetaReleaseRefResp *out);

/* List chunks response (variable-length array of ChunkMetadata) */
int meta_pack_list_chunks_resp(const ChunkMetadata *metas, uint32_t count,
                                UmmProtoBody *body);
int meta_unpack_list_chunks_resp(const UmmProtoBody *body,
                                  ChunkMetadata *out_metas,
                                  uint32_t max_count, uint32_t *out_count);

/* ------------------------------------------------------------------ */
/* Pack / Unpack — Storage resource registration                      */
/* ------------------------------------------------------------------ */
int meta_pack_register_storage_resource(node_id_t node, const StorageResource *res,
                                         UmmProtoBody *body);
int meta_unpack_register_storage_resource(const UmmProtoBody *body,
                                           MetaRegisterStorageReq *out);

/* ------------------------------------------------------------------ */
/* Pack / Unpack — Topology query                                     */
/* ------------------------------------------------------------------ */
int meta_pack_get_storage_topology(node_id_t node, UmmProtoBody *body);
int meta_unpack_get_storage_topology(const UmmProtoBody *body,
                                      MetaGetTopologyReq *out);
int meta_pack_get_storage_topology_resp(const StorageTopology *topo,
                                         UmmProtoBody *body);
int meta_unpack_get_storage_topology_resp(const UmmProtoBody *body,
                                           StorageTopology *out);

#ifdef __cplusplus
}
#endif

#endif /* UMM_META_PROTOCOL_H */
