/* ========================================================================
 * test_integration.c -- Full integration tests using the public API (umm.h)
 *
 * v2.0: No Region concept. Only Chunk. All memory is CXL shared.
 * Unified allocation path: umm_alloc(size) -> umms alloc -> make_gpa
 *                         -> ummd register -> return ChunkDescriptor
 *
 * Uses direct (embedded) mode: empty addresses => local services.
 * ======================================================================== */

#include "../include/umm.h"
#include "test_framework.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------------
 * Direct mode configuration
 * ------------------------------------------------------------------------ */
static UMMConfig direct_cfg = {
    .transport           = "mock",
    .consistency_model   = "hardware",
    .memory_size         = 64ULL * 1024 * 1024,  /* 64 MB */
    .meta_server_addr    = "",                   /* empty = direct mode */
    .mem_server_addr     = "",                   /* empty = direct mode */
    .my_node_id          = 0,
};

/* ------------------------------------------------------------------------
 * Helper: ensure clean state (defensive against prior test failure)
 * ------------------------------------------------------------------------ */
static void ensure_clean_state(void)
{
    umm_deinit(); /* safe to call even if not initialized */
}

/* ------------------------------------------------------------------------
 * Test 1: init_deinit
 * Initialize and shut down the library.
 * ------------------------------------------------------------------------ */
TEST(init_deinit)
{
    ensure_clean_state();

    int rc = umm_init(&direct_cfg);
    ASSERT_EQ(rc, UMM_OK);

    umm_deinit();
}

/* ------------------------------------------------------------------------
 * Test 2: alloc_free
 * Allocate a 4KB chunk, verify descriptor fields, then free.
 * ------------------------------------------------------------------------ */
TEST(alloc_free)
{
    ensure_clean_state();

    int rc = umm_init(&direct_cfg);
    ASSERT_EQ(rc, UMM_OK);

    ChunkDescriptor desc;
    memset(&desc, 0, sizeof(desc));

    rc = umm_alloc(4096, &desc);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_NE(desc.chunk_id, 0);
    /* Note: base_gpa may be 0 (node 0, offset 0) which is valid */
    ASSERT_EQ(desc.user_size, 4096);

    /* Verify GPA encodes our node */
    ASSERT_EQ(gpa_to_node(desc.base_gpa), direct_cfg.my_node_id);

    rc = umm_free(&desc);
    ASSERT_EQ(rc, UMM_OK);

    umm_deinit();
}

/* ------------------------------------------------------------------------
 * Test 3: alloc_sizes
 * Allocate various sizes: 4KB, 64KB, 1MB. Verify sizes are page-aligned.
 * ------------------------------------------------------------------------ */
TEST(alloc_sizes)
{
    ensure_clean_state();

    int rc = umm_init(&direct_cfg);
    ASSERT_EQ(rc, UMM_OK);

    ChunkDescriptor desc4k, desc64k, desc1m;

    /* 4KB */
    rc = umm_alloc(4096, &desc4k);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_NE(desc4k.chunk_id, 0);
    ASSERT_EQ(desc4k.user_size, 4096);

    /* 64KB */
    rc = umm_alloc(64ULL * 1024, &desc64k);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_NE(desc64k.chunk_id, 0);
    ASSERT_EQ(desc64k.user_size, 64ULL * 1024);

    /* 1MB */
    rc = umm_alloc(1024ULL * 1024, &desc1m);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_NE(desc1m.chunk_id, 0);
    ASSERT_EQ(desc1m.user_size, 1024ULL * 1024);

    /* All chunk IDs should be different */
    ASSERT_NE(desc4k.chunk_id, desc64k.chunk_id);
    ASSERT_NE(desc64k.chunk_id, desc1m.chunk_id);

    /* Clean up */
    umm_free(&desc4k);
    umm_free(&desc64k);
    umm_free(&desc1m);

    umm_deinit();
}

/* ------------------------------------------------------------------------
 * Test 4: read_write
 * Allocate 4KB, write a pattern, read back, verify data.
 * ------------------------------------------------------------------------ */
TEST(read_write)
{
    ensure_clean_state();

    int rc = umm_init(&direct_cfg);
    ASSERT_EQ(rc, UMM_OK);

    ChunkDescriptor desc;
    rc = umm_alloc(4096, &desc);
    ASSERT_EQ(rc, UMM_OK);

    /* Write a test pattern */
    uint8_t write_buf[4096];
    for (int i = 0; i < 4096; i++)
        write_buf[i] = (uint8_t)(i & 0xFF);

    rc = umm_write(&desc, 0, 4096, write_buf);
    ASSERT_EQ(rc, UMM_OK);

    /* Read it back */
    uint8_t read_buf[4096];
    memset(read_buf, 0, sizeof(read_buf));
    rc = umm_read(&desc, 0, 4096, read_buf);
    ASSERT_EQ(rc, UMM_OK);

    /* Verify */
    ASSERT_EQ(memcmp(read_buf, write_buf, 4096), 0);

    umm_free(&desc);
    umm_deinit();
}

/* ------------------------------------------------------------------------
 * Test 5: atomic_cas
 * Atomic compare-and-swap success and failure cases.
 * ------------------------------------------------------------------------ */
