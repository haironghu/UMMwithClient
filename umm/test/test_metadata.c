/* ========================================================================
 * test_metadata.c -- Unit tests for the metadata service v2.0 (direct/local)
 *
 * v2.0: No Region concept. Flat chunk namespace.
 * All chunks are registered in ummd with unique names.
 * ======================================================================== */

#include "../src/metadata_service/meta_service.h"
#include "../src/metadata_service/meta_service_direct.h"
#include "test_framework.h"

#include <stdint.h>
#include <string.h>

#include "../src/common/error_codes.h"

static MetadataServiceVtbl *vtbl = NULL;
static void *ctx = NULL;

static void setup_meta(void)
{
    ctx = NULL;
    vtbl = meta_service_direct_create(&ctx);
    ASSERT_NOT_NULL(vtbl);
    ASSERT_NOT_NULL(ctx);
}

static void teardown_meta(void)
{
    if (ctx) {
        meta_service_direct_destroy(ctx);
        ctx = NULL;
        vtbl = NULL;
    }
}

/* ------------------------------------------------------------------------
 * Test 1: register_and_lookup
 * Register a chunk, then look it up by name and verify gpa/size match.
 * ------------------------------------------------------------------------ */
TEST(register_and_lookup)
{
    setup_meta();

    gpa_t gpa = 0x1000;
    chunk_id_t cid = 0;
    int rc = vtbl->register_chunk(ctx, "test1", gpa, 4096, &cid);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_NE(cid, 0);

    ChunkMetadata meta;
    memset(&meta, 0, sizeof(meta));
    rc = vtbl->lookup_chunk(ctx, "test1", &meta);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(meta.gpa, gpa);
    ASSERT_EQ(meta.size, 4096);
    ASSERT_EQ(strcmp(meta.name, "test1"), 0);

    teardown_meta();
}

/* ------------------------------------------------------------------------
 * Test 2: lookup_not_found
 * Lookup a non-existent name, expect UMM_E_NOT_FOUND.
 * ------------------------------------------------------------------------ */
TEST(lookup_not_found)
{
    setup_meta();

    ChunkMetadata meta;
    memset(&meta, 0, sizeof(meta));
    int rc = vtbl->lookup_chunk(ctx, "no_such_chunk", &meta);
    ASSERT_EQ(rc, UMM_E_NOT_FOUND);

    teardown_meta();
}

/* ------------------------------------------------------------------------
 * Test 3: duplicate_name
 * Register the same name twice, expect ALREADY_EXISTS on second.
 * ------------------------------------------------------------------------ */
TEST(duplicate_name)
{
    setup_meta();

    chunk_id_t cid1 = 0, cid2 = 0;
    int rc = vtbl->register_chunk(ctx, "dup_chunk", 0x1000, 4096, &cid1);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_NE(cid1, 0);

    rc = vtbl->register_chunk(ctx, "dup_chunk", 0x2000, 8192, &cid2);
    ASSERT_EQ(rc, UMM_E_ALREADY_EXISTS);

    teardown_meta();
}

/* ------------------------------------------------------------------------
 * Test 4: register_multiple
 * Register 5 chunks with different names, all succeed.
 * ------------------------------------------------------------------------ */
TEST(register_multiple)
{
    setup_meta();

    const int num_chunks = 5;
    const char *names[5] = {"chunk_a", "chunk_b", "chunk_c", "chunk_d", "chunk_e"};
    chunk_id_t cids[5];

    for (int i = 0; i < num_chunks; i++) {
        gpa_t gpa = (gpa_t)(0x1000ULL * (i + 1));
        int rc = vtbl->register_chunk(ctx, names[i], gpa, 4096, &cids[i]);
        ASSERT_EQ(rc, UMM_OK);
        ASSERT_NE(cids[i], 0);
    }

    /* Verify all chunk IDs are unique */
    for (int i = 0; i < num_chunks; i++) {
        for (int j = i + 1; j < num_chunks; j++) {
            ASSERT_NE(cids[i], cids[j]);
        }
    }

    /* Verify each can be looked up */
    for (int i = 0; i < num_chunks; i++) {
        ChunkMetadata meta;
        int rc = vtbl->lookup_chunk(ctx, names[i], &meta);
        ASSERT_EQ(rc, UMM_OK);
        ASSERT_EQ(meta.size, 4096);
    }

    teardown_meta();
}

