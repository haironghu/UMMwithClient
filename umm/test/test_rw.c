/* ========================================================================
 * test_rw.c -- Unified Read/Write Test
 *
 * Verifies umm_read/write routes to the correct device based on GPA tier.
 *
 * Test scenarios:
 *   1. CXL memory: alloc -> write -> read -> verify
 *   2. SSD storage: alloc_tiered(SSD) -> write -> read -> verify
 *   3. Data isolation: CXL and SSD data do not interfere
 *   4. Multiple SSD chunks: sequential allocation and readback
 *
 * Architecture:
 *   umm_write -> tier_router -> gpa_to_tier(gpa) -> [CXL|SSD] transport
 *   CXL: tier_router -> mock_transport -> memcpy (local memory)
 *   SSD: tier_router -> ssd_transport -> ssd_backend -> device file
 *
 * Usage:
 *   ./test_rw
 * ======================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../include/umm.h"
#include "test_framework.h"

#define TEST_SSD_DIR "/tmp/umm_test_rw_ssd"
#define TEST_MEM_SIZE (64ULL * 1024 * 1024)  /* 64 MiB */

/* ------------------------------------------------------------------------ */
/* Cleanup helper                                                           */
/* ------------------------------------------------------------------------ */
static void cleanup(void)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", TEST_SSD_DIR);
    system(cmd);
}

/* ------------------------------------------------------------------------ */
/* Make test config (direct mode, tier-aware)                               */
/* ------------------------------------------------------------------------ */
static UMMConfig make_config(void)
{
    UMMConfig cfg;
    memset(&cfg, 0, sizeof(cfg));

    /* Tier-aware mode: transport empty string */
    cfg.transport[0]      = '\0';
    cfg.consistency_model[0] = '\0';
    cfg.memory_size       = TEST_MEM_SIZE;
    cfg.my_node_id        = 0;

    /* Direct mode -- no RPC servers */
    cfg.meta_server_addr[0] = '\0';
    cfg.mem_server_addr[0]  = '\0';

    return cfg;
}

/* ------------------------------------------------------------------------ */
/* Test 1: CXL memory read/write                                            */
/* ------------------------------------------------------------------------ */
TEST(cx1_rw_basic)
{
    cleanup();

    UMMConfig cfg = make_config();
    int rc = umm_init(&cfg);
    ASSERT_EQ(rc, UMM_OK);

    /* Register SSD tier (so topology has both CXL + SSD) */
    rc = umm_register_storage_tier(UMM_TIER_SSD, TEST_SSD_DIR, TEST_MEM_SIZE);
    ASSERT_EQ(rc, UMM_OK);

    /* Allocate 4KB on CXL tier (default) */
    ChunkDescriptor desc;
    memset(&desc, 0, sizeof(desc));
    rc = umm_alloc(4096, &desc);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(desc.user_size, (uint64_t)4096);

    /* tier should be CXL */
    tier_id_t tier = gpa_to_tier(desc.base_gpa);
    ASSERT_EQ(tier, UMM_TIER_CXL);

    /* Write data */
    const char *msg = "Hello CXL World!";
    rc = umm_write(&desc, 0, strlen(msg) + 1, msg);
    ASSERT_EQ(rc, UMM_OK);

    /* Read back */
    char buf[64];
    memset(buf, 0, sizeof(buf));
    rc = umm_read(&desc, 0, strlen(msg) + 1, buf);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(strcmp(buf, msg), 0);

    /* Cleanup */
    umm_free(&desc);
    umm_deinit();
    cleanup();
}

/* ------------------------------------------------------------------------ */
/* Test 2: SSD storage read/write                                           */
/* ------------------------------------------------------------------------ */
TEST(ssd_rw_basic)
{
    cleanup();

    UMMConfig cfg = make_config();
    int rc = umm_init(&cfg);
    ASSERT_EQ(rc, UMM_OK);

    /* Register SSD storage */
    rc = umm_register_storage_tier(UMM_TIER_SSD, TEST_SSD_DIR, TEST_MEM_SIZE);
    ASSERT_EQ(rc, UMM_OK);

    /* Allocate 4KB on SSD tier */
    ChunkDescriptor desc;
    memset(&desc, 0, sizeof(desc));
    rc = umm_alloc_tiered(4096, UMM_TIER_SSD, &desc);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(desc.user_size, (uint64_t)4096);

    /* tier should be SSD */
    tier_id_t tier = gpa_to_tier(desc.base_gpa);
    ASSERT_EQ(tier, UMM_TIER_SSD);

    /* Write data */
    const char *msg = "Hello SSD World!";
    rc = umm_write(&desc, 0, strlen(msg) + 1, msg);
    ASSERT_EQ(rc, UMM_OK);

    /* Read back */
    char buf[64];
    memset(buf, 0, sizeof(buf));
    rc = umm_read(&desc, 0, strlen(msg) + 1, buf);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(strcmp(buf, msg), 0);

    /* Cleanup */
    umm_free(&desc);
    umm_deinit();
    cleanup();
}

