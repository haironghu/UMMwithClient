/* ========================================================================
 * test_ssd_dist.c -- Distributed SSD Read/Write Integration Test
 *
 * Scenario: Simulates Node 0 (writer) and Node 1 (reader) in a single
 * process using direct mode with tier-aware transport.
 *
 * Architecture:
 *   - ummd: in-process metadata service (chunk directory)
 *   - umms-0: memory service for node 0 (CXL + SSD tiers)
 *   - umms-1: memory service for node 1 (CXL + SSD tiers)
 *   - Node 0: allocates SSD chunk, writes data
 *   - Node 1: looks up chunk by name, reads data via tier_router
 *
 * This proves the end-to-end data path:
 *   umm_alloc_tiered(SSD) -> umms-0 -> ba_alloc(SSD bitmap)
 *   umm_write -> tier_router -> transport_ssd -> ssd_backend -> chunk file
 *   umm_lookup_chunk -> ummd -> ChunkMetadata
 *   umm_read -> tier_router -> transport_ssd -> ssd_backend -> chunk file
 * ======================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "../include/umm.h"
#include "../src/common/config_parser.h"
#include "test_framework.h"

#define TEST_SSD_DIR_NODE0 "/tmp/umm_ssd_dist_node0"
#define TEST_SSD_DIR_NODE1 "/tmp/umm_ssd_dist_node1"

/* ------------------------------------------------------------------------ */
/* Cleanup helper                                                           */
/* ------------------------------------------------------------------------ */
static void cleanup(void)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s %s",
             TEST_SSD_DIR_NODE0, TEST_SSD_DIR_NODE1);
    system(cmd);
}

/* ------------------------------------------------------------------------ */
/* Test: Node 0 writes SSD, Node 1 reads                                    */
/* ------------------------------------------------------------------------ */
TEST(dist_ssd_node0_write_node1_read)
{
    cleanup();

    /* Phase 1: Node 0 (writer) initializes, allocates SSD chunk, writes */
    printf("\n[Phase 1] Node 0 (writer) init + alloc SSD + write\n");

    UMMConfig cfg0;
    memset(&cfg0, 0, sizeof(cfg0));
    strncpy(cfg0.transport, "", sizeof(cfg0.transport) - 1);  /* tier-aware */
    strncpy(cfg0.consistency_model, "hardware",
            sizeof(cfg0.consistency_model) - 1);
    cfg0.memory_size  = 64ULL * 1024 * 1024;  /* 64 MB */
    cfg0.my_node_id   = 0;
    /* meta/mem server addr empty = direct mode */

    int rc = umm_init(&cfg0);
    ASSERT_EQ(rc, UMM_OK);

    /* Register SSD storage tier (direct mode) */
    rc = umm_register_storage_tier(UMM_TIER_SSD,
                                    TEST_SSD_DIR_NODE0,
                                    cfg0.memory_size);
    ASSERT_EQ(rc, UMM_OK);

    /* Allocate 4KB chunk on SSD tier */
    ChunkDescriptor desc0;
    memset(&desc0, 0, sizeof(desc0));
    rc = umm_alloc_tiered(4096, UMM_TIER_SSD, &desc0);
    ASSERT_EQ(rc, UMM_OK);

    printf("  Node 0 allocated SSD chunk:\n");
    printf("    chunk_id : %lu\n", (unsigned long)desc0.chunk_id);
    printf("    base_gpa : 0x%016lx\n", (unsigned long)desc0.base_gpa);
    printf("    tier     : %s (tier_id=%u)\n",
           umm_tier_name(gpa_to_tier(desc0.base_gpa)),
           (unsigned)gpa_to_tier(desc0.base_gpa));
    printf("    size     : %lu bytes\n", (unsigned long)desc0.user_size);

    /* Build chunk name */
    uint64_t offset0 = gpa_to_offset(desc0.base_gpa);
    char chunk_name[64];
    snprintf(chunk_name, sizeof(chunk_name), "chunk_%u_%lu_tier%u",
             (unsigned)cfg0.my_node_id, (unsigned long)offset0,
             (unsigned)gpa_to_tier(desc0.base_gpa));
    printf("    name     : %s\n", chunk_name);

    /* Write data */
    const char *msg = "Hello from Node 0 via SSD shared storage!";
    rc = umm_write(&desc0, 0, strlen(msg) + 1, msg);
    ASSERT_EQ(rc, UMM_OK);
    printf("  Node 0 wrote: \"%s\"\n", msg);

    /* Get metadata for verification */
    ChunkMetadata meta0;
    memset(&meta0, 0, sizeof(meta0));
    rc = umm_lookup_chunk(chunk_name, &meta0);
    ASSERT_EQ(rc, UMM_OK);

    /* Phase 2: Simulate Node 1 reading the same chunk
     *
     * In a real distributed setup, Node 1 would have its own ummd
     * connection and would call umm_lookup_chunk() from a separate
     * process. Here we verify that the chunk metadata (GPA) is
     * correctly stored and can be used for cross-node reads.
     */
    printf("\n[Phase 2] Node 1 (reader) lookup + read via GPA\n");

    /* Lookup chunk by name (same ummd) */
    ChunkMetadata meta1;
    memset(&meta1, 0, sizeof(meta1));
    rc = umm_lookup_chunk(chunk_name, &meta1);
    ASSERT_EQ(rc, UMM_OK);

    printf("  Node 1 found chunk '%s':\n", chunk_name);
    printf("    chunk_id : %lu\n", (unsigned long)meta1.chunk_id);
    printf("    gpa      : 0x%016lx\n", (unsigned long)meta1.gpa);
    printf("    tier     : %s (tier_id=%u)\n",
           umm_tier_name(gpa_to_tier(meta1.gpa)),
           (unsigned)gpa_to_tier(meta1.gpa));
    printf("    size     : %lu bytes\n", (unsigned long)meta1.size);

    /* Verify GPA encoding */
    ASSERT_EQ(gpa_to_node(meta1.gpa), 0);        /* home node = 0 */
    ASSERT_EQ(gpa_to_tier(meta1.gpa), UMM_TIER_SSD);  /* SSD tier */
    ASSERT_EQ(meta1.gpa, meta0.gpa);             /* Same GPA */

    /* Read data through tier_router -> transport_ssd */
    ChunkDescriptor desc1;
    memset(&desc1, 0, sizeof(desc1));
    desc1.chunk_id  = meta1.chunk_id;
    desc1.base_gpa  = meta1.gpa;
    desc1.user_size = meta1.size;

    char buf[256];
    memset(buf, 0, sizeof(buf));
    rc = umm_read(&desc1, 0, sizeof(buf), buf);
    ASSERT_EQ(rc, UMM_OK);

    printf("  Node 1 read: \"%s\"\n", buf);
    ASSERT_EQ(strcmp(buf, msg), 0);

    umm_deinit();
    printf("  Node 1 deinited\n");

    cleanup();
}