/* ------------------------------------------------------------------------
 * Test 5: lookup_by_id
 * Register a chunk, get chunk_id, then lookup_chunk_by_id and verify.
 * ------------------------------------------------------------------------ */
TEST(lookup_by_id)
{
    setup_meta();

    gpa_t gpa = 0x5000;
    chunk_id_t cid = 0;
    int rc = vtbl->register_chunk(ctx, "by_id_chunk", gpa, 8192, &cid);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_NE(cid, 0);

    ChunkMetadata meta;
    memset(&meta, 0, sizeof(meta));
    rc = vtbl->lookup_chunk_by_id(ctx, cid, &meta);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(meta.gpa, gpa);
    ASSERT_EQ(meta.size, 8192);
    ASSERT_EQ(strcmp(meta.name, "by_id_chunk"), 0);

    teardown_meta();
}

/* ------------------------------------------------------------------------
 * Test 6: unregister
 * Register a chunk, unregister it, then lookup should return NOT_FOUND.
 * ------------------------------------------------------------------------ */
TEST(unregister)
{
    setup_meta();

    chunk_id_t cid = 0;
    int rc = vtbl->register_chunk(ctx, "temp_chunk", 0x3000, 4096, &cid);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_NE(cid, 0);

    /* Verify it exists */
    ChunkMetadata meta;
    rc = vtbl->lookup_chunk(ctx, "temp_chunk", &meta);
    ASSERT_EQ(rc, UMM_OK);

    /* Unregister */
    rc = vtbl->unregister_chunk(ctx, cid);
    ASSERT_EQ(rc, UMM_OK);

    /* Verify it's gone */
    rc = vtbl->lookup_chunk(ctx, "temp_chunk", &meta);
    ASSERT_EQ(rc, UMM_E_NOT_FOUND);

    /* lookup_by_id should also fail */
    rc = vtbl->lookup_chunk_by_id(ctx, cid, &meta);
    ASSERT_EQ(rc, UMM_E_NOT_FOUND);

    teardown_meta();
}

/* ------------------------------------------------------------------------
 * Test 7: ref_counting
 * Register (ref=1), add_ref (ref=2), release_ref (ref=1),
 * release_ref (ref=0, auto-unregister), lookup returns NOT_FOUND.
 * ------------------------------------------------------------------------ */
TEST(ref_counting)
{
    setup_meta();

    chunk_id_t cid = 0;
    int rc = vtbl->register_chunk(ctx, "ref_chunk", 0x4000, 4096, &cid);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_NE(cid, 0);

    /* Add a reference -- chunk should still exist */
    rc = vtbl->add_ref(ctx, cid);
    ASSERT_EQ(rc, UMM_OK);

    /* Release one ref -- chunk should still exist */
    rc = vtbl->release_ref(ctx, cid);
    ASSERT_EQ(rc, UMM_OK);

    ChunkMetadata meta;
    rc = vtbl->lookup_chunk(ctx, "ref_chunk", &meta);
    ASSERT_EQ(rc, UMM_OK); /* still exists */

    /* Release final ref -- chunk should be auto-unregistered */
    rc = vtbl->release_ref(ctx, cid);
    ASSERT_EQ(rc, UMM_OK);

    /* After releasing all refs, chunk should be gone */
    rc = vtbl->lookup_chunk(ctx, "ref_chunk", &meta);
    ASSERT_EQ(rc, UMM_E_NOT_FOUND);

    teardown_meta();
}

/* ------------------------------------------------------------------------
 * Test 8: list_by_node
 * Register chunks with different node GPAs, list_chunks_by_node filters
 * correctly.
 * ------------------------------------------------------------------------ */