/* ------------------------------------------------------------------------ */
/* Test 3: Data isolation between CXL and SSD                               */
/* ------------------------------------------------------------------------ */
TEST(data_isolation_cxl_vs_ssd)
{
    cleanup();

    UMMConfig cfg = make_config();
    int rc = umm_init(&cfg);
    ASSERT_EQ(rc, UMM_OK);

    rc = umm_register_storage_tier(UMM_TIER_SSD, TEST_SSD_DIR, TEST_MEM_SIZE);
    ASSERT_EQ(rc, UMM_OK);

    /* Allocate both CXL and SSD chunks */
    ChunkDescriptor cx1_desc;
    ChunkDescriptor ssd_desc;
    memset(&cx1_desc, 0, sizeof(cx1_desc));
    memset(&ssd_desc, 0, sizeof(ssd_desc));

    rc = umm_alloc(4096, &cx1_desc);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(gpa_to_tier(cx1_desc.base_gpa), UMM_TIER_CXL);

    rc = umm_alloc_tiered(4096, UMM_TIER_SSD, &ssd_desc);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(gpa_to_tier(ssd_desc.base_gpa), UMM_TIER_SSD);

    /* Write different data to each */
    const char *cx1_msg = "CXL_DATA_42";
    const char *ssd_msg = "SSD_DATA_99";

    rc = umm_write(&cx1_desc, 0, strlen(cx1_msg) + 1, cx1_msg);
    ASSERT_EQ(rc, UMM_OK);

    rc = umm_write(&ssd_desc, 0, strlen(ssd_msg) + 1, ssd_msg);
    ASSERT_EQ(rc, UMM_OK);

    /* Read back and verify no cross-contamination */
    char buf[64];

    memset(buf, 0, sizeof(buf));
    rc = umm_read(&cx1_desc, 0, strlen(cx1_msg) + 1, buf);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(strcmp(buf, cx1_msg), 0);

    memset(buf, 0, sizeof(buf));
    rc = umm_read(&ssd_desc, 0, strlen(ssd_msg) + 1, buf);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(strcmp(buf, ssd_msg), 0);

    /* Ensure SSD data is different from CXL data */
    memset(buf, 0, sizeof(buf));
    rc = umm_read(&ssd_desc, 0, strlen(cx1_msg) + 1, buf);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_NE(strcmp(buf, cx1_msg), 0);

    umm_free(&cx1_desc);
    umm_free(&ssd_desc);
    umm_deinit();
    cleanup();
}

/* ------------------------------------------------------------------------ */
/* Test 4: Multiple SSD chunks                                              */
/* ------------------------------------------------------------------------ */
TEST(multiple_ssd_chunks)
{
    cleanup();

    UMMConfig cfg = make_config();
    int rc = umm_init(&cfg);
    ASSERT_EQ(rc, UMM_OK);

    rc = umm_register_storage_tier(UMM_TIER_SSD, TEST_SSD_DIR, TEST_MEM_SIZE);
    ASSERT_EQ(rc, UMM_OK);

    /* Allocate 3 SSD chunks */
    ChunkDescriptor descs[3];
    const char *msgs[3] = {
        "SSD_CHUNK_0_DATA",
        "SSD_CHUNK_1_DATA",
        "SSD_CHUNK_2_DATA"
    };

    for (int i = 0; i < 3; i++) {
        memset(&descs[i], 0, sizeof(descs[i]));
        rc = umm_alloc_tiered(4096, UMM_TIER_SSD, &descs[i]);
        ASSERT_EQ(rc, UMM_OK);
        ASSERT_EQ(gpa_to_tier(descs[i].base_gpa), UMM_TIER_SSD);

        /* Write unique data */
        rc = umm_write(&descs[i], 0, strlen(msgs[i]) + 1, msgs[i]);
        ASSERT_EQ(rc, UMM_OK);
    }

    /* Verify each chunk has correct data */
    for (int i = 0; i < 3; i++) {
        char buf[64];
        memset(buf, 0, sizeof(buf));
        rc = umm_read(&descs[i], 0, strlen(msgs[i]) + 1, buf);
        ASSERT_EQ(rc, UMM_OK);
        ASSERT_EQ(strcmp(buf, msgs[i]), 0);
    }

    /* Verify chunks are at different offsets */
    ASSERT_NE(descs[0].base_gpa, descs[1].base_gpa);
    ASSERT_NE(descs[1].base_gpa, descs[2].base_gpa);

    for (int i = 0; i < 3; i++) {
        umm_free(&descs[i]);
    }
    umm_deinit();
    cleanup();
}

