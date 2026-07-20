#include "bitmap_allocator.h"
#include "error_codes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* Default page size */
#define BA_DEFAULT_PAGE_SIZE 4096

/* ========================================================================
 * BitmapAllocator internals
 * ======================================================================== */

struct BitmapAllocator {
    uint64_t      total_bytes;
    uint64_t      page_size;
    uint64_t      num_pages;
    uint64_t      free_pages;
    uint8_t      *bitmap;       /* One bit per page: 1=free, 0=allocated */
    pthread_mutex_t lock;
};

/* ========================================================================
 * Bitmap primitives (single-bit ops)
 * ======================================================================== */

static inline int bitmap_get(uint8_t *bitmap, uint64_t idx)
{
    return (bitmap[idx >> 3] >> (idx & 7)) & 1;
}

static inline void bitmap_set(uint8_t *bitmap, uint64_t idx)
{
    bitmap[idx >> 3] |= (uint8_t)(1u << (idx & 7));
}

static inline void bitmap_clear(uint8_t *bitmap, uint64_t idx)
{
    bitmap[idx >> 3] &= (uint8_t) ~(1u << (idx & 7));
}

/* ========================================================================
 * Helper: byte index / bit position within a byte
 * ======================================================================== */

static inline uint64_t byte_idx(uint64_t page)
{
    return page >> 3;
}

/*
 * Fast word-level scan:
 * Search forward from @page looking for a contiguous run of @npages
 * free pages. Returns the page index of the start of the run, or
 * num_pages on failure.
 */
static uint64_t find_free_run(uint8_t *bitmap, uint64_t num_pages,
                              uint64_t start_page, uint64_t npages)
{
    uint64_t page = start_page;
    uint64_t end  = num_pages;

    while (page + npages <= end) {
        /* Quick word-level check: can we skip a whole byte? */
        if ((page & 7) == 0 && npages <= 8) {
            /* Byte-aligned; check if entire byte is free */
            uint64_t bi = byte_idx(page);
            if (bitmap[bi] == 0xFF) {
                /* All 8 pages free */
                return page;
            }
        }

        /* Check run starting at 'page' */
        int found = 1;
        for (uint64_t i = 0; i < npages; i++) {
            if (!bitmap_get(bitmap, page + i)) {
                found = 0;
                page += i + 1; /* skip past the allocated page */
                break;
            }
        }
        if (found)
            return page;
    }

    return num_pages; /* not found */
}

/* ========================================================================
 * Public API
 * ======================================================================== */

BitmapAllocator* ba_create(uint64_t total_bytes, uint64_t page_size)
{
    if (total_bytes == 0)
        return NULL;

    if (page_size == 0)
        page_size = BA_DEFAULT_PAGE_SIZE;

    if (page_size == 0 || total_bytes < page_size)
        return NULL;

    BitmapAllocator *ba = calloc(1, sizeof(BitmapAllocator));
    if (!ba)
        return NULL;

    ba->total_bytes = total_bytes;
    ba->page_size   = page_size;
    ba->num_pages   = (total_bytes + page_size - 1) / page_size;
    ba->free_pages  = ba->num_pages;

    uint64_t bitmap_bytes = (ba->num_pages + 7) / 8;
    ba->bitmap = calloc(1, bitmap_bytes);
    if (!ba->bitmap) {
        free(ba);
        return NULL;
    }

    /* Mark all pages as free */
    memset(ba->bitmap, 0xFF, bitmap_bytes);

    /* Clear any bits past num_pages (they don't exist) */
    uint64_t spare = bitmap_bytes * 8 - ba->num_pages;
    for (uint64_t i = 0; i < spare; i++)
        bitmap_clear(ba->bitmap, ba->num_pages + i);

    pthread_mutex_init(&ba->lock, NULL);

    return ba;
}

void ba_destroy(BitmapAllocator *ba)
{
    if (!ba)
        return;

    pthread_mutex_destroy(&ba->lock);
    free(ba->bitmap);
    free(ba);
}

