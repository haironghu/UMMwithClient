/* ========================================================================
 * test_ssd_simple_dist.c -- Simple distributed SSD test (no transport)
 *
 * Core idea: SSD is shared storage. All nodes see the same device file.
 * umd stores metadata (name -> GPA). GPA encodes the offset.
 * Data flows directly through the SSD device file, NOT through transport.
 *
 * Phase 1 (Node 0): umm_alloc_tiered(SSD) → allocates offset in device + registers in umd
 *                    open(SSD_DEVICE) + mmap(offset) → write data directly
 * Phase 2 (Node 1): umm_lookup_chunk → gets GPA → parses offset
 *                    open(SSD_DEVICE) + mmap(offset) → read
 *
 * No transport involved for data movement. The SSD device file IS the shared medium.
 * ======================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdint.h>

#include "../include/umm.h"
#include "../src/transport/ssd_pool.h"
#include "test_framework.h"

#define SSD_DEVICE "/tmp/umm_ssd_shared/pool.raw"

static void cleanup(void) {
    system("rm -rf /tmp/umm_ssd_shared");
}

/* ======================================================================== */
/* Test: Node 0 writes SSD file, Node 1 reads the same file                */
/* ======================================================================== */
TEST(sdist_basic)
{
    cleanup();

    /* =================================================================== */
    /* Phase 1: Node 0 — alloc SSD chunk, write directly to device         */
    /* =================================================================== */
    printf("\n[Phase 1] Node 0: alloc_tiered(SSD) -> write to device\n");

    UMMConfig cfg0;
    memset(&cfg0, 0, sizeof(cfg0));
    cfg0.transport[0] = '\0';          /* tier-aware mode */
    cfg0.memory_size  = 64 * 1024 * 1024;
    cfg0.my_node_id   = 0;

    int rc = umm_init(&cfg0);
    ASSERT_EQ(rc, UMM_OK);

    /* Register SSD tier (creates ssd_backend internally) */
    rc = umm_register_storage_tier(UMM_TIER_SSD, SSD_DEVICE, cfg0.memory_size);
    ASSERT_EQ(rc, UMM_OK);

    /* Allocate 4KB on SSD — this allocates from the device AND registers in umd */
    ChunkDescriptor desc;
    rc = umm_alloc_tiered(4096, UMM_TIER_SSD, &desc);
    ASSERT_EQ(rc, UMM_OK);

    uint64_t offset = gpa_to_offset(desc.base_gpa);
    printf("  Allocated: chunk_id=%lu, gpa=0x%lx, offset=%lu\n",
           (unsigned long)desc.chunk_id, (unsigned long)desc.base_gpa,
           (unsigned long)offset);

    /* === Write data DIRECTLY to the SSD device (no transport) === */
    /* Open the shared device file and mmap at the allocated offset. */
    int fd = open(SSD_DEVICE, O_RDWR);
    ASSERT_TRUE(fd >= 0);

    void *ptr = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)offset);
    ASSERT_TRUE(ptr != MAP_FAILED);

    const char *msg = "Hello from Node 0 via shared SSD!";
    memcpy(ptr, msg, strlen(msg) + 1);
    msync(ptr, 4096, MS_SYNC);

    printf("  Wrote to %s at offset %lu: \"%s\"\n", SSD_DEVICE, (unsigned long)offset, msg);

    munmap(ptr, 4096);
    close(fd);
    printf("  Node 0 done (umd still running with chunk registered).\n");

    /* =================================================================== */
    /* Phase 2: Node 1 — lookup in the SAME umd, read directly from device  */
    /*                                                                      */
    /* In a real distributed setup, Node 1 would be a separate process      */
    /* connecting to the same ummd via RPC. Here we simulate by looking up  */
    /* in the same in-process umd (the metadata is shared).                 */
    /* =================================================================== */
    printf("\n[Phase 2] Node 1: lookup_chunk (same umd) -> open device -> read\n");

    /* Build chunk name (same format as umm_alloc_tiered uses) */
    char chunk_name[64];
    snprintf(chunk_name, sizeof(chunk_name), "chunk_%u_%lu_tier%u",
             0, (unsigned long)offset, (unsigned)UMM_TIER_SSD);

    /* Lookup chunk metadata from umd (same umd that Node 0 used) */
    ChunkMetadata meta;
    rc = umm_lookup_chunk(chunk_name, &meta);
    ASSERT_EQ(rc, UMM_OK);

    printf("  Found '%s': gpa=0x%lx, size=%lu\n",
           chunk_name, (unsigned long)meta.gpa, (unsigned long)meta.size);

    /* Decode GPA to get offset */
    uint64_t read_offset = gpa_to_offset(meta.gpa);
    ASSERT_EQ(read_offset, offset);   /* same offset */
    ASSERT_EQ(gpa_to_tier(meta.gpa), UMM_TIER_SSD);

    /* === Read DIRECTLY from the same SSD device (no transport) === */
    /* Open the shared device file and mmap at the recorded offset. */
    int fd2 = open(SSD_DEVICE, O_RDONLY);
    ASSERT_TRUE(fd2 >= 0);

    void *ptr2 = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd2, (off_t)read_offset);
    ASSERT_TRUE(ptr2 != MAP_FAILED);

    char buf[256];
    memcpy(buf, ptr2, strlen(msg) + 1);
    printf("  Read from %s at offset %lu: \"%s\"\n", SSD_DEVICE, (unsigned long)read_offset, buf);

    ASSERT_EQ(strcmp(buf, msg), 0);

    munmap(ptr2, 4096);
    close(fd2);

    /* Shutdown umd */
    umm_deinit();
    cleanup();
}

