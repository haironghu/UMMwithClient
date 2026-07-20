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

    uint64_t offset_within_tier = 0;
    int rc = ba_alloc(t->allocator, size, &offset_within_tier);
    if (rc == UMM_OK) {
        *out_offset = t->base_offset + offset_within_tier;
        m->alloc_count++;

        /* For SSD tier: allocate from the multi-device pool.
         * ssd_pool_alloc returns a virtual offset across all devices. */
        if (tier == UMM_TIER_SSD && t->ssd_pool) {
            uint64_t voffset = 0;
            int ssd_rc = ssd_pool_alloc(t->ssd_pool, size, &voffset);
            if (ssd_rc != UMM_OK) {
                /* rollback bitmap allocation */
                ba_free(t->allocator, offset_within_tier, size);
                m->alloc_count--;
                pthread_mutex_unlock(&m->lock);
                return ssd_rc;
            }
            /* Virtual offset must match the tier-local offset */
            if (voffset != offset_within_tier) {
                umm_log_warn("memsvc: SSD pool alloc returned voffset %lu, "
                             "expected %lu",
                             (unsigned long)voffset,
                             (unsigned long)offset_within_tier);
            }
            umm_log_debug("memsvc: SSD chunk at voffset=0x%lx (tier_offset=0x%lx), "
                          "global=0x%lx, size=%lu",
                          (unsigned long)voffset,
                          (unsigned long)offset_within_tier,
                          (unsigned long)*out_offset, (unsigned long)size);
        }
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

    uint64_t offset_within_tier = offset - t->base_offset;
    int rc = ba_free(t->allocator, offset_within_tier, size);
    if (rc == UMM_OK) {
        m->free_count++;

        /* For SSD tier: free from multi-device pool */
        if (tier == UMM_TIER_SSD && t->ssd_pool) {
            ssd_pool_free(t->ssd_pool, offset, size);
            umm_log_debug("memsvc: SSD chunk freed at voffset 0x%lx",
                          (unsigned long)offset);
        }
    }

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
    if (res->capacity == 0)
        return UMM_E_INVALID_ARG;

    MemServiceCtx *m = (MemServiceCtx *)ctx;

    pthread_mutex_lock(&m->lock);

    tier_id_t tier = res->tier;
    TierMemCtx *t = &m->tiers[tier];

    /* First-time registration: initialize tier */
    if (!t->online) {
        t->total_size  = res->capacity;
        t->base_offset = res->base_offset;
        t->online      = 1;
        t->mmap_base   = NULL;
        t->mmap_fd     = -1;
        t->ssd_pool    = NULL;

        /* Create bitmap allocator */
        t->allocator = ba_create(res->capacity, MEMSVC_DEFAULT_PAGE_SIZE);
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
        /* SSD: create backend (device_path may be file or dir) */
        if (t->device_path[0] != '\0') {
            const char *dev = t->device_path;

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

            /* Resolve device path (directory → dir/pool.raw, file → as-is) */
            size_t dplen = strlen(dev);
            int is_file = (dplen > 4 && strcmp(dev + dplen - 4, ".raw") == 0);
            char resolved[288];
            if (is_file) {
                strncpy(resolved, dev, sizeof(resolved) - 1);
                resolved[sizeof(resolved) - 1] = '\0';
            } else {
                snprintf(resolved, sizeof(resolved), "%s/pool.raw", dev);
            }

            int rc = ssd_pool_add_device(t->ssd_pool, resolved, t->total_size);
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
        }
        umm_log_info("memsvc: registered SSD tier, capacity=%lu, "
                     "base_offset=0x%lx, path=%s, devices=%u",
                     (unsigned long)t->total_size,
                     (unsigned long)t->base_offset,
                     t->device_path,
                     ssd_pool_num_devices(t->ssd_pool));

    } else if (tier == UMM_TIER_DRAM) {
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
    (void)size;  /* unused: size validated by caller if needed */
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
    if (!out_ctx || memory_size == 0)
        return NULL;

    MemoryServiceVtbl *vtbl = mem_service_direct_create_v2(node_id, out_ctx);
    if (!vtbl || !*out_ctx)
        return NULL;

    /* Auto-register a CXL tier with the legacy parameters */
    StorageResource res = {
        .tier        = UMM_TIER_CXL,
        .capacity    = memory_size,
        .base_offset = base_gpa,
        .online      = 1,
    };
    strncpy(res.device_path, "mock", sizeof(res.device_path) - 1);
    res.device_path[sizeof(res.device_path) - 1] = '\0';

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
            if (i == UMM_TIER_CXL && t->mmap_fd >= 0) {
                /* Real CXL mmap */
                munmap(t->mmap_base, (size_t)t->total_size);
                if (t->mmap_fd >= 0)
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