int ba_alloc(BitmapAllocator *ba, uint64_t num_bytes, uint64_t *out_offset)
{
    if (!ba || !out_offset || num_bytes == 0)
        return UMM_E_INVALID_ARG;

    uint64_t npages = (num_bytes + ba->page_size - 1) / ba->page_size;
    if (npages > ba->num_pages)
        return UMM_E_NO_MEMORY;

    pthread_mutex_lock(&ba->lock);

    if (npages > ba->free_pages) {
        pthread_mutex_unlock(&ba->lock);
        return UMM_E_NO_MEMORY;
    }

    uint64_t page = find_free_run(ba->bitmap, ba->num_pages, 0, npages);
    if (page >= ba->num_pages) {
        pthread_mutex_unlock(&ba->lock);
        return UMM_E_NO_MEMORY;
    }

    /* Mark pages as allocated */
    for (uint64_t i = 0; i < npages; i++)
        bitmap_clear(ba->bitmap, page + i);

    ba->free_pages -= npages;
    *out_offset = page * ba->page_size;

    pthread_mutex_unlock(&ba->lock);
    return UMM_OK;
}

int ba_free(BitmapAllocator *ba, uint64_t offset, uint64_t num_bytes)
{
    if (!ba || num_bytes == 0)
        return UMM_E_INVALID_ARG;

    if (offset >= ba->total_bytes)
        return UMM_E_INVALID_ARG;

    uint64_t page  = offset / ba->page_size;
    uint64_t npages = (num_bytes + ba->page_size - 1) / ba->page_size;

    if (page + npages > ba->num_pages)
        return UMM_E_INVALID_ARG;

    pthread_mutex_lock(&ba->lock);

    /* Validate: all pages must be currently allocated (not free) */
    for (uint64_t i = 0; i < npages; i++) {
        if (bitmap_get(ba->bitmap, page + i)) {
            /* Page is free – double-free or bad offset */
            pthread_mutex_unlock(&ba->lock);
            return UMM_E_INVALID_ARG;
        }
    }

    /* Mark pages as free */
    for (uint64_t i = 0; i < npages; i++)
        bitmap_set(ba->bitmap, page + i);

    ba->free_pages += npages;

    pthread_mutex_unlock(&ba->lock);
    return UMM_OK;
}

int ba_is_allocated(BitmapAllocator *ba, uint64_t offset, uint64_t num_bytes)
{
    if (!ba || num_bytes == 0)
        return 0;

    if (offset >= ba->total_bytes)
        return 0;

    uint64_t page   = offset / ba->page_size;
    uint64_t npages = (num_bytes + ba->page_size - 1) / ba->page_size;

    if (page + npages > ba->num_pages)
        return 0;

    pthread_mutex_lock(&ba->lock);

    int allocated = 1;
    for (uint64_t i = 0; i < npages; i++) {
        if (bitmap_get(ba->bitmap, page + i)) {
            /* Page is free (bit=1 means free) */
            allocated = 0;
            break;
        }
    }

    pthread_mutex_unlock(&ba->lock);
    return allocated;
}

uint64_t ba_get_free_pages(BitmapAllocator *ba)
{
    if (!ba)
        return 0;

    pthread_mutex_lock(&ba->lock);
    uint64_t freep = ba->free_pages;
    pthread_mutex_unlock(&ba->lock);
    return freep;
}

void ba_dump(BitmapAllocator *ba)
{
    if (!ba)
        return;

    pthread_mutex_lock(&ba->lock);

    fprintf(stderr,
        "[BitmapAllocator] total_bytes=%lu page_size=%lu num_pages=%lu free_pages=%lu\n",
        (unsigned long)ba->total_bytes,
        (unsigned long)ba->page_size,
        (unsigned long)ba->num_pages,
        (unsigned long)ba->free_pages);

    /* Print bitmap in compact form: 32 bytes per line */
    uint64_t bitmap_bytes = (ba->num_pages + 7) / 8;
    for (uint64_t i = 0; i < bitmap_bytes; i++) {
        if (i % 16 == 0)
            fprintf(stderr, "  [%04lu] ", (unsigned long)i);
        fprintf(stderr, "%02X ", ba->bitmap[i]);
        if (i % 16 == 15)
            fprintf(stderr, "\n");
    }
    if (bitmap_bytes % 16 != 0)
        fprintf(stderr, "\n");

    pthread_mutex_unlock(&ba->lock);
}
