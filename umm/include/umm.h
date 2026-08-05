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

/* 哨兵值：ssd_owner_node / alloc 响应属主节点 "未知" 标识 */
#define UMM_NODE_UNKNOWN   0xFF
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
#define UMM_E_UNSUPPORTED   -9
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

    /* NDS RPC server（Process A）托管配置——仅 umms 服务层使用，
     * 全部可选（缺省：enable=0, ctrl=/dev/libnvm_helper0, ns=1,
     * qd=64, socket=/tmp/nvm_host_rpc.sock, keep_alive=1）。
     * 库层（libumm.so）绝不拉起特权进程，见 umms.c 设计注释。 */
    int             nds_rpc_server_enable;      /* 0/1 */
    char            nds_rpc_server_ctrl[256];   /* NVMe 控制器路径 */
    uint32_t        nds_rpc_server_ns;          /* namespace id */
    uint32_t        nds_rpc_server_qd;          /* admin queue 深度 */
    char            nds_rpc_server_socket[256]; /* RPC unix socket */
    int             nds_rpc_server_keep_alive;  /* 1=umms 退出保留 server */

    /* ---- Phase 1: 多节点远程数据面（全部可选；缺省 = 单节点行为不变） ----
     * peer_nodes:    集群数据面对等表 "node:host:port,node:host:port,..."。
     *                client 侧用于把"非本节点 GPA"的 I/O 路由到属主节点。
     * rpc_token:     共享密钥（任意长度，线上只带 6 字节 FNV-1a 摘要）。
     *                服务端配置非空后，所有 mem 协议请求逐帧校验；
     *                client 侧配置同一密钥。空 = 不校验（旧行为）。
     * allow_cidrs:   umms 服务端 accept 白名单 "cidr,cidr,..."（IPv4）。
     *                空 = 全放行（旧行为）。仅 umms 使用。
     * ssd_owner_node:旧版服务端（alloc 响应不含属主节点）时 SSD GPA 的
     *                属主 node 回退值；UMM_NODE_UNKNOWN(0xFF) = 用 my_node_id
     *                （保持旧行为）。新服务端下此值仅作地址解析回退。
     * data_max_io:   单个数据面 RPC 的 payload 上限（字节）。
     *                0 = 默认 1MB；有效范围钳位 [4KB, 16MB]。双侧需一致
     *                （client 按它分块，server 按它拒绝超限帧）。 */
    char            peer_nodes[1024];
    char            rpc_token[64];
    char            allow_cidrs[512];
    uint8_t         ssd_owner_node;
    /* Phase 2 混合池：客户端本地内存数据面注册为 DRAM tier(tier=0)。
     * 0 = 默认（注册 CXL tier，旧行为；无 CXL 设备时 mock/malloc 后备）；
     * 1 = 注册 DRAM tier（无 CXL 硬件场景；tier_router DRAM 槽位生效，
     *     umm_alloc_tiered(size, UMM_TIER_DRAM) 的数据面落本地 malloc。
     *     注意此模式下 legacy umm_alloc()（tier 硬编码 CXL）不可用，
     *     请一律使用 umm_alloc_tiered。）
     * 占用原 _reserved_phase1[0]，结构体 sizeof 与字段偏移不变。 */
    uint8_t         local_mem_as_dram;
    uint8_t         _reserved_phase1[2];
    uint32_t        data_max_io;
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

/* umm_invalidate — 丢弃本进程对 chunk 区域的缓存页视图（仅 SSD tier）。
 * 共享盘读共享：对端 umm_write + umm_fence 落盘后，本端须先 invalidate
 * 再读。DRAM/CXL 层为 no-op（UMM_OK）。详见 umm_api.c 注释。 */
int umm_invalidate(const ChunkDescriptor *desc, uint64_t offset, uint64_t len);

#ifdef __cplusplus
}
#endif

#endif /* UMM_H */
