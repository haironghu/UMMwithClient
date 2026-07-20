#ifndef UMM_BITMAP_ALLOCATOR_H
#define UMM_BITMAP_ALLOCATOR_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle */
typedef struct BitmapAllocator BitmapAllocator;

/**
 * Create a new bitmap allocator.
 *
 * @param total_bytes  Total memory arena size in bytes.
 * @param page_size    Allocation granularity (must be >0, default 4096 if 0).
 * @return             New allocator handle, or NULL on error.
 */
BitmapAllocator* ba_create(uint64_t total_bytes, uint64_t page_size);

/**
 * Destroy a bitmap allocator and free all associated memory.
 */
void ba_destroy(BitmapAllocator *ba);

/**
 * Allocate contiguous pages covering at least num_bytes.
 *
 * @param ba         Allocator handle.
 * @param num_bytes  Number of bytes requested.
 * @param out_offset Byte offset of the allocation (output).
 * @return           UMM_OK on success, negative error code on failure.
 */
int ba_alloc(BitmapAllocator *ba, uint64_t num_bytes, uint64_t *out_offset);

/**
 * Free a previously allocated range.
 *
 * @param ba         Allocator handle.
 * @param offset     Byte offset previously returned by ba_alloc().
 * @param num_bytes  Number of bytes originally requested (used to compute page count).
 * @return           UMM_OK on success, negative error code on failure.
 */
int ba_free(BitmapAllocator *ba, uint64_t offset, uint64_t num_bytes);

/**
 * Check whether every page in a range is currently allocated.
 *
 * @return  1 if all pages in the range are allocated, 0 otherwise.
 */
int ba_is_allocated(BitmapAllocator *ba, uint64_t offset, uint64_t num_bytes);

/**
 * Return the number of free pages remaining.
 */
uint64_t ba_get_free_pages(BitmapAllocator *ba);

/**
 * Print allocator state to stderr (for debugging).
 */
void ba_dump(BitmapAllocator *ba);

#ifdef __cplusplus
}
#endif

#endif /* UMM_BITMAP_ALLOCATOR_H */