/* ------------------------------------------------------------------------ */
/* Test: Multi-tier routing verification                                    */
/* ------------------------------------------------------------------------ */
TEST(dist_ssd_tier_routing)
{
    cleanup();

    UMMConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.transport, "", sizeof(cfg.transport) - 1);
    strncpy(cfg.consistency_model, "hardware",
            sizeof(cfg.consistency_model) - 1);
    cfg.memory_size = 64ULL * 1024 * 1024;
    cfg.my_node_id  = 0;

    int rc = umm_init(&cfg);
    ASSERT_EQ(rc, UMM_OK);

    /* Register SSD storage tier (direct mode) */
    rc = umm_register_storage_tier(UMM_TIER_SSD,
                                    TEST_SSD_DIR_NODE0,
                                    cfg.memory_size);
    ASSERT_EQ(rc, UMM_OK);

    /* Allocate CXL chunk */
    ChunkDescriptor desc_cxl;
    rc = umm_alloc(4096, &desc_cxl);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(gpa_to_tier(desc_cxl.base_gpa), UMM_TIER_CXL);

    /* Allocate SSD chunk */
    ChunkDescriptor desc_ssd;
    rc = umm_alloc_tiered(4096, UMM_TIER_SSD, &desc_ssd);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(gpa_to_tier(desc_ssd.base_gpa), UMM_TIER_SSD);

    printf("  CXL chunk: gpa=0x%016lx, tier=%s\n",
           (unsigned long)desc_cxl.base_gpa,
           umm_tier_name(gpa_to_tier(desc_cxl.base_gpa)));
    printf("  SSD chunk: gpa=0x%016lx, tier=%s\n",
           (unsigned long)desc_ssd.base_gpa,
           umm_tier_name(gpa_to_tier(desc_ssd.base_gpa)));

    /* Write different data to each tier */
    const char *msg_cxl = "CXL_DATA_12345";
    const char *msg_ssd = "SSD_DATA_ABCDE";

    rc = umm_write(&desc_cxl, 0, strlen(msg_cxl) + 1, msg_cxl);
    ASSERT_EQ(rc, UMM_OK);

    rc = umm_write(&desc_ssd, 0, strlen(msg_ssd) + 1, msg_ssd);
    ASSERT_EQ(rc, UMM_OK);

    /* Read back and verify isolation */
    char buf[256];

    memset(buf, 0, sizeof(buf));
    rc = umm_read(&desc_cxl, 0, sizeof(buf), buf);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(strcmp(buf, msg_cxl), 0);

    memset(buf, 0, sizeof(buf));
    rc = umm_read(&desc_ssd, 0, sizeof(buf), buf);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(strcmp(buf, msg_ssd), 0);

    /* Verify they are different (no cross-tier contamination) */
    ASSERT_NE(desc_cxl.base_gpa, desc_ssd.base_gpa);

    umm_free(&desc_cxl);
    umm_free(&desc_ssd);
    umm_deinit();
    cleanup();
}

/* ------------------------------------------------------------------------ */
/* main                                                                     */
/* ------------------------------------------------------------------------ */
TEST_SUITE("Distributed SSD Integration")
    RUN_TEST(dist_ssd_node0_write_node1_read);
    RUN_TEST(dist_ssd_tier_routing);
END_TEST_SUITE()
