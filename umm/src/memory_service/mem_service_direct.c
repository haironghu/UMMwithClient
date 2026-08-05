/* ========================================================================
 * mem_service_direct.c -- Multi-tier memory service (storage device owner)
 *
 * This module owns the storage devices:
 *   - CXL (tier 1): opens /dev/cxl/mem*, mmap()
 *   - SSD (tier 2): manages multi-device pool via ssd_pool
 *   - MOCK/DRAM (tier 0): malloc() backing buffer
 *
 * Backward compatibility: alloc_local / free_local / get_stats default to
 * UMM_TIER_CXL.  The old 4-arg factory is a thin wrapper around the new
 * mem_service_direct_create_v2().
 * ======================================================================== */

#include "mem_service.h"
#include "mem_service_direct.h"

#include "../../include/umm.h"
#include "../common/types.h"
#include "../common/error_codes.h"
#include "../common/bitmap_allocator.h"
#include "../common/log.h"

#include "../transport/ssd_pool.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>

/* Default page size used when creating bitmap allocators */
#define MEMSVC_DEFAULT_PAGE_SIZE 4096ULL

/* ========================================================================
 * Per-tier context
 * ======================================================================== */

typedef struct {
    BitmapAllocator *allocator;      /* page allocator for this tier */
    uint64_t         total_size;     /* registered capacity (bytes) */
    uint64_t         base_offset;    /* starting offset in unified address space */
    char             device_path[256];
    int              online;         /* 1=available, 0=offline/not registered */
    void            *mmap_base;      /* CXL: mmap base; MOCK: malloc ptr; SSD: NULL */
    int              mmap_fd;        /* CXL: device fd; SSD/MOCK: -1 */
    SsdPool         *ssd_pool;       /* SSD tier: multi-device pool */
} TierMemCtx;

/* ========================================================================
 * Global memory-service context
 * ======================================================================== */

typedef struct {
    TierMemCtx       tiers[UMM_NUM_TIERS];  /* one per tier */
    node_id_t        node_id;
    uint64_t         alloc_count;
    uint64_t         free_count;
    pthread_mutex_t  lock;
} MemServiceCtx;

/* ========================================================================
 * Helpers
 * ======================================================================== */

static inline int tier_is_valid(tier_id_t tier)
{
    return (tier < UMM_NUM_TIERS);
}

/* Ensure the backing store for a CXL tier is mmap'd (lazy on first alloc) */
static int ensure_cxl_mmap(TierMemCtx *tier)
{
    if (tier->mmap_base != NULL)
        return UMM_OK;

    /* device_path is "" for MOCK mode -> use malloc */
    if (tier->device_path[0] == '\0' || strcmp(tier->device_path, "mock") == 0) {
        tier->mmap_base = malloc((size_t)tier->total_size);
        if (!tier->mmap_base)
            return UMM_E_NO_MEMORY;
        memset(tier->mmap_base, 0, (size_t)tier->total_size);
        tier->mmap_fd = -1;
        umm_log_info("memsvc: tier CXL using malloc backing (%lu bytes)",
                     (unsigned long)tier->total_size);
        return UMM_OK;
    }

    /* Real CXL device */
    int fd = open(tier->device_path, O_RDWR);
    if (fd < 0) {
        umm_log_warn("memsvc: cannot open CXL device %s (%s), "
                     "falling back to malloc backing",
                     tier->device_path, strerror(errno));
        /* Fallback to malloc (test mode or device unavailable) */
        tier->mmap_base = malloc((size_t)tier->total_size);
        if (!tier->mmap_base)
            return UMM_E_NO_MEMORY;
        memset(tier->mmap_base, 0, (size_t)tier->total_size);
        tier->mmap_fd = -1;
        return UMM_OK;
    }

    void *map = mmap(NULL, (size_t)tier->total_size,
                     PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        umm_log_error("memsvc: cannot mmap CXL device %s: %s",
                      tier->device_path, strerror(errno));
        close(fd);
        tier->online = 0;
        return UMM_E_TRANSPORT_ERROR;
    }

    tier->mmap_base = map;
    tier->mmap_fd   = fd;
    umm_log_info("memsvc: tier CXL mmap'd %s (%lu bytes)",
                 tier->device_path, (unsigned long)tier->total_size);
    return UMM_OK;
}