TEST(atomic_cas)
{
    ensure_clean_state();

    int rc = umm_init(&direct_cfg);
    ASSERT_EQ(rc, UMM_OK);

    /* Need at least 8 bytes for atomic ops */
    ChunkDescriptor desc;
    rc = umm_alloc(4096, &desc);
    ASSERT_EQ(rc, UMM_OK);

    /* Initialize to 0 */
    rc = umm_atomic_set(&desc, 0, 0);
    ASSERT_EQ(rc, UMM_OK);

    /* CAS: 0 -> 42 (should succeed) */
    uint64_t old = 0;
    rc = umm_atomic_cas(&desc, 0, 0, 42, &old);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(old, 0);

    /* CAS: 0 -> 99 (should fail, current value is 42) */
    old = 0;
    rc = umm_atomic_cas(&desc, 0, 0, 99, &old);
    ASSERT_EQ(rc, UMM_OK); /* CAS returns OK even on mismatch */
    ASSERT_EQ(old, 42);    /* old receives the actual current value */

    /* Verify final value is still 42 */
    uint64_t final_val = 0;
    rc = umm_read(&desc, 0, sizeof(uint64_t), &final_val);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(final_val, 42);

    umm_free(&desc);
    umm_deinit();
}

/* ------------------------------------------------------------------------
 * Test 6: fence
 * Fence doesn't crash.
 * ------------------------------------------------------------------------ */
TEST(fence)
{
    ensure_clean_state();

    int rc = umm_init(&direct_cfg);
    ASSERT_EQ(rc, UMM_OK);

    umm_fence();
    umm_barrier_all();
    umm_quiet();

    umm_deinit();
}

/* ------------------------------------------------------------------------
 * Test 7: multiple_chunks
 * Allocate 5 chunks, verify different chunk_ids and GPAs.
 * ------------------------------------------------------------------------ */
TEST(multiple_chunks)
{
    ensure_clean_state();

    int rc = umm_init(&direct_cfg);
    ASSERT_EQ(rc, UMM_OK);

    ChunkDescriptor chunks[5];

    for (int i = 0; i < 5; i++) {
        rc = umm_alloc(4096, &chunks[i]);
        ASSERT_EQ(rc, UMM_OK);
        ASSERT_NE(chunks[i].chunk_id, 0);
        ASSERT_EQ(chunks[i].user_size, 4096);
    }

    /* Verify all chunk IDs are unique */
    for (int i = 0; i < 5; i++) {
        for (int j = i + 1; j < 5; j++) {
            ASSERT_NE(chunks[i].chunk_id, chunks[j].chunk_id);
            ASSERT_NE(chunks[i].base_gpa, chunks[j].base_gpa);
        }
    }

    /* Write unique data to each chunk */
    for (int i = 0; i < 5; i++) {
        uint64_t val = (uint64_t)(0x100 + i);
        rc = umm_write(&chunks[i], 0, sizeof(uint64_t), &val);
        ASSERT_EQ(rc, UMM_OK);
    }

    /* Read back and verify */
    for (int i = 0; i < 5; i++) {
        uint64_t val = 0;
        rc = umm_read(&chunks[i], 0, sizeof(uint64_t), &val);
        ASSERT_EQ(rc, UMM_OK);
        ASSERT_EQ(val, (uint64_t)(0x100 + i));
    }

    /* Free all */
    for (int i = 0; i < 5; i++) {
        rc = umm_free(&chunks[i]);
        ASSERT_EQ(rc, UMM_OK);
    }

    umm_deinit();
}

/* ------------------------------------------------------------------------
 * Test 8: lookup_chunk
 * Allocate (auto-named), then lookup by name and verify metadata.
 * ------------------------------------------------------------------------ */
TEST(lookup_chunk)
{
    ensure_clean_state();

    int rc = umm_init(&direct_cfg);
    ASSERT_EQ(rc, UMM_OK);

    ChunkDescriptor desc;
    rc = umm_alloc(4096, &desc);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_NE(desc.chunk_id, 0);

    /* Build the auto-generated name */
    uint64_t offset = gpa_to_offset(desc.base_gpa);
    char expected_name[64];
    snprintf(expected_name, sizeof(expected_name), "chunk_%u_%lu",
             (unsigned)direct_cfg.my_node_id, (unsigned long)offset);

    /* Look up by name */
    ChunkMetadata meta;
    memset(&meta, 0, sizeof(meta));
    rc = umm_lookup_chunk(expected_name, &meta);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(meta.gpa, desc.base_gpa);
    ASSERT_EQ(meta.size, desc.user_size);
    ASSERT_EQ(strcmp(meta.name, expected_name), 0);

    umm_free(&desc);
    umm_deinit();
}

/* ------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------ */
TEST_SUITE("Integration v2.0 (Direct Mode)")
{
    RUN_TEST(init_deinit);
    RUN_TEST(alloc_free);
    RUN_TEST(alloc_sizes);
    RUN_TEST(read_write);
    RUN_TEST(atomic_cas);
    RUN_TEST(fence);
    RUN_TEST(multiple_chunks);
    RUN_TEST(lookup_chunk);
}
END_TEST_SUITE()