/* ======================================================================== */
TEST(sdist_persistence)
{
    cleanup();

    printf("\n[Persistence test]\n");

    /* Node 0: create chunk, write, deinit (device file survives) */
    UMMConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.transport[0] = '\0';
    cfg.memory_size  = 64 * 1024 * 1024;
    cfg.my_node_id   = 0;

    int rc = umm_init(&cfg);
    ASSERT_EQ(rc, UMM_OK);

    rc = umm_register_storage_tier(UMM_TIER_SSD, SSD_DEVICE, cfg.memory_size);
    ASSERT_EQ(rc, UMM_OK);

    ChunkDescriptor desc;
    rc = umm_alloc_tiered(4096, UMM_TIER_SSD, &desc);
    ASSERT_EQ(rc, UMM_OK);

    uint64_t off = gpa_to_offset(desc.base_gpa);

    /* Write directly to the SSD device at the allocated offset */
    int fd = open(SSD_DEVICE, O_RDWR);
    ASSERT_TRUE(fd >= 0);
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)off);
    ASSERT_TRUE(p != MAP_FAILED);
    strcpy((char *)p, "PERSISTENT_DATA_42");
    msync(p, 4096, MS_SYNC);
    munmap(p, 4096); close(fd);

    /* Destroy everything (including ssd_backend) */
    umm_deinit();

    /* Simulate "Node 1 comes later": re-open device, data still there */
    fd = open(SSD_DEVICE, O_RDONLY);
    ASSERT_TRUE(fd >= 0);
    p = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, (off_t)off);
    ASSERT_TRUE(p != MAP_FAILED);

    char buf[64];
    memcpy(buf, p, 19);
    buf[19] = '\0';
    ASSERT_EQ(strcmp(buf, "PERSISTENT_DATA_42"), 0);
    printf("  Data survived after umms restart: \"%s\"\n", buf);

    munmap(p, 4096); close(fd);
    cleanup();
}

/* ======================================================================== */
TEST_SUITE("Simple SSD Dist (no transport)")
    RUN_TEST(sdist_basic);
    RUN_TEST(sdist_persistence);
END_TEST_SUITE()