TEST(list_by_node)
{
    setup_meta();

    /* Register chunks on node 0 */
    chunk_id_t cid0a = 0, cid0b = 0;
    int rc = vtbl->register_chunk(ctx, "node0_chunk_a",
                                  make_gpa(0, UMM_TIER_CXL, 0x1000), 4096, &cid0a);
    ASSERT_EQ(rc, UMM_OK);
    rc = vtbl->register_chunk(ctx, "node0_chunk_b",
                              make_gpa(0, UMM_TIER_CXL, 0x2000), 4096, &cid0b);
    ASSERT_EQ(rc, UMM_OK);

    /* Register chunks on node 1 */
    chunk_id_t cid1a = 0, cid1b = 0;
    rc = vtbl->register_chunk(ctx, "node1_chunk_a",
                              make_gpa(1, UMM_TIER_CXL, 0x1000), 4096, &cid1a);
    ASSERT_EQ(rc, UMM_OK);
    rc = vtbl->register_chunk(ctx, "node1_chunk_b",
                              make_gpa(1, UMM_TIER_CXL, 0x2000), 8192, &cid1b);
    ASSERT_EQ(rc, UMM_OK);

    /* Register chunk on node 2 */
    chunk_id_t cid2 = 0;
    rc = vtbl->register_chunk(ctx, "node2_chunk",
                              make_gpa(2, UMM_TIER_CXL, 0x1000), 4096, &cid2);
    ASSERT_EQ(rc, UMM_OK);

    /* List chunks for node 0 -- expect 2 */
    ChunkMetadata out_array[8];
    uint32_t count = 8;
    rc = vtbl->list_chunks_by_node(ctx, 0, out_array, &count);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(count, 2);

    /* List chunks for node 1 -- expect 2 */
    count = 8;
    rc = vtbl->list_chunks_by_node(ctx, 1, out_array, &count);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(count, 2);

    /* List chunks for node 2 -- expect 1 */
    count = 8;
    rc = vtbl->list_chunks_by_node(ctx, 2, out_array, &count);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(count, 1);

    /* List chunks for node 99 -- expect 0 */
    count = 8;
    rc = vtbl->list_chunks_by_node(ctx, 99, out_array, &count);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(count, 0);

    teardown_meta();
}

/* ------------------------------------------------------------------------
 * Test 9: auto_unregister_on_zero_ref
 * Register, release_ref to 0, chunk auto-removed.
 * ------------------------------------------------------------------------ */
TEST(auto_unregister_on_zero_ref)
{
    setup_meta();

    chunk_id_t cid = 0;
    int rc = vtbl->register_chunk(ctx, "auto_unref", make_gpa(0, UMM_TIER_CXL, 0x6000), 4096, &cid);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_NE(cid, 0);

    /* Verify it exists initially */
    ChunkMetadata meta;
    rc = vtbl->lookup_chunk(ctx, "auto_unref", &meta);
    ASSERT_EQ(rc, UMM_OK);

    /* Release the initial ref count to 0 -- should auto-unregister */
    rc = vtbl->release_ref(ctx, cid);
    ASSERT_EQ(rc, UMM_OK);

    /* Chunk should be gone */
    rc = vtbl->lookup_chunk(ctx, "auto_unref", &meta);
    ASSERT_EQ(rc, UMM_E_NOT_FOUND);

    teardown_meta();
}

/* ------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------ */
TEST_SUITE("Metadata Service v2.0")
{
    RUN_TEST(register_and_lookup);
    RUN_TEST(lookup_not_found);
    RUN_TEST(duplicate_name);
    RUN_TEST(register_multiple);
    RUN_TEST(lookup_by_id);
    RUN_TEST(unregister);
    RUN_TEST(ref_counting);
    RUN_TEST(list_by_node);
    RUN_TEST(auto_unregister_on_zero_ref);
}
END_TEST_SUITE()