/* ========================================================================
 * VTable: tier-aware allocation
 * ======================================================================== */

static int memsvc_alloc_tiered(void *ctx, tier_id_t tier, uint64_t size,
                                uint64_t *out_offset)
{
    if (!ctx || !out_offset || size == 0)
        return UMM_E_INVALID_ARG;
    if (!tier_is_valid(tier))
        return UMM_E_INVALID_ARG;

    MemServiceCtx *m = (MemServiceCtx *)ctx;

    pthread_mutex_lock(&m->lock);

    TierMemCtx *t = &m->tiers[tier];
    if (!t->online || !t->allocator) {
        pthread_mutex_unlock(&m->lock);
        return UMM_E_NOT_INITIALIZED;
    }

    /* Lazy mmap for CXL tier */
    if (tier == UMM_TIER_CXL) {
        int rc = ensure_cxl_mmap(t);
        if (rc != UMM_OK) {
            pthread_mutex_unlock(&m->lock);
            return rc;
        }
    }

    /* SSD tier：池自有虚拟位图即分配权威（覆盖全部设备），
     * 不再与 tier ba 双写——ba 仅按首设备容量创建，多设备池下
     * 会把 >首设备容量的合法分配误判为 NO_MEMORY。 */
    if (tier == UMM_TIER_SSD && t->ssd_pool) {
        uint64_t voffset = 0;
        int rc = ssd_pool_alloc(t->ssd_pool, size, &voffset);
        if (rc == UMM_OK) {
            *out_offset = t->base_offset + voffset;
            m->alloc_count++;
            umm_log_debug("memsvc: SSD chunk at voffset=0x%lx, "
                          "global=0x%lx, size=%lu",
                          (unsigned long)voffset,
                          (unsigned long)*out_offset,
                          (unsigned long)size);
        }
        pthread_mutex_unlock(&m->lock);
        return rc;
    }

    uint64_t offset_within_tier = 0;
    int rc = ba_alloc(t->allocator, size, &offset_within_tier);
    if (rc == UMM_OK) {
        *out_offset = t->base_offset + offset_within_tier;
        m->alloc_count++;
    }

    pthread_mutex_unlock(&m->lock);
    return rc;
}

static int memsvc_free_tiered(void *ctx, tier_id_t tier, uint64_t offset,
                               uint64_t size)
{
    if (!ctx || size == 0)
        return UMM_E_INVALID_ARG;
    if (!tier_is_valid(tier))
        return UMM_E_INVALID_ARG;

    MemServiceCtx *m = (MemServiceCtx *)ctx;

    pthread_mutex_lock(&m->lock);

    TierMemCtx *t = &m->tiers[tier];
    if (!t->online || !t->allocator) {
        pthread_mutex_unlock(&m->lock);
        return UMM_E_NOT_INITIALIZED;
    }

    /* SSD tier：与 alloc 对称，只走池位图 */
    if (tier == UMM_TIER_SSD && t->ssd_pool) {
        int rc = ssd_pool_free(t->ssd_pool, offset - t->base_offset, size);
        if (rc == UMM_OK) {
            m->free_count++;
            umm_log_debug("memsvc: SSD chunk freed at voffset 0x%lx",
                          (unsigned long)(offset - t->base_offset));
        }
        pthread_mutex_unlock(&m->lock);
        return rc;
    }

    uint64_t offset_within_tier = offset - t->base_offset;
    int rc = ba_free(t->allocator, offset_within_tier, size);
    if (rc == UMM_OK)
        m->free_count++;

    pthread_mutex_unlock(&m->lock);
    return rc;
}

