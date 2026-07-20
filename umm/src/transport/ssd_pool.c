/* ========================================================================
 * ssd_pool.c — Multi-device SSD pool with contiguous virtual address space
 *
 * Maps N physical SSD devices into a single contiguous virtual address space.
 * Each device has its own SsdBackend (bitmap allocator + mmap).
 * The pool adds a global virtual bitmap + virtual→physical translation layer.
 * ======================================================================== */

#include "ssd_pool.h"
#include "../common/error_codes.h"
#include "../common/log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>
#include <errno.h>

/* ========================================================================
 * SSD Backend implementation (merged from ssd_backend.c)
 * ======================================================================== */

#define SSD_PAGE_SIZE   4096
#define SSD_BITS_PER_U64 64

struct SsdBackend {
    char       *device_path;    /* path to device file */
    int         fd;             /* device file descriptor */
    void       *mmap_base;      /* mmap base address */
    uint64_t    capacity;       /* total capacity (bytes) */
    uint64_t    total_pages;    /* capacity / page_size */
    uint64_t    free_pages;     /* number of free pages */
    uint64_t   *bitmap;         /* allocation bitmap */
    uint64_t    bitmap_words;   /* number of uint64_t in bitmap */
    pthread_mutex_t lock;
};

/* ------------------------------------------------------------------------ */
/* Bitmap helpers                                                           */
/* ------------------------------------------------------------------------ */

static inline int bitmap_test(uint64_t *bm, uint64_t page)
{
    return (bm[page / SSD_BITS_PER_U64] >> (page % SSD_BITS_PER_U64)) & 1ULL;
}

static inline void bitmap_set(uint64_t *bm, uint64_t page)
{
    bm[page / SSD_BITS_PER_U64] |= (1ULL << (page % SSD_BITS_PER_U64));
}

static inline void bitmap_clear(uint64_t *bm, uint64_t page)
{
    bm[page / SSD_BITS_PER_U64] &= ~(1ULL << (page % SSD_BITS_PER_U64));
}

/**
 * Find 'npages' consecutive free pages starting from page 'start'.
 * Returns the starting page index, or total_pages (not found).
 */
static uint64_t bitmap_find_free(uint64_t *bm, uint64_t total_pages,
                                  uint64_t start, uint64_t npages)
{
    if (npages == 0 || start >= total_pages)
        return total_pages;

    uint64_t found = start;
    uint64_t consecutive = 0;

    for (uint64_t p = start; p < total_pages; p++) {
        if (!bitmap_test(bm, p)) {
            if (consecutive == 0)
                found = p;
            consecutive++;
            if (consecutive >= npages)
                return found;
        } else {
            consecutive = 0;
        }
    }
    return total_pages; /* not found */
}

/* ------------------------------------------------------------------------ */
/* ssd_backend_create                                                       */
/* ------------------------------------------------------------------------ */

