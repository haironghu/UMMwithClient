/* ========================================================================
 * test_cross_node.c -- Cross-node chunk sharing test (v2.0)
 *
 * v2.0: No Region concept. Only Chunk. All memory is CXL shared.
 * Every allocation follows the unified path and is registered in ummd.
 *
 * Simulates two nodes using mock transport:
 *   - Node 0: init, alloc chunk, write data
 *   - Node 0 chunk is registered in ummd (embedded)
 *   - Verify Node 0 can read its own data
 *   - Verify chunk is listed by ummd
 * ======================================================================== */

#include "../include/umm.h"
#include "test_framework.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------------
 * Configuration for Node 0
 * ------------------------------------------------------------------------ */
static UMMConfig node0_cfg = {
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
 * Test 1: node_alloc
 * Node 0: alloc a chunk, write data, read back, verify.
 * ------------------------------------------------------------------------ */
TEST(node_alloc)
{
    ensure_clean_state();

    int rc = umm_init(&node0_cfg);
    ASSERT_EQ(rc, UMM_OK);

    ChunkDescriptor desc;
    memset(&desc, 0, sizeof(desc));

    rc = umm_alloc(4096, &desc);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_NE(desc.chunk_id, 0);

    /* Verify GPA encodes node 0 */
    ASSERT_EQ(gpa_to_node(desc.base_gpa), node0_cfg.my_node_id);

    /* Write a test message */
    const char *test_msg = "Hello from Node 0! This is shared chunk data.";
    size_t msg_len = strlen(test_msg) + 1;

    rc = umm_write(&desc, 0, msg_len, test_msg);
    ASSERT_EQ(rc, UMM_OK);

    /* Read it back */
    char read_buf[128] = {0};
    rc = umm_read(&desc, 0, msg_len, read_buf);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(strcmp(read_buf, test_msg), 0);

    /* Clean up */
    rc = umm_free(&desc);
    ASSERT_EQ(rc, UMM_OK);

    umm_deinit();
}

/* ------------------------------------------------------------------------
 * Test 2: chunk_visible_in_umd
 * After alloc, the chunk is auto-registered in ummd.
 * Look it up by auto-generated name and verify metadata.
 * ------------------------------------------------------------------------ */
TEST(chunk_visible_in_umd)
{
    ensure_clean_state();

    int rc = umm_init(&node0_cfg);
    ASSERT_EQ(rc, UMM_OK);

    ChunkDescriptor desc;
    rc = umm_alloc(4096, &desc);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_NE(desc.chunk_id, 0);

    /* Build the auto-generated name that ummd assigned */
    uint64_t offset = gpa_to_offset(desc.base_gpa);
    char chunk_name[64];
    snprintf(chunk_name, sizeof(chunk_name), "chunk_%u_%lu",
             (unsigned)node0_cfg.my_node_id, (unsigned long)offset);

    /* Look up the chunk in ummd */
    ChunkMetadata meta;
    memset(&meta, 0, sizeof(meta));
    rc = umm_lookup_chunk(chunk_name, &meta);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(meta.gpa, desc.base_gpa);
    ASSERT_EQ(meta.size, desc.user_size);
    ASSERT_EQ(strcmp(meta.name, chunk_name), 0);

    /* Clean up */
    rc = umm_free(&desc);
    ASSERT_EQ(rc, UMM_OK);

    umm_deinit();
}

/* ------------------------------------------------------------------------
 * Test 3: gpa_encoding
 * Verify that GPA correctly encodes the node ID in the upper 8 bits.
 * ------------------------------------------------------------------------ */
TEST(gpa_encoding)
{
    ensure_clean_state();

    int rc = umm_init(&node0_cfg);
    ASSERT_EQ(rc, UMM_OK);

    ChunkDescriptor desc;
    rc = umm_alloc(4096, &desc);
    ASSERT_EQ(rc, UMM_OK);

    /* GPA node bits should match my_node_id (bits [63:58]) */
    ASSERT_EQ(gpa_to_node(desc.base_gpa), node0_cfg.my_node_id);

    /* The tier_id should be CXL (bits [57:56]) */
    ASSERT_EQ(gpa_to_tier(desc.base_gpa), UMM_TIER_CXL);

    /* Full extraction via inline helpers */
    node_id_t node = gpa_to_node(desc.base_gpa);
    tier_id_t tier = gpa_to_tier(desc.base_gpa);
    uint64_t  offset = gpa_to_offset(desc.base_gpa);
    ASSERT_EQ(node, node0_cfg.my_node_id);
    ASSERT_EQ(tier, UMM_TIER_CXL);

    /* GPA should be reconstructable */
    gpa_t reconstructed = make_gpa(node0_cfg.my_node_id, UMM_TIER_CXL, offset);
    ASSERT_EQ(reconstructed, desc.base_gpa);

    umm_free(&desc);
    umm_deinit();
}

/* ------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------ */
TEST_SUITE("Cross-Node Chunk Sharing v2.0")
{
    RUN_TEST(node_alloc);
    RUN_TEST(chunk_visible_in_umd);
    RUN_TEST(gpa_encoding);
}
END_TEST_SUITE()