static int memsvc_get_tier_stats(void *ctx, tier_id_t tier,
                                  uint64_t *total, uint64_t *used,
                                  uint64_t *free_mem)
{
    if (!ctx || !total || !used || !free_mem)
        return UMM_E_INVALID_ARG;
    if (!tier_is_valid(tier))
        return UMM_E_INVALID_ARG;

    MemServiceCtx *m = (MemServiceCtx *)ctx;

    pthread_mutex_lock(&m->lock);

    TierMemCtx *t = &m->tiers[tier];
    if (!t->online || !t->allocator) {
        pthread_mutex_unlock(&m->lock);
        return UMM_E_NOT_INITIALIZED;
    }

    /* SSD tier：以池虚拟位图为权威（ba 仅按首设备容量创建，
     * 多设备池下 ba 统计失真） */
    if (tier == UMM_TIER_SSD && t->ssd_pool) {
        uint64_t pool_total = 0, pool_free = 0;
        ssd_pool_get_usage(t->ssd_pool, &pool_total, &pool_free);
        *total    = pool_total;
        *used     = pool_total - pool_free;
        *free_mem = pool_free;
        pthread_mutex_unlock(&m->lock);
        return UMM_OK;
    }

    uint64_t free_pages = ba_get_free_pages(t->allocator);
    uint64_t page_size  = MEMSVC_DEFAULT_PAGE_SIZE;
    uint64_t free_bytes = free_pages * page_size;
    uint64_t used_bytes = t->total_size - free_bytes;

    *total    = t->total_size;
    *used     = used_bytes;
    *free_mem = free_bytes;

    pthread_mutex_unlock(&m->lock);
    return UMM_OK;
}

/* ========================================================================
 * VTable: storage resource management
 * ======================================================================== */

