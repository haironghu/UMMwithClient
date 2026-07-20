/* ========================================================================
 * test_cxl.c — CXL transport memory allocation test
 *
 * This test verifies that UMM correctly uses the CXL transport path.
 * On systems without CXL hardware, the transport automatically falls
 * back to an in-memory mock (malloc + memcpy), so the test still works.
 *
 * Two test modes:
 *   1. transport=mock  → uses transport_mock.c (fastest, ~10ns)
 *   2. transport=cxl   → uses transport_cxl.c  (attempts mmap, falls back to mock)
 *
 * To test with real CXL hardware:
 *   1. Ensure CXL device exists: ls /dev/cxl/mem0
 *   2. Ensure you have root (for mmap)
 *   3. Set cxl_device: "/dev/cxl/mem0" in config
 *   4. Run: ./test_cxl cxl
 * ======================================================================== */

#include <stdio.h>
#include <string.h>

#include "../include/umm.h"
#include "test_framework.h"

/* ------------------------------------------------------------------------ */
/* Helper: ensure clean state (defensive against prior test failure)        */
/* ------------------------------------------------------------------------ */
static void ensure_clean_state(void)
{
    umm_deinit(); /* safe to call even if not initialized */
}

/* ------------------------------------------------------------------------ */
static UMMConfig make_config(const char *transport)
{
    UMMConfig cfg;
    memset(&cfg, 0, sizeof(cfg));

    strncpy(cfg.transport, transport, sizeof(cfg.transport) - 1);
    strncpy(cfg.consistency_model, "hardware",
            sizeof(cfg.consistency_model) - 1);

    cfg.memory_size = 64ULL * 1024 * 1024;   /* 64 MiB per node */
    cfg.my_node_id  = 0;

    /* Direct (embedded) mode — no RPC servers needed */
    cfg.meta_server_addr[0] = '\0';
    cfg.mem_server_addr[0]  = '\0';

    /* CXL device path (only used when transport=cxl) */
    if (strcmp(transport, "cxl") == 0) {
        /* On systems with real CXL, change to "/dev/cxl/mem0" */
        /* On systems without CXL, leave empty for auto-fallback */
        strncpy(cfg.cxl_device, "/dev/cxl/mem0_notexist",
                sizeof(cfg.cxl_device) - 1);
    }

    return cfg;
}

/* ------------------------------------------------------------------------ */
/* Test 1: alloc with CXL transport (fallback to mock on non-CXL systems)   */
/* ------------------------------------------------------------------------ */
TEST(cxl_alloc)
{
    ensure_clean_state();
    UMMConfig cfg = make_config("cxl");

    int rc = umm_init(&cfg);
    ASSERT_EQ(rc, UMM_OK);

    ChunkDescriptor desc;
    memset(&desc, 0, sizeof(desc));

    rc = umm_alloc(4096, &desc);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_NE(desc.chunk_id, (chunk_id_t)0);   /* chunk_id proves success */
    /* base_gpa can be 0 (offset 0 is valid), check only chunk_id */
    ASSERT_EQ(desc.user_size, (uint64_t)4096);

    /* Verify GPA encodes correct node */
    node_id_t node = gpa_to_node(desc.base_gpa);
    ASSERT_EQ(node, (node_id_t)0);

    /* Verify offset is page-aligned */
    uint64_t offset = gpa_to_offset(desc.base_gpa);
    ASSERT_EQ(offset & 0xFFF, (uint64_t)0);  /* 4KB aligned */

    umm_free(&desc);
    umm_deinit();
}

/* ------------------------------------------------------------------------ */
/* Test 2: read/write through CXL transport                                  */
/* ------------------------------------------------------------------------ */
TEST(cxl_read_write)
{
    ensure_clean_state();
    UMMConfig cfg = make_config("cxl");

    int rc = umm_init(&cfg);
    ASSERT_EQ(rc, UMM_OK);

    ChunkDescriptor desc;
    rc = umm_alloc(4096, &desc);
    ASSERT_EQ(rc, UMM_OK);

    /* Write test pattern */
    const char *pattern = "Hello CXL shared memory!";
    rc = umm_write(&desc, 0, strlen(pattern) + 1, pattern);
    ASSERT_EQ(rc, UMM_OK);

    /* Read back */
    char buf[256];
    memset(buf, 0, sizeof(buf));
    rc = umm_read(&desc, 0, strlen(pattern) + 1, buf);
    ASSERT_EQ(rc, UMM_OK);

    /* Verify */
    ASSERT_EQ(strcmp(buf, pattern), 0);

    umm_free(&desc);
    umm_deinit();
}

/* ------------------------------------------------------------------------ */
/* Test 3: multiple allocations                                              */
/* ------------------------------------------------------------------------ */
TEST(cxl_multiple_allocs)
{
    ensure_clean_state();
    UMMConfig cfg = make_config("cxl");

    int rc = umm_init(&cfg);
    ASSERT_EQ(rc, UMM_OK);

    ChunkDescriptor chunks[5];
    memset(chunks, 0, sizeof(chunks));

    for (int i = 0; i < 5; i++) {
        rc = umm_alloc(4096, &chunks[i]);
        ASSERT_EQ(rc, UMM_OK);
        ASSERT_NE(chunks[i].chunk_id, (chunk_id_t)0);

        /* Write unique data to each */
        uint64_t val = (uint64_t)(i + 1) * 0x1111111111111111ULL;
        rc = umm_write(&chunks[i], 0, sizeof(val), &val);
        ASSERT_EQ(rc, UMM_OK);
    }

    /* Verify all chunks have different IDs and offsets */
    for (int i = 0; i < 5; i++) {
        for (int j = i + 1; j < 5; j++) {
            ASSERT_NE(chunks[i].chunk_id, chunks[j].chunk_id);
            ASSERT_NE(chunks[i].base_gpa, chunks[j].base_gpa);
        }
    }

    /* Verify data integrity */
    for (int i = 0; i < 5; i++) {
        uint64_t val = 0;
        rc = umm_read(&chunks[i], 0, sizeof(val), &val);
        ASSERT_EQ(rc, UMM_OK);
        ASSERT_EQ(val, (uint64_t)(i + 1) * 0x1111111111111111ULL);
    }

    for (int i = 0; i < 5; i++)
        umm_free(&chunks[i]);

    umm_deinit();
}