/* ------------------------------------------------------------------------ */
/* Test 5: Large data transfer (1MB)                                        */
/* ------------------------------------------------------------------------ */
TEST(large_transfer_1mb)
{
    cleanup();

    UMMConfig cfg = make_config();
    int rc = umm_init(&cfg);
    ASSERT_EQ(rc, UMM_OK);

    rc = umm_register_storage_tier(UMM_TIER_SSD, TEST_SSD_DIR, TEST_MEM_SIZE);
    ASSERT_EQ(rc, UMM_OK);

    size_t size = 1024 * 1024;  /* 1 MB */

    /* CXL large transfer */
    ChunkDescriptor cx1_desc;
    memset(&cx1_desc, 0, sizeof(cx1_desc));
    rc = umm_alloc(size, &cx1_desc);
    ASSERT_EQ(rc, UMM_OK);

    uint8_t *write_buf = malloc(size);
    uint8_t *read_buf  = malloc(size);
    ASSERT_NOT_NULL(write_buf);
    ASSERT_NOT_NULL(read_buf);

    /* Fill with pattern */
    for (size_t i = 0; i < size; i++) {
        write_buf[i] = (uint8_t)(i % 256);
    }

    rc = umm_write(&cx1_desc, 0, size, write_buf);
    ASSERT_EQ(rc, UMM_OK);

    memset(read_buf, 0, size);
    rc = umm_read(&cx1_desc, 0, size, read_buf);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(memcmp(write_buf, read_buf, size), 0);

    /* SSD large transfer */
    ChunkDescriptor ssd_desc;
    memset(&ssd_desc, 0, sizeof(ssd_desc));
    rc = umm_alloc_tiered(size, UMM_TIER_SSD, &ssd_desc);
    ASSERT_EQ(rc, UMM_OK);

    /* Different pattern for SSD */
    for (size_t i = 0; i < size; i++) {
        write_buf[i] = (uint8_t)(255 - (i % 256));
    }

    rc = umm_write(&ssd_desc, 0, size, write_buf);
    ASSERT_EQ(rc, UMM_OK);

    memset(read_buf, 0, size);
    rc = umm_read(&ssd_desc, 0, size, read_buf);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(memcmp(write_buf, read_buf, size), 0);

    free(write_buf);
    free(read_buf);

    umm_free(&cx1_desc);
    umm_free(&ssd_desc);
    umm_deinit();
    cleanup();
}

/* ------------------------------------------------------------------------ */
/* Test 6: Offset write/read (not from beginning)                           */
/* ------------------------------------------------------------------------ */
TEST(offset_rw)
{
    cleanup();

    UMMConfig cfg = make_config();
    int rc = umm_init(&cfg);
    ASSERT_EQ(rc, UMM_OK);

    rc = umm_register_storage_tier(UMM_TIER_SSD, TEST_SSD_DIR, TEST_MEM_SIZE);
    ASSERT_EQ(rc, UMM_OK);

    ChunkDescriptor desc;
    memset(&desc, 0, sizeof(desc));
    rc = umm_alloc(4096, &desc);
    ASSERT_EQ(rc, UMM_OK);

    /* Write at offset 100 */
    const char *msg = "OffsetData";
    rc = umm_write(&desc, 100, strlen(msg) + 1, msg);
    ASSERT_EQ(rc, UMM_OK);

    /* Read from offset 100 */
    char buf[64];
    memset(buf, 0, sizeof(buf));
    rc = umm_read(&desc, 100, strlen(msg) + 1, buf);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(strcmp(buf, msg), 0);

    /* Verify offset 0 is not affected */
    memset(buf, 0, sizeof(buf));
    rc = umm_read(&desc, 0, 10, buf);
    ASSERT_EQ(rc, UMM_OK);
    /* First 10 bytes should be zeros (fresh allocation) */
    for (int i = 0; i < 10; i++) {
        ASSERT_EQ(buf[i], 0);
    }

    umm_free(&desc);
    umm_deinit();
    cleanup();
}

/* ------------------------------------------------------------------------ */
/* Main                                                                     */
/* ------------------------------------------------------------------------ */
TEST_SUITE("Unified Read/Write (tier-aware routing)")
    RUN_TEST(cx1_rw_basic);
    RUN_TEST(ssd_rw_basic);
    RUN_TEST(data_isolation_cxl_vs_ssd);
    RUN_TEST(multiple_ssd_chunks);
    RUN_TEST(large_transfer_1mb);
    RUN_TEST(offset_rw);
END_TEST_SUITE()