static int memsvc_register_storage(void *ctx, const StorageResource *res)
{
    if (!ctx || !res)
        return UMM_E_INVALID_ARG;
    if (!tier_is_valid(res->tier))
        return UMM_E_INVALID_ARG;

    /* ---- SSD：解析设备路径 ----
     * 仅支持三类后端：
     *   libnvm:<ctrl>@<ns>  —— libnvm 用户态驱动（真实硬件，原样使用）
     *   nds:<dev_id>[+<off>] —— NDS NPU 直驱后端（真实硬件，原样使用）
     *   文件/目录          —— 模拟盘（*.raw 原样；目录 → dir/pool.raw）
     * 内核块设备（/dev/nvmeXnY）不再支持：规避误写系统盘/数据盘风险。
     * 容量一律必须显式配置。 */
    char     ssd_resolved[288] = {0};
    uint64_t eff_capacity = res->capacity;
    if (res->tier == UMM_TIER_SSD && res->device_path[0] != '\0') {
        if (strncmp(res->device_path, "libnvm:", 7) == 0 ||
            strncmp(res->device_path, "nds:", 4) == 0 ||
            /* nds-meta 纯分配后端（umms 簿记形态），同样原样透传 */
            strncmp(res->device_path, "nds-meta:", 9) == 0) {
            snprintf(ssd_resolved, sizeof(ssd_resolved), "%s",
                     res->device_path);
        } else {
            size_t dplen = strlen(res->device_path);
            int is_file = (dplen > 4 &&
                           strcmp(res->device_path + dplen - 4, ".raw") == 0);
            /* 内核块设备（QEMU/SAN 共享盘实验场景）：默认沿用"不支持"
             * 保护（防误写系统盘），仅在显式 UMM_ALLOW_BLOCK_DEVICE=1
             * 时原样透传；后端打开侧已支持 S_ISBLK（跳过 ftruncate） */
            struct stat st;
            if (!is_file && stat(res->device_path, &st) == 0 &&
                S_ISBLK(st.st_mode)) {
                const char *allow = getenv("UMM_ALLOW_BLOCK_DEVICE");
                if (!allow || strcmp(allow, "1") != 0) {
                    umm_log_error("memsvc: block device %s rejected "
                                  "(export UMM_ALLOW_BLOCK_DEVICE=1 to "
                                  "opt in, 确认非系统盘/数据盘)",
                                  res->device_path);
                    return UMM_E_INVALID_ARG;
                }
                umm_log_warn("memsvc: BLOCK DEVICE %s opt-in 放行，"
                             "将直接读写该设备（capacity=%lu）",
                             res->device_path,
                             (unsigned long)eff_capacity);
                snprintf(ssd_resolved, sizeof(ssd_resolved), "%s",
                         res->device_path);
            } else if (is_file) {
                snprintf(ssd_resolved, sizeof(ssd_resolved), "%s",
                         res->device_path);
            } else {
                snprintf(ssd_resolved, sizeof(ssd_resolved), "%s/pool.raw",
                         res->device_path);
            }
        }
        if (eff_capacity == 0) {
            umm_log_error("memsvc: SSD backend requires explicit "
                          "capacity (path=%s)", res->device_path);
            return UMM_E_INVALID_ARG;
        }
    }

    if (res->capacity == 0)
        return UMM_E_INVALID_ARG;

    MemServiceCtx *m = (MemServiceCtx *)ctx;

    pthread_mutex_lock(&m->lock);

    tier_id_t tier = res->tier;
    TierMemCtx *t = &m->tiers[tier];
    int       first_reg = !t->online;

    /* First-time registration: initialize tier */
    if (!t->online) {
        t->total_size  = (tier == UMM_TIER_SSD) ? eff_capacity : res->capacity;
        t->base_offset = res->base_offset;
        t->online      = 1;
        t->mmap_base   = NULL;
        t->mmap_fd     = -1;
        t->ssd_pool    = NULL;

        /* Create bitmap allocator（SSD 用探测后的有效容量） */
        t->allocator = ba_create(t->total_size, MEMSVC_DEFAULT_PAGE_SIZE);
        if (!t->allocator) {
            t->online = 0;
            pthread_mutex_unlock(&m->lock);
            return UMM_E_NO_MEMORY;
        }
    }

    /* Copy latest device_path (for reference) */
    size_t dp_len = strlen(res->device_path);
    if (dp_len >= sizeof(t->device_path))
        dp_len = sizeof(t->device_path) - 1;
    memcpy(t->device_path, res->device_path, dp_len);
    t->device_path[dp_len] = '\0';

    /* Tier-specific device initialization */
    if (tier == UMM_TIER_CXL) {
        /* CXL: defer mmap until first allocation (lazy) */
        umm_log_info("memsvc: registered CXL tier, capacity=%lu, base_offset=0x%lx, dev=%s",
                     (unsigned long)t->total_size,
                     (unsigned long)t->base_offset,
                     t->device_path[0] ? t->device_path : "(mock)");

    } else if (tier == UMM_TIER_SSD) {
        /* SSD: create backend (file / dir / libnvm) */
        if (t->device_path[0] != '\0') {
            /* Create pool on first registration, add device on subsequent */
            if (!t->ssd_pool) {
                t->ssd_pool = ssd_pool_create();
                if (!t->ssd_pool) {
                    ba_destroy(t->allocator);
                    t->allocator = NULL;
                    t->online = 0;
                    pthread_mutex_unlock(&m->lock);
                    return UMM_E_NO_MEMORY;
                }
            }

            /* 使用注册入口预解析好的路径（块设备原样 / 文件 / 目录） */
            int rc = ssd_pool_add_device(t->ssd_pool, ssd_resolved,
                                         eff_capacity);
            if (rc != UMM_OK) {
                if (ssd_pool_num_devices(t->ssd_pool) == 0) {
                    ssd_pool_destroy(t->ssd_pool);
                    t->ssd_pool = NULL;
                    ba_destroy(t->allocator);
                    t->allocator = NULL;
                    t->online = 0;
                }
                pthread_mutex_unlock(&m->lock);
                return rc;
            }
            /* 第 2+ 个设备：容量累加进 tier 总量（首设备在初始化分支） */
            if (!first_reg)
                t->total_size += eff_capacity;
        }
        umm_log_info("memsvc: registered SSD tier, capacity=%lu, "
                     "base_offset=0x%lx, path=%s, devices=%u",
                     (unsigned long)t->total_size,
                     (unsigned long)t->base_offset,
                     t->device_path,
                     ssd_pool_num_devices(t->ssd_pool));

    } else if (tier == UMM_TIER_DRAM) {
        if (t->device_path[0] != '\0') {
            /* Phase 2.5 共享内存窗口（virtio-pmem 等，/dev/pmem0）：
             * open + mmap(MAP_SHARED) 作为 DRAM tier 后备。显式给了设备就
             * 绝不回退 malloc——静默回退会把"共享"无声退化成"私有"。
             * （CXL tier 保留 lazy+malloc 回退的旧行为，那是路线A语义；
             *  其共享窗口场景由 06 断言"无 fallback 日志"来守护。） */
            int fd = open(t->device_path, O_RDWR);
            if (fd < 0) {
                umm_log_error("memsvc: DRAM tier device %s open failed: %s",
                              t->device_path, strerror(errno));
                ba_destroy(t->allocator);
                t->allocator = NULL;
                t->online = 0;
                pthread_mutex_unlock(&m->lock);
                return UMM_E_NOT_FOUND;
            }
            void *map = mmap(NULL, (size_t)t->total_size,
                             PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (map == MAP_FAILED) {
                umm_log_error("memsvc: DRAM tier device %s mmap(%lu) failed: %s",
                              t->device_path, (unsigned long)t->total_size,
                              strerror(errno));
                close(fd);
                ba_destroy(t->allocator);
                t->allocator = NULL;
                t->online = 0;
                pthread_mutex_unlock(&m->lock);
                return UMM_E_NO_MEMORY;
            }
            t->mmap_base = map;
            t->mmap_fd   = fd;
            umm_log_info("memsvc: registered DRAM tier, capacity=%lu, "
                         "base_offset=0x%lx, dev=%s (shared window)",
                         (unsigned long)t->total_size,
                         (unsigned long)t->base_offset, t->device_path);
        } else {
            /* MOCK/DRAM: malloc backing buffer immediately */
            t->mmap_base = malloc((size_t)t->total_size);
            if (!t->mmap_base) {
                ba_destroy(t->allocator);
                t->allocator = NULL;
                t->online = 0;
                pthread_mutex_unlock(&m->lock);
                return UMM_E_NO_MEMORY;
            }
            memset(t->mmap_base, 0, (size_t)t->total_size);
            t->mmap_fd = -1;
            umm_log_info("memsvc: registered DRAM tier, capacity=%lu, base_offset=0x%lx",
                         (unsigned long)t->total_size,
                         (unsigned long)t->base_offset);
        }

    } else {
        /* Reserved tier - not supported */
        ba_destroy(t->allocator);
        t->allocator = NULL;
        t->online = 0;
        pthread_mutex_unlock(&m->lock);
        return UMM_E_INVALID_ARG;
    }

    pthread_mutex_unlock(&m->lock);
    return UMM_OK;
}

static int memsvc_get_topology(void *ctx, StorageTopology *out)
{
    if (!ctx || !out)
        return UMM_E_INVALID_ARG;

    MemServiceCtx *m = (MemServiceCtx *)ctx;

    pthread_mutex_lock(&m->lock);

    out->node_id = m->node_id;
    out->num_resources = 0;

    for (int i = 0; i < UMM_NUM_TIERS; i++) {
        TierMemCtx *t = &m->tiers[i];
        if (t->online) {
            StorageResource *r = &out->resources[out->num_resources];
            r->tier        = (tier_id_t)i;
            size_t dp_len = strlen(t->device_path);
            if (dp_len >= sizeof(r->device_path))
                dp_len = sizeof(r->device_path) - 1;
            memcpy(r->device_path, t->device_path, dp_len);
            r->device_path[dp_len] = '\0';
            r->capacity    = t->total_size;
            r->base_offset = t->base_offset;
            r->online      = 1;
            out->num_resources++;
        }
    }

    pthread_mutex_unlock(&m->lock);
    return UMM_OK;
}

/* ========================================================================
 * VTable: device mapping
 * ======================================================================== */

static int memsvc_map_device(void *ctx, tier_id_t tier, node_id_t node,
                              uint64_t offset, uint64_t size, void **out_ptr)
{
    /* size：SSD 池 mmap 快路径需校验跨设备跨度（见下） */
    (void)node;  /* node not used in new ssd_backend API */

    if (!ctx || !out_ptr)
        return UMM_E_INVALID_ARG;
    if (!tier_is_valid(tier))
        return UMM_E_INVALID_ARG;

    MemServiceCtx *m = (MemServiceCtx *)ctx;

    pthread_mutex_lock(&m->lock);

    TierMemCtx *t = &m->tiers[tier];
    if (!t->online) {
        pthread_mutex_unlock(&m->lock);
        return UMM_E_NOT_INITIALIZED;
    }

    /* Lazy mmap for CXL tier (map_device may be called before alloc) */
    if (tier == UMM_TIER_CXL) {
        int rc = ensure_cxl_mmap(t);
        if (rc != UMM_OK) {
            pthread_mutex_unlock(&m->lock);
            return rc;
        }
    }

    uint64_t offset_within_tier = offset - t->base_offset;
    void *ptr = NULL;

    if (tier == UMM_TIER_SSD && t->ssd_pool) {
        /* mmap 快路径仅当 [offset, offset+size) 完全落在单个设备内；
         * 跨界/越界返回 INVALID，调用方回退 ssd_read/ssd_write
         * （host 通路自带跨设备分段与边界检查）。
         * 修复前多设备池下 map+memcpy 可冲出设备映射（SEGV）。 */
        if (size > 0 &&
            ssd_pool_span_in_one_device(t->ssd_pool, offset_within_tier,
                                        size) != UMM_OK) {
            pthread_mutex_unlock(&m->lock);
            return UMM_E_INVALID_ARG;
        }
        /* Translate virtual offset to (device, physical_offset) → pointer */
        ptr = ssd_pool_get_ptr(t->ssd_pool, offset_within_tier);
        if (!ptr) {
            pthread_mutex_unlock(&m->lock);
            return UMM_E_NOT_FOUND;
        }
    } else {
        /* CXL / DRAM / MOCK: direct pointer from mmap_base */
        if (!t->mmap_base) {
            pthread_mutex_unlock(&m->lock);
            return UMM_E_NOT_INITIALIZED;
        }
        ptr = (char *)t->mmap_base + offset_within_tier;
    }

    *out_ptr = ptr;

    pthread_mutex_unlock(&m->lock);
    return UMM_OK;
}

static int memsvc_unmap_device(void *ctx, tier_id_t tier, node_id_t node,
                                uint64_t offset, uint64_t size)
{
    (void)ctx;
    (void)tier;
    (void)node;
    (void)offset;
    (void)size;
    /* Currently a no-op.  munmap is done only at destroy time. */
    return UMM_OK;
}

/* ========================================================================
 * VTable: backward-compatible local functions (default to CXL tier)
 * ======================================================================== */

static int memsvc_alloc_local(void *ctx, uint64_t size, uint64_t *out_offset)
{
    return memsvc_alloc_tiered(ctx, UMM_TIER_CXL, size, out_offset);
}

static int memsvc_free_local(void *ctx, uint64_t offset, uint64_t size)
{
    return memsvc_free_tiered(ctx, UMM_TIER_CXL, offset, size);
}

static int memsvc_get_stats(void *ctx, uint64_t *total, uint64_t *used,
                             uint64_t *free_mem)
{
    return memsvc_get_tier_stats(ctx, UMM_TIER_CXL, total, used, free_mem);
}

/* ========================================================================
 * Static vtable instance
 * ======================================================================== */

/* ========================================================================
 * 可选接口：SSD 块设备后端的主机 I/O 回退通路（pread/pwrite 语义）
 * 当 SSD tier 由真实块设备纳管（无 mmap）时，transport 经此读写。
 * ======================================================================== */

static int memsvc_ssd_io(void *ctx, tier_id_t tier, node_id_t node,
                         uint64_t offset, uint64_t len, void *buf,
                         int is_write)
{
    (void)node;
    if (!ctx || (!buf && len > 0))
        return UMM_E_INVALID_ARG;
    if (tier != UMM_TIER_SSD)
        return UMM_E_INVALID_ARG;

    MemServiceCtx *m = (MemServiceCtx *)ctx;

    /* 锁内仅解析 pool 指针与基址（注册后不变），I/O 移到锁外——
     * 同步 nvm_host_read/write 长达数十~数百 us，持锁会把所有并发
     * SSD 读写串行化（真机实测并发 0.88x 的根因之一） */
    pthread_mutex_lock(&m->lock);

    TierMemCtx *t = &m->tiers[tier];
    if (!t->online || !t->ssd_pool) {
        pthread_mutex_unlock(&m->lock);
        return UMM_E_NOT_INITIALIZED;
    }

    SsdPool *pool = t->ssd_pool;
    uint64_t voff = offset - t->base_offset;   /* 与 map_device 同一语义 */

    pthread_mutex_unlock(&m->lock);

    return is_write ? ssd_pool_pwrite(pool, voff, len, buf)
                    : ssd_pool_pread (pool, voff, len, buf);
}

static int memsvc_ssd_read(void *ctx, tier_id_t tier, node_id_t node,
                           uint64_t offset, uint64_t len, void *buf)
{
    return memsvc_ssd_io(ctx, tier, node, offset, len, buf, 0);
}

static int memsvc_ssd_write(void *ctx, tier_id_t tier, node_id_t node,
                            uint64_t offset, uint64_t len, const void *buf)
{
    return memsvc_ssd_io(ctx, tier, node, offset, len, (void *)buf, 1);
}

static MemoryServiceVtbl g_direct_vtbl = {
    .alloc_local       = memsvc_alloc_local,
    .free_local        = memsvc_free_local,
    .get_stats         = memsvc_get_stats,
    .alloc_tiered      = memsvc_alloc_tiered,
    .free_tiered       = memsvc_free_tiered,
    .get_tier_stats    = memsvc_get_tier_stats,
    .register_storage  = memsvc_register_storage,
    .get_topology      = memsvc_get_topology,
    .map_device        = memsvc_map_device,
    .unmap_device      = memsvc_unmap_device,
    .ssd_read          = memsvc_ssd_read,
    .ssd_write         = memsvc_ssd_write,
};

/* ========================================================================
 * Public API: new v2 factory (creates empty multi-tier context)
 * ======================================================================== */

MemoryServiceVtbl* mem_service_direct_create_v2(node_id_t node_id, void **out_ctx)
{
    if (!out_ctx)
        return NULL;

    MemServiceCtx *ctx = calloc(1, sizeof(MemServiceCtx));
    if (!ctx)
        return NULL;

    ctx->node_id = node_id;
    ctx->alloc_count = 0;
    ctx->free_count  = 0;

    /* Mark all tiers offline by default (calloc already zeros everything) */
    for (int i = 0; i < UMM_NUM_TIERS; i++) {
        ctx->tiers[i].online   = 0;
        ctx->tiers[i].mmap_fd  = -1;
    }

    if (pthread_mutex_init(&ctx->lock, NULL) != 0) {
        free(ctx);
        return NULL;
    }

    *out_ctx = ctx;
    return &g_direct_vtbl;
}

/* ========================================================================
 * Public API: backward-compatible wrapper (old 4-arg signature)
 *
 * Creates a multi-tier context and auto-registers one CXL tier so that
 * existing callers (mem_server.c:242) continue to work unchanged.
 * ======================================================================== */

MemoryServiceVtbl* mem_service_direct_create(node_id_t node_id,
                                              uint64_t memory_size,
                                              uint64_t base_gpa,
                                              void **out_ctx)
{
    return mem_service_direct_create_tiered(node_id, memory_size, base_gpa,
                                            UMM_TIER_CXL, NULL, out_ctx);
}

/* ========================================================================
 * Public API: tier-selectable variant（Phase 2 混合池）
 *
 * tier = UMM_TIER_CXL（默认，mock/真实 CXL 语义不变）
 *      = UMM_TIER_DRAM（无 CXL 硬件的 DRAM 层：注册即 malloc 后备）
 * mem_device = 内存层后备设备（Phase 2.5 共享内存窗口，如 /dev/pmem0）；
 *              NULL/"" = 旧行为。
 * ======================================================================== */

MemoryServiceVtbl* mem_service_direct_create_tiered(node_id_t node_id,
                                                     uint64_t memory_size,
                                                     uint64_t base_gpa,
                                                     tier_id_t tier,
                                                     const char *mem_device,
                                                     void **out_ctx)
{
    if (!out_ctx || memory_size == 0)
        return NULL;
    if (tier != UMM_TIER_CXL && tier != UMM_TIER_DRAM)
        return NULL;

    MemoryServiceVtbl *vtbl = mem_service_direct_create_v2(node_id, out_ctx);
    if (!vtbl || !*out_ctx)
        return NULL;

    /* Auto-register the requested memory tier with the legacy parameters.
     * CXL: device_path="mock"（lazy mmap→malloc 回退）；
     * DRAM: device_path=""（DRAM 分支忽略 path，注册即 malloc 后备）。
     * mem_device 非空时两个 tier 都改用显式设备后备。 */
    StorageResource res = {
        .tier        = tier,
        .capacity    = memory_size,
        .base_offset = base_gpa,
        .online      = 1,
    };
    if (mem_device && mem_device[0] != '\0') {
        strncpy(res.device_path, mem_device, sizeof(res.device_path) - 1);
        res.device_path[sizeof(res.device_path) - 1] = '\0';
    } else if (tier == UMM_TIER_CXL) {
        strncpy(res.device_path, "mock", sizeof(res.device_path) - 1);
        res.device_path[sizeof(res.device_path) - 1] = '\0';
    }

    int rc = vtbl->register_storage(*out_ctx, &res);
    if (rc != UMM_OK) {
        mem_service_direct_destroy(*out_ctx);
        *out_ctx = NULL;
        return NULL;
    }

    return vtbl;
}

/* ========================================================================
 * Public API: destroy
 * ======================================================================== */

/* ========================================================================
 * Accessor: SSD pool（供 transport_ssd 的 fence/invalidate 使用。
 * 数据面语义操作不属于 mem_service vtbl 的分配面职责，故以访问器暴露。）
 * ======================================================================== */

SsdPool *mem_service_direct_ssd_pool(void *ctx)
{
    MemServiceCtx *m = (MemServiceCtx *)ctx;
    if (!m)
        return NULL;
    return m->tiers[UMM_TIER_SSD].ssd_pool;
}

void mem_service_direct_destroy(void *ctx)
{
    if (!ctx)
        return;

    MemServiceCtx *m = (MemServiceCtx *)ctx;

    pthread_mutex_lock(&m->lock);

    for (int i = 0; i < UMM_NUM_TIERS; i++) {
        TierMemCtx *t = &m->tiers[i];

        if (t->allocator) {
            ba_destroy(t->allocator);
            t->allocator = NULL;
        }

        if (t->ssd_pool) {
            ssd_pool_destroy(t->ssd_pool);
            t->ssd_pool = NULL;
        }

        if (t->mmap_base) {
            if (t->mmap_fd >= 0) {
                /* 设备后备（CXL 真设备 / DRAM 共享窗口）：munmap + close */
                munmap(t->mmap_base, (size_t)t->total_size);
                close(t->mmap_fd);
            } else {
                /* MOCK/DRAM malloc buffer */
                free(t->mmap_base);
            }
            t->mmap_base = NULL;
            t->mmap_fd   = -1;
        }

        t->online = 0;
    }

    pthread_mutex_unlock(&m->lock);
    pthread_mutex_destroy(&m->lock);
    free(m);
}