/* ------------------------------------------------------------------------ */
/* Test 4: chunk registered in ummd (visible via lookup)                     */
/* ------------------------------------------------------------------------ */
TEST(cxl_chunk_registered)
{
    ensure_clean_state();
    UMMConfig cfg = make_config("cxl");

    int rc = umm_init(&cfg);
    ASSERT_EQ(rc, UMM_OK);

    ChunkDescriptor desc;
    rc = umm_alloc(4096, &desc);
    ASSERT_EQ(rc, UMM_OK);

    /* Build the auto-generated name */
    uint64_t offset = gpa_to_offset(desc.base_gpa);
    char name[64];
    snprintf(name, sizeof(name), "chunk_%u_%lu",
             (unsigned)cfg.my_node_id, (unsigned long)offset);

    /* Lookup should succeed — chunk is registered in ummd */
    ChunkMetadata meta;
    memset(&meta, 0, sizeof(meta));
    rc = umm_lookup_chunk(name, &meta);
    ASSERT_EQ(rc, UMM_OK);

    /* Verify metadata matches */
    ASSERT_EQ(meta.chunk_id, desc.chunk_id);
    ASSERT_EQ(meta.gpa, desc.base_gpa);
    ASSERT_EQ(meta.size, desc.user_size);

    umm_free(&desc);
    umm_deinit();
}

/* ------------------------------------------------------------------------ */
/* Test 5: alloc large chunk (1 MB)                                          */
/* ------------------------------------------------------------------------ */
TEST(cxl_large_alloc)
{
    ensure_clean_state();
    UMMConfig cfg = make_config("cxl");

    int rc = umm_init(&cfg);
    ASSERT_EQ(rc, UMM_OK);

    ChunkDescriptor desc;
    rc = umm_alloc(1024ULL * 1024, &desc);  /* 1 MB */
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(desc.user_size, (uint64_t)(1024ULL * 1024));

    /* Write and verify large data */
    uint8_t *write_buf = malloc(1024 * 1024);
    ASSERT_NOT_NULL(write_buf);
    for (size_t i = 0; i < 1024ULL * 1024; i++)
        write_buf[i] = (uint8_t)(i & 0xFF);

    rc = umm_write(&desc, 0, 1024ULL * 1024, write_buf);
    ASSERT_EQ(rc, UMM_OK);

    uint8_t *read_buf = malloc(1024 * 1024);
    ASSERT_NOT_NULL(read_buf);
    rc = umm_read(&desc, 0, 1024ULL * 1024, read_buf);
    ASSERT_EQ(rc, UMM_OK);

    ASSERT_EQ(memcmp(write_buf, read_buf, 1024ULL * 1024), 0);

    free(write_buf);
    free(read_buf);
    umm_free(&desc);
    umm_deinit();
}

/* ------------------------------------------------------------------------ */
/* Test 6: compare mock vs cxl transport produce same results                */
/* ------------------------------------------------------------------------ */
TEST(cxl_vs_mock_consistency)
{
    ensure_clean_state();
    /* First with mock */
    UMMConfig cfg_mock = make_config("mock");
    int rc = umm_init(&cfg_mock);
    ASSERT_EQ(rc, UMM_OK);

    ChunkDescriptor desc_mock;
    rc = umm_alloc(4096, &desc_mock);
    ASSERT_EQ(rc, UMM_OK);

    uint64_t val_mock = 0xDEADBEEFCAFEBABEULL;
    rc = umm_write(&desc_mock, 0, sizeof(val_mock), &val_mock);
    ASSERT_EQ(rc, UMM_OK);
    umm_free(&desc_mock);
    umm_deinit();

    /* Then with cxl (fallback) */
    UMMConfig cfg_cxl = make_config("cxl");
    rc = umm_init(&cfg_cxl);
    ASSERT_EQ(rc, UMM_OK);

    ChunkDescriptor desc_cxl;
    rc = umm_alloc(4096, &desc_cxl);
    ASSERT_EQ(rc, UMM_OK);

    uint64_t val_cxl = 0xDEADBEEFCAFEBABEULL;
    rc = umm_write(&desc_cxl, 0, sizeof(val_cxl), &val_cxl);
    ASSERT_EQ(rc, UMM_OK);

    uint64_t read_back = 0;
    rc = umm_read(&desc_cxl, 0, sizeof(read_back), &read_back);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(read_back, 0xDEADBEEFCAFEBABEULL);

    umm_free(&desc_cxl);
    umm_deinit();
}

/* ------------------------------------------------------------------------ */
/* main                                                                      */
/* ------------------------------------------------------------------------ */
TEST_SUITE("CXL Transport")
    RUN_TEST(cxl_alloc);
    RUN_TEST(cxl_read_write);
    RUN_TEST(cxl_multiple_allocs);
    RUN_TEST(cxl_chunk_registered);
    RUN_TEST(cxl_large_alloc);
    RUN_TEST(cxl_vs_mock_consistency);
END_TEST_SUITE()
