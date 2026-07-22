#ifndef UMM_H
#define UMM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * GPA (Global Physical Address) encoding
 *
 * Bits [63:58] = node_id  (6 bits, 64 nodes)
 * Bits [57:56] = tier_id  (2 bits, 4 tiers)
 * Bits [55:0]  = offset   (56 bits, 64 PB per tier)
 * ======================================================================== */

typedef uint64_t gpa_t;
typedef uint8_t  node_id_t;
typedef uint64_t chunk_id_t;
typedef uint8_t  tier_id_t;

#define GPA_NODE_SHIFT   58
#define GPA_TIER_SHIFT   56
#define GPA_NODE_MASK    0xFC00000000000000ULL
#define GPA_TIER_MASK    0x0300000000000000ULL
#define GPA_OFFSET_MASK  0x00FFFFFFFFFFFFFFULL

/* Tier IDs */
#define UMM_TIER_DRAM    0
#define UMM_TIER_CXL     1
#define UMM_TIER_SSD     2
#define UMM_TIER_RESV    3
#define UMM_NUM_TIERS    4

static inline node_id_t gpa_to_node(gpa_t gpa) {
    return (node_id_t)((gpa & GPA_NODE_MASK) >> GPA_NODE_SHIFT);
}

static inline tier_id_t gpa_to_tier(gpa_t gpa) {
    return (tier_id_t)((gpa & GPA_TIER_MASK) >> GPA_TIER_SHIFT);
}

static inline uint64_t gpa_to_offset(gpa_t gpa) {
    return gpa & GPA_OFFSET_MASK;
}

static inline gpa_t make_gpa(node_id_t node, tier_id_t tier, uint64_t offset) {
    return ((gpa_t)node << GPA_NODE_SHIFT)
         | ((gpa_t)tier << GPA_TIER_SHIFT)
         | (offset & GPA_OFFSET_MASK);
}

/* ========================================================================
 * Error codes
 * ======================================================================== */

#define UMM_OK                0
#define UMM_E_INVALID_ARG    -1
#define UMM_E_NOT_FOUND      -2
#define UMM_E_NO_MEMORY      -3
#define UMM_E_ALREADY_EXISTS -4
#define UMM_E_RPC_ERROR      -5
#define UMM_E_TIMEOUT        -6
#define UMM_E_TRANSPORT_ERROR -7
#define UMM_E_NOT_INITIALIZED -8
#define UMM_E_UNKNOWN        -99

/* Aliases for backward compatibility */
#define UMM_E_INVALID    UMM_E_INVALID_ARG
#define UMM_E_NOMEM      UMM_E_NO_MEMORY
#define UMM_E_IO         UMM_E_TRANSPORT_ERROR
#define UMM_E_REGION_FULL UMM_E_NO_MEMORY

const char* umm_error_string(int code);
const char* umm_tier_name(tier_id_t tier);  /* "DRAM"/"CXL"/"SSD"/"???" */

/* ========================================================================
 * Data structures
 * ======================================================================== */

/* ====================================================================
 * Storage Resource — describes a storage device for a tier on a node
 * ==================================================================== */

typedef struct {
    tier_id_t   tier;
    char        device_path[256];   /* /dev/cxl/mem0 or /tmp/umm_ssd */
    uint64_t    capacity;           /* total capacity (bytes) */
    uint64_t    base_offset;        /* starting offset in unified address space */
    int         online;             /* 1=available, 0=offline */
} StorageResource;

typedef struct {
    node_id_t          node_id;
    uint32_t           num_resources;
    StorageResource    resources[UMM_NUM_TIERS];  /* one per tier */
} StorageTopology;

/* ChunkDescriptor — client-facing handle for a chunk */
typedef struct {
    chunk_id_t  chunk_id;
    gpa_t       base_gpa;
    uint64_t    user_size;
} ChunkDescriptor;

