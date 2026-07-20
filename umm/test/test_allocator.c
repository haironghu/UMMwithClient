/* ========================================================================
 * test_allocator.c -- Unit tests for the bitmap allocator
 * ======================================================================== */

#include "test_framework.h"
#include "common/bitmap_allocator.h"
#include "common/error_codes.h"

#include <stdint.h>
#include <stdio.h>

/* ------------------------------------------------------------------------ */
/* Test 1: basic_alloc_free -- single allocation and free                   */
/* ------------------------------------------------------------------------ */
TEST(basic_alloc_free)
{
    BitmapAllocator *ba = ba_create(1024ULL * 1024, 4096); /* 1 MB, 4 KB pages */
    ASSERT_NOT_NULL(ba);

    uint64_t offset = 0xDEADBEEF;
    int rc = ba_alloc(ba, 4096, &offset);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(offset, 0); /* first allocation starts at offset 0 */

    rc = ba_free(ba, offset, 4096);
    ASSERT_EQ(rc, UMM_OK);

    ba_destroy(ba);
}

/* ------------------------------------------------------------------------ */
/* Test 2: multiple_allocs -- several allocations, verify offsets differ    */
/* ------------------------------------------------------------------------ */
TEST(multiple_allocs)
{
    BitmapAllocator *ba = ba_create(1024ULL * 1024, 4096); /* 1 MB */
    ASSERT_NOT_NULL(ba);

    uint64_t off1 = 0, off2 = 0, off3 = 0;
    int rc;

    rc = ba_alloc(ba, 4096, &off1);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(off1, 0);

    rc = ba_alloc(ba, 8192, &off2); /* 2 pages */
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(off2, 4096); /* starts right after first alloc */

    rc = ba_alloc(ba, 16384, &off3); /* 4 pages */
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(off3, 4096 + 8192); /* starts after first two */

    /* Verify all are allocated */
    ASSERT_TRUE(ba_is_allocated(ba, off1, 4096));
    ASSERT_TRUE(ba_is_allocated(ba, off2, 8192));
    ASSERT_TRUE(ba_is_allocated(ba, off3, 16384));

    /* Free all */
    rc = ba_free(ba, off1, 4096);
    ASSERT_EQ(rc, UMM_OK);
    rc = ba_free(ba, off2, 8192);
    ASSERT_EQ(rc, UMM_OK);
    rc = ba_free(ba, off3, 16384);
    ASSERT_EQ(rc, UMM_OK);

    ba_destroy(ba);
}

/* ------------------------------------------------------------------------ */
/* Test 3: alloc_until_full -- exhaust a small allocator, verify ENOMEM     */
/* ------------------------------------------------------------------------ */
TEST(alloc_until_full)
{
    /* 64 KB total, 4 KB pages => 16 pages */
    BitmapAllocator *ba = ba_create(64ULL * 1024, 4096);
    ASSERT_NOT_NULL(ba);

    uint64_t offsets[16];
    int rc;
    int i;

    /* Allocate all 16 pages one at a time */
    for (i = 0; i < 16; i++) {
        rc = ba_alloc(ba, 4096, &offsets[i]);
        ASSERT_EQ(rc, UMM_OK);
    }

    /* Next allocation should fail with out-of-memory */
    uint64_t extra = 0;
    rc = ba_alloc(ba, 4096, &extra);
    ASSERT_EQ(rc, UMM_E_NO_MEMORY);

    /* Free all */
    for (i = 0; i < 16; i++) {
        rc = ba_free(ba, offsets[i], 4096);
        ASSERT_EQ(rc, UMM_OK);
    }

    ba_destroy(ba);
}

/* ------------------------------------------------------------------------ */
/* Test 4: free_and_realloc -- free, then alloc again, verify same offset   */
/* ------------------------------------------------------------------------ */
TEST(free_and_realloc)
{
    BitmapAllocator *ba = ba_create(1024ULL * 1024, 4096);
    ASSERT_NOT_NULL(ba);

    uint64_t offset1 = 0;
    int rc = ba_alloc(ba, 4096, &offset1);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(offset1, 0);

    rc = ba_free(ba, offset1, 4096);
    ASSERT_EQ(rc, UMM_OK);

    uint64_t offset2 = 0;
    rc = ba_alloc(ba, 4096, &offset2);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(offset2, offset1); /* should reuse the freed page */

    ba_free(ba, offset2, 4096);
    ba_destroy(ba);
}

/* ------------------------------------------------------------------------ */
/* Test 5: invalid_free -- freeing an unallocated offset returns error      */
/* ------------------------------------------------------------------------ */
TEST(invalid_free)
{
    BitmapAllocator *ba = ba_create(1024ULL * 1024, 4096);
    ASSERT_NOT_NULL(ba);

    /* Try to free offset that was never allocated */
    int rc = ba_free(ba, 0, 4096);
    ASSERT_EQ(rc, UMM_E_INVALID_ARG);

    /* Allocate, then free, then try double-free */
    uint64_t offset = 0;
    rc = ba_alloc(ba, 4096, &offset);
    ASSERT_EQ(rc, UMM_OK);

    rc = ba_free(ba, offset, 4096);
    ASSERT_EQ(rc, UMM_OK);

    rc = ba_free(ba, offset, 4096);
    ASSERT_EQ(rc, UMM_E_INVALID_ARG);

    ba_destroy(ba);
}

/* ------------------------------------------------------------------------ */
/* Test 6: cross_boundary -- size not aligned to page rounds up             */
/* ------------------------------------------------------------------------ */
TEST(cross_boundary)
{
    BitmapAllocator *ba = ba_create(1024ULL * 1024, 4096);
    ASSERT_NOT_NULL(ba);

    /* Request 1 byte -- should allocate 1 full page (4096 bytes) */
    uint64_t offset = 0;
    int rc = ba_alloc(ba, 1, &offset);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(offset, 0);

    /* The full page should be marked allocated */
    ASSERT_TRUE(ba_is_allocated(ba, 0, 4096));

    /* Next allocation should start at page boundary */
    uint64_t offset2 = 0;
    rc = ba_alloc(ba, 1, &offset2);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(offset2, 4096);

    ba_free(ba, offset, 1);
    ba_free(ba, offset2, 1);
    ba_destroy(ba);
}

/* ------------------------------------------------------------------------ */
/* Main                                                                     */
/* ------------------------------------------------------------------------ */
TEST_SUITE("Bitmap Allocator")
{
    RUN_TEST(basic_alloc_free);
    RUN_TEST(multiple_allocs);
    RUN_TEST(alloc_until_full);
    RUN_TEST(free_and_realloc);
    RUN_TEST(invalid_free);
    RUN_TEST(cross_boundary);
}
END_TEST_SUITE()