SsdBackend* ssd_backend_create(const char *device_path, uint64_t capacity)
{
    if (!device_path || capacity == 0)
        return NULL;

    /* Align capacity to page boundary */
    capacity = (capacity + SSD_PAGE_SIZE - 1) & ~(uint64_t)(SSD_PAGE_SIZE - 1);

    SsdBackend *sb = calloc(1, sizeof(SsdBackend));
    if (!sb)
        return NULL;

    sb->device_path = strdup(device_path);
    sb->capacity    = capacity;
    sb->total_pages = capacity / SSD_PAGE_SIZE;
    sb->free_pages  = sb->total_pages;
    sb->fd          = -1;
    sb->mmap_base   = MAP_FAILED;
    pthread_mutex_init(&sb->lock, NULL);

    /* Auto-create parent directory if needed */
    char *dir = strdup(device_path);
    char *last_slash = strrchr(dir, '/');
    if (last_slash && last_slash != dir) {
        *last_slash = '\0';
        mkdir(dir, 0755);  /* ignore errors (may already exist) */
    }
    free(dir);

    /* Create or open the device file */
    sb->fd = open(device_path, O_RDWR | O_CREAT, 0644);
    if (sb->fd < 0) {
        umm_log_error(__FILE__, __LINE__,
                      "ssd_backend: open(%s) failed: %s",
                      device_path, strerror(errno));
        goto fail;
    }

    /* Size the file (sparse) */
    if (ftruncate(sb->fd, (off_t)capacity) != 0) {
        umm_log_error(__FILE__, __LINE__,
                      "ssd_backend: ftruncate(%s, %lu) failed: %s",
                      device_path, (unsigned long)capacity, strerror(errno));
        goto fail;
    }

    /* mmap the entire device */
    sb->mmap_base = mmap(NULL, (size_t)capacity, PROT_READ | PROT_WRITE,
                         MAP_SHARED, sb->fd, 0);
    if (sb->mmap_base == MAP_FAILED) {
        umm_log_error(__FILE__, __LINE__,
                      "ssd_backend: mmap(%s, %lu) failed: %s",
                      device_path, (unsigned long)capacity, strerror(errno));
        goto fail;
    }

    /* Allocate bitmap */
    sb->bitmap_words = (sb->total_pages + SSD_BITS_PER_U64 - 1)
                       / SSD_BITS_PER_U64;
    sb->bitmap = calloc(sb->bitmap_words, sizeof(uint64_t));
    if (!sb->bitmap) {
        umm_log_error(__FILE__, __LINE__,
                      "ssd_backend: bitmap alloc failed");
        goto fail;
    }

    umm_log_info(__FILE__, __LINE__,
                 "ssd_backend: created device=%s, capacity=%lu MB, "
                 "pages=%lu, mmap=%p",
                 device_path,
                 (unsigned long)(capacity / (1024 * 1024)),
                 (unsigned long)sb->total_pages,
                 sb->mmap_base);
    return sb;

fail:
    umm_log_error(__FILE__, __LINE__,
                  "ssd_backend_create FAILED: errno=%d (%s), device=%s, "
                  "capacity=%lu",
                  errno, strerror(errno),
                  device_path, (unsigned long)capacity);
    if (sb->mmap_base != MAP_FAILED && sb->mmap_base != NULL)
        munmap(sb->mmap_base, (size_t)sb->capacity);
    if (sb->fd >= 0)
        close(sb->fd);
    free(sb->bitmap);
    free(sb->device_path);
    free(sb);
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* ssd_backend_destroy                                                      */
/* ------------------------------------------------------------------------ */

void ssd_backend_destroy(SsdBackend *sb)
{
    if (!sb)
        return;

    pthread_mutex_lock(&sb->lock);

    if (sb->mmap_base != MAP_FAILED && sb->mmap_base != NULL) {
        msync(sb->mmap_base, (size_t)sb->capacity, MS_SYNC);
        munmap(sb->mmap_base, (size_t)sb->capacity);
    }
    if (sb->fd >= 0)
        close(sb->fd);

    umm_log_info(__FILE__, __LINE__,
                 "ssd_backend: destroyed device=%s, used=%lu/%lu pages",
                 sb->device_path ? sb->device_path : "?",
                 (unsigned long)(sb->total_pages - sb->free_pages),
                 (unsigned long)sb->total_pages);

    pthread_mutex_unlock(&sb->lock);
    pthread_mutex_destroy(&sb->lock);

    free(sb->bitmap);
    free(sb->device_path);
    free(sb);
}

/* ------------------------------------------------------------------------ */
/* ssd_backend_alloc — bitmap allocation                                    */
/* ------------------------------------------------------------------------ */

int ssd_backend_alloc(SsdBackend *sb, uint64_t size, uint64_t *out_offset)
{
    if (!sb || !out_offset || size == 0)
        return UMM_E_INVALID_ARG;

    uint64_t npages = (size + SSD_PAGE_SIZE - 1) / SSD_PAGE_SIZE;

    pthread_mutex_lock(&sb->lock);

    if (sb->free_pages < npages) {
        pthread_mutex_unlock(&sb->lock);
        umm_log_error(__FILE__, __LINE__,
                      "ssd_backend: out of space (need %lu pages, "
                      "free %lu)",
                      (unsigned long)npages,
                      (unsigned long)sb->free_pages);
        return UMM_E_NO_MEMORY;
    }

    uint64_t page = bitmap_find_free(sb->bitmap, sb->total_pages, 0, npages);
    if (page >= sb->total_pages) {
        pthread_mutex_unlock(&sb->lock);
        umm_log_error(__FILE__, __LINE__,
                      "ssd_backend: no contiguous %lu-page region",
                      (unsigned long)npages);
        return UMM_E_NO_MEMORY;
    }

    /* Mark pages as allocated */
    for (uint64_t i = 0; i < npages; i++)
        bitmap_set(sb->bitmap, page + i);

    sb->free_pages -= npages;
    *out_offset = page * SSD_PAGE_SIZE;

    pthread_mutex_unlock(&sb->lock);

    umm_log_debug(__FILE__, __LINE__,
                  "ssd_backend: allocated %lu pages at offset %lu "
                  "(free=%lu/%lu)",
                  (unsigned long)npages, (unsigned long)*out_offset,
                  (unsigned long)sb->free_pages,
                  (unsigned long)sb->total_pages);
    return UMM_OK;
}

/* ------------------------------------------------------------------------ */
/* ssd_backend_free                                                         */
/* ------------------------------------------------------------------------ */

void ssd_backend_free(SsdBackend *sb, uint64_t offset, uint64_t size)
{
    if (!sb || size == 0 || offset >= sb->capacity)
        return;

    uint64_t start_page = offset / SSD_PAGE_SIZE;
    uint64_t npages = (size + SSD_PAGE_SIZE - 1) / SSD_PAGE_SIZE;

    /* Clamp to device bounds */
    if (start_page + npages > sb->total_pages)
        npages = sb->total_pages - start_page;

    pthread_mutex_lock(&sb->lock);

    for (uint64_t i = 0; i < npages; i++) {
        if (bitmap_test(sb->bitmap, start_page + i)) {
            bitmap_clear(sb->bitmap, start_page + i);
            sb->free_pages++;
        }
    }

    pthread_mutex_unlock(&sb->lock);

    umm_log_debug(__FILE__, __LINE__,
                  "ssd_backend: freed %lu pages at offset %lu "
                  "(free=%lu/%lu)",
                  (unsigned long)npages, (unsigned long)offset,
                  (unsigned long)sb->free_pages,
                  (unsigned long)sb->total_pages);
}

/* ------------------------------------------------------------------------ */
/* ssd_backend_get_ptr                                                      */
/* ------------------------------------------------------------------------ */

void* ssd_backend_get_ptr(SsdBackend *sb, uint64_t offset)
{
    if (!sb || offset >= sb->capacity || sb->mmap_base == MAP_FAILED)
        return NULL;

    /* Pure I/O: no bitmap check. Transport layer is a dumb executor.
     * Allocation validation happens at the alloc layer (umms bitmap).
     * After free, the pointer is still valid (mmap is intact) but
     * should not be used — that's a user-level bug. */
    return (uint8_t *)sb->mmap_base + offset;
}

/* ------------------------------------------------------------------------ */
/* ssd_backend_sync                                                         */
/* ------------------------------------------------------------------------ */

int ssd_backend_sync(SsdBackend *sb, uint64_t offset, uint64_t size)
{
    if (!sb || sb->mmap_base == MAP_FAILED)
        return UMM_E_INVALID_ARG;

    if (offset + size > sb->capacity)
        size = sb->capacity - offset;

    int rc = msync((uint8_t *)sb->mmap_base + offset, (size_t)size, MS_SYNC);
    if (rc != 0) {
        umm_log_error(__FILE__, __LINE__,
                      "ssd_backend: msync failed: %s", strerror(errno));
        return UMM_E_TRANSPORT_ERROR;
    }
    return UMM_OK;
}

/* ------------------------------------------------------------------------ */
/* ssd_backend_recover                                                      */
/* ------------------------------------------------------------------------ */

int ssd_backend_recover(SsdBackend *sb)
{
    if (!sb)
        return UMM_E_INVALID_ARG;

    /* For now: clear all allocations (fresh start).
     * In production: scan journal/headers to rebuild bitmap. */
    pthread_mutex_lock(&sb->lock);

    memset(sb->bitmap, 0, sb->bitmap_words * sizeof(uint64_t));
    sb->free_pages = sb->total_pages;

    /* Optional: fsync the underlying file */
    if (sb->fd >= 0)
        fsync(sb->fd);

    pthread_mutex_unlock(&sb->lock);

    umm_log_info(__FILE__, __LINE__,
                 "ssd_backend: recovered device=%s, all %lu pages free",
                 sb->device_path ? sb->device_path : "?",
                 (unsigned long)sb->total_pages);
    return UMM_OK;
}


/* ========================================================================
 * SSD Pool implementation
 * ======================================================================== */

#define SSD_POOL_PAGE_SIZE   4096
#define SSD_POOL_BITS_PER_U64 64
#define SSD_POOL_MAX_DEVICES  16

typedef struct {
    SsdBackend *backend;        /* underlying ssd_backend */
    uint64_t    virtual_base;   /* virtual offset where this device starts */
    uint64_t    capacity;       /* device capacity in bytes */
} SsdDevice;

struct SsdPool {
    SsdDevice   devices[SSD_POOL_MAX_DEVICES];
    uint32_t    num_devices;
    uint64_t    total_capacity; /* sum of all device capacities */
    uint64_t    total_pages;    /* total_capacity / page_size */
    uint64_t    free_pages;
    uint64_t   *bitmap;         /* global virtual allocation bitmap */
    uint64_t    bitmap_words;
    pthread_mutex_t lock;
};

/* ------------------------------------------------------------------------ */
/* Bitmap helpers (same logic as ssd_backend)                               */
/* ------------------------------------------------------------------------ */

static inline int bm_test(uint64_t *bm, uint64_t page)
{
    return (bm[page / SSD_POOL_BITS_PER_U64] >> (page % SSD_POOL_BITS_PER_U64)) & 1ULL;
}

static inline void bm_set(uint64_t *bm, uint64_t page)
{
    bm[page / SSD_POOL_BITS_PER_U64] |= (1ULL << (page % SSD_POOL_BITS_PER_U64));
}

static inline void bm_clear(uint64_t *bm, uint64_t page)
{
    bm[page / SSD_POOL_BITS_PER_U64] &= ~(1ULL << (page % SSD_POOL_BITS_PER_U64));
}

static uint64_t bm_find_free(uint64_t *bm, uint64_t total_pages,
                              uint64_t start, uint64_t npages)
{
    if (npages == 0 || start >= total_pages)
        return total_pages;

    uint64_t found = start;
    uint64_t consecutive = 0;

    for (uint64_t p = start; p < total_pages; p++) {
        if (!bm_test(bm, p)) {
            if (consecutive == 0)
                found = p;
            consecutive++;
            if (consecutive >= npages)
                return found;
        } else {
            consecutive = 0;
        }
    }
    return total_pages;
}

/* ------------------------------------------------------------------------ */
/* ssd_pool_create / destroy                                                */
/* ------------------------------------------------------------------------ */

SsdPool* ssd_pool_create(void)
{
    SsdPool *pool = calloc(1, sizeof(SsdPool));
    if (!pool)
        return NULL;
    pthread_mutex_init(&pool->lock, NULL);
    return pool;
}

void ssd_pool_destroy(SsdPool *pool)
{
    if (!pool)
        return;

    pthread_mutex_lock(&pool->lock);

    for (uint32_t i = 0; i < pool->num_devices; i++) {
        if (pool->devices[i].backend)
            ssd_backend_destroy(pool->devices[i].backend);
    }
    free(pool->bitmap);

    umm_log_info(__FILE__, __LINE__,
                 "ssd_pool: destroyed %u devices, total=%lu MB",
                 pool->num_devices,
                 (unsigned long)(pool->total_capacity / (1024 * 1024)));

    pthread_mutex_unlock(&pool->lock);
    pthread_mutex_destroy(&pool->lock);
    free(pool);
}

/* ------------------------------------------------------------------------ */
/* ssd_pool_add_device                                                      */
/* ------------------------------------------------------------------------ */

int ssd_pool_add_device(SsdPool *pool, const char *device_path, uint64_t capacity)
{
    if (!pool || !device_path || capacity == 0)
        return UMM_E_INVALID_ARG;
    if (pool->num_devices >= SSD_POOL_MAX_DEVICES)
        return UMM_E_NO_MEMORY;

    /* Align capacity */
    capacity = (capacity + SSD_POOL_PAGE_SIZE - 1)
               & ~(uint64_t)(SSD_POOL_PAGE_SIZE - 1);

    pthread_mutex_lock(&pool->lock);

    uint32_t idx = pool->num_devices;
    uint64_t virtual_base = pool->total_capacity;

    /* Create underlying ssd_backend */
    SsdBackend *sb = ssd_backend_create(device_path, capacity);
    if (!sb) {
        pthread_mutex_unlock(&pool->lock);
        umm_log_error(__FILE__, __LINE__,
                      "ssd_pool: failed to create backend for %s", device_path);
        return UMM_E_TRANSPORT_ERROR;
    }

    pool->devices[idx].backend      = sb;
    pool->devices[idx].virtual_base = virtual_base;
    pool->devices[idx].capacity     = capacity;
    pool->num_devices++;
    pool->total_capacity += capacity;

    /* Resize global virtual bitmap */
    uint64_t new_total_pages = pool->total_capacity / SSD_POOL_PAGE_SIZE;
    uint64_t new_words = (new_total_pages + SSD_POOL_BITS_PER_U64 - 1)
                         / SSD_POOL_BITS_PER_U64;

    uint64_t *new_bm = calloc(new_words, sizeof(uint64_t));
    if (!new_bm) {
        /* rollback */
        pool->total_capacity -= capacity;
        pool->num_devices--;
        ssd_backend_destroy(sb);
        pthread_mutex_unlock(&pool->lock);
        return UMM_E_NO_MEMORY;
    }

    if (pool->bitmap) {
        /* Copy old bitmap (old pages remain at the same positions) */
        memcpy(new_bm, pool->bitmap, pool->bitmap_words * sizeof(uint64_t));
        free(pool->bitmap);
    }

    pool->bitmap = new_bm;
    pool->bitmap_words = new_words;
    pool->total_pages = new_total_pages;
    pool->free_pages += (capacity / SSD_POOL_PAGE_SIZE);

    pthread_mutex_unlock(&pool->lock);

    umm_log_info(__FILE__, __LINE__,
                 "ssd_pool: added device[%u] %s, capacity=%lu MB, "
                 "virtual_base=0x%lx, total_pool=%lu MB",
                 idx, device_path,
                 (unsigned long)(capacity / (1024 * 1024)),
                 (unsigned long)virtual_base,
                 (unsigned long)(pool->total_capacity / (1024 * 1024)));
    return UMM_OK;
}

/* ------------------------------------------------------------------------ */
/* Query helpers                                                            */
/* ------------------------------------------------------------------------ */

uint64_t ssd_pool_total_capacity(const SsdPool *pool)
{
    return pool ? pool->total_capacity : 0;
}

uint32_t ssd_pool_num_devices(const SsdPool *pool)
{
    return pool ? pool->num_devices : 0;
}

/* ------------------------------------------------------------------------ */
/* ssd_pool_alloc — allocate from global virtual address space              */
/* ------------------------------------------------------------------------ */

int ssd_pool_alloc(SsdPool *pool, uint64_t size, uint64_t *out_voffset)
{
    if (!pool || !out_voffset || size == 0)
        return UMM_E_INVALID_ARG;

    uint64_t npages = (size + SSD_POOL_PAGE_SIZE - 1) / SSD_POOL_PAGE_SIZE;

    pthread_mutex_lock(&pool->lock);

    if (pool->free_pages < npages) {
        pthread_mutex_unlock(&pool->lock);
        umm_log_error(__FILE__, __LINE__,
                      "ssd_pool: out of space (need %lu pages, free %lu)",
                      (unsigned long)npages, (unsigned long)pool->free_pages);
        return UMM_E_NO_MEMORY;
    }

    uint64_t page = bm_find_free(pool->bitmap, pool->total_pages, 0, npages);
    if (page >= pool->total_pages) {
        pthread_mutex_unlock(&pool->lock);
        umm_log_error(__FILE__, __LINE__,
                      "ssd_pool: no contiguous %lu-page region",
                      (unsigned long)npages);
        return UMM_E_NO_MEMORY;
    }

    for (uint64_t i = 0; i < npages; i++)
        bm_set(pool->bitmap, page + i);

    pool->free_pages -= npages;
    *out_voffset = page * SSD_POOL_PAGE_SIZE;

    pthread_mutex_unlock(&pool->lock);

    umm_log_debug(__FILE__, __LINE__,
                  "ssd_pool: allocated %lu pages at voffset=0x%lx "
                  "(free=%lu/%lu)",
                  (unsigned long)npages, (unsigned long)*out_voffset,
                  (unsigned long)pool->free_pages,
                  (unsigned long)pool->total_pages);
    return UMM_OK;
}

/* ------------------------------------------------------------------------ */
/* ssd_pool_free                                                            */
/* ------------------------------------------------------------------------ */

void ssd_pool_free(SsdPool *pool, uint64_t voffset, uint64_t size)
{
    if (!pool || size == 0 || voffset >= pool->total_capacity)
        return;

    uint64_t start_page = voffset / SSD_POOL_PAGE_SIZE;
    uint64_t npages = (size + SSD_POOL_PAGE_SIZE - 1) / SSD_POOL_PAGE_SIZE;

    if (start_page + npages > pool->total_pages)
        npages = pool->total_pages - start_page;

    pthread_mutex_lock(&pool->lock);

    for (uint64_t i = 0; i < npages; i++) {
        if (bm_test(pool->bitmap, start_page + i)) {
            bm_clear(pool->bitmap, start_page + i);
            pool->free_pages++;
        }
    }

    pthread_mutex_unlock(&pool->lock);

    umm_log_debug(__FILE__, __LINE__,
                  "ssd_pool: freed %lu pages at voffset=0x%lx "
                  "(free=%lu/%lu)",
                  (unsigned long)npages, (unsigned long)voffset,
                  (unsigned long)pool->free_pages,
                  (unsigned long)pool->total_pages);
}

/* ------------------------------------------------------------------------ */
/* ssd_pool_translate — virtual offset → (device, physical_offset)          */
/* ------------------------------------------------------------------------ */

int ssd_pool_translate(SsdPool *pool, uint64_t voffset,
                        uint32_t *out_dev, uint64_t *out_poff)
{
    if (!pool || voffset >= pool->total_capacity)
        return UMM_E_INVALID_ARG;

    for (uint32_t i = 0; i < pool->num_devices; i++) {
        SsdDevice *dev = &pool->devices[i];
        if (voffset >= dev->virtual_base &&
            voffset < dev->virtual_base + dev->capacity) {
            if (out_dev)  *out_dev = i;
            if (out_poff) *out_poff = voffset - dev->virtual_base;
            return UMM_OK;
        }
    }
    return UMM_E_INVALID_ARG;  /* should not reach here */
}

/* ------------------------------------------------------------------------ */
/* ssd_pool_get_ptr                                                         */
/* ------------------------------------------------------------------------ */

void* ssd_pool_get_ptr(SsdPool *pool, uint64_t voffset)
{
    if (!pool)
        return NULL;

    uint32_t dev_idx;
    uint64_t poff;
    if (ssd_pool_translate(pool, voffset, &dev_idx, &poff) != UMM_OK)
        return NULL;

    return ssd_backend_get_ptr(pool->devices[dev_idx].backend, poff);
}

/* ------------------------------------------------------------------------ */
/* ssd_pool_sync                                                            */
/* ------------------------------------------------------------------------ */

int ssd_pool_sync(SsdPool *pool, uint64_t voffset, uint64_t size)
{
    if (!pool || size == 0)
        return UMM_E_INVALID_ARG;

    /* Handle cross-device sync */
    uint64_t end = voffset + size;
    if (end > pool->total_capacity)
        end = pool->total_capacity;

    while (voffset < end) {
        uint32_t dev_idx;
        uint64_t poff;
        int rc = ssd_pool_translate(pool, voffset, &dev_idx, &poff);
        if (rc != UMM_OK)
            return rc;

        /* How much remains in this device? */
        uint64_t dev_end = pool->devices[dev_idx].virtual_base
                         + pool->devices[dev_idx].capacity;
        uint64_t chunk = end - voffset;
        if (voffset + chunk > dev_end)
            chunk = dev_end - voffset;

        rc = ssd_backend_sync(pool->devices[dev_idx].backend, poff, chunk);
        if (rc != UMM_OK)
            return rc;

        voffset += chunk;
    }
    return UMM_OK;
}