/* ChunkMetadata — stored in ummd's chunk table */
typedef struct {
    chunk_id_t  chunk_id;
    char        name[64];
    gpa_t       gpa;
    uint64_t    size;
    tier_id_t   primary_tier;
    uint8_t     has_ssd_copy;
} ChunkMetadata;

/* SSD device entry for multi-device configuration */
#define UMM_MAX_SSD_DEVICES 16
typedef struct {
    char     path[256];     /* Device file path */
    uint64_t size;          /* Device capacity in bytes */
} SsdDeviceConfig;

/* UMMConfig — system configuration */
typedef struct {
    char      transport[16];
    char      consistency_model[16];
    uint64_t  memory_size;
    char      meta_server_addr[256];
    char      mem_server_addr[256];
    char      cxl_device[256];
    char      ssd_device[256];   /* Legacy: single SSD device path */
    node_id_t my_node_id;

    /* Multi-SSD device list (parsed from ssd_devices config key) */
    SsdDeviceConfig ssd_devices[UMM_MAX_SSD_DEVICES];
    uint32_t        num_ssd_devices;

    /* Server listening config (for ummd/umms standalone mode) */
    uint16_t        listen_port;         /* listening port (0 = use default) */
    uint64_t        base_gpa;            /* base GPA for this node (default: 0) */
} UMMConfig;

/* ========================================================================
 * Transport vtable (public reference)
 * ======================================================================== */

struct MemoryTransportVtbl;

/* ========================================================================
 * Allocation flags
 * ======================================================================== */

#define UMM_ALLOC_DEFAULT     0x00
#define UMM_ALLOC_TEMP        0x00
#define UMM_ALLOC_BUFFER      0x01
#define UMM_ALLOC_PERSISTENT  0x02

/* ========================================================================
 * Public API
 * ======================================================================== */

/* System init/deinit */
int  umm_init(const UMMConfig *cfg);
void umm_deinit(void);

/* Chunk allocation */
int umm_alloc(uint64_t size, ChunkDescriptor *out);
int umm_alloc_ex(uint64_t size, uint32_t flags, ChunkDescriptor *out);
int umm_alloc_tiered(uint64_t size, tier_id_t tier, ChunkDescriptor *out);
int umm_free(ChunkDescriptor *desc);

/* Storage resource management */
int umm_register_storage_tier(tier_id_t tier, const char *device_path,
                                uint64_t capacity);

/* 查询存储拓扑：direct 模式查本地 mem_service；RPC 模式查 ummD。
 * 注意布局：resources 可能按 tier 稀疏存放，遍历应取 [0,UMM_NUM_TIERS)
 * 全部槽位并过滤 online，而非仅遍历前 num_resources 项。 */
int umm_get_topology(StorageTopology *out);

/* Data I/O */
int umm_read (const ChunkDescriptor *desc, uint64_t offset, uint64_t len, void *out_buf);
int umm_write(const ChunkDescriptor *desc, uint64_t offset, uint64_t len, const void *buf);

/* Atomic operations */
int umm_atomic_cas(const ChunkDescriptor *desc, uint64_t offset,
                    uint64_t expected, uint64_t desired, uint64_t *old);
int umm_atomic_fetch_add(const ChunkDescriptor *desc, uint64_t offset,
                          uint64_t value, uint64_t *result);
int umm_atomic_set(const ChunkDescriptor *desc, uint64_t offset, uint64_t value);

/* Cross-node chunk lookup */
int umm_lookup_chunk(const char *name, ChunkMetadata *out);

/* Multi-tier API */
int umm_persist(const ChunkDescriptor *desc);
int umm_migrate_tier(ChunkDescriptor *desc, tier_id_t tier);
int umm_read_tiered(const ChunkDescriptor *desc, uint64_t offset,
                     uint64_t len, void *out_buf);
int umm_write_tiered(const ChunkDescriptor *desc, uint64_t offset,
                      uint64_t len, const void *buf);

/* Sync */
void umm_fence(void);
void umm_barrier_all(void);
void umm_quiet(void);

#ifdef __cplusplus
}
#endif

#endif /* UMM_H */
