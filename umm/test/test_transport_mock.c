/* ========================================================================
 * test_transport_mock.c -- Unit tests for the local transport layer
 * ======================================================================== */

#include "test_framework.h"
#include "transport/transport.h"
#include "common/types.h"
#include "common/error_codes.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Local transport vtable accessor (defined in transport_local.c) */
extern const MemoryTransportVtbl *umm_local_vtbl_get(void);

/* MockTransportCtx is opaque; we allocate a buffer large enough for it.
 * On Linux x86_64 the struct is ~64 bytes; 256 bytes gives ample margin. */
#define MOCK_CTX_SIZE 256

static const MemoryTransportVtbl *vtbl = NULL;
static uint8_t ctx_buf[MOCK_CTX_SIZE];
static void *ctx = NULL;

/* ------------------------------------------------------------------------ */
/* Helper: initialize a fresh mock transport context                        */
/* ------------------------------------------------------------------------ */
static void setup_mock(uint32_t node_count, uint64_t node_size, node_id_t my_node)
{
    vtbl = umm_local_vtbl_get();
    ASSERT_NOT_NULL((void *)vtbl);

    ctx = ctx_buf;
    memset(ctx, 0, MOCK_CTX_SIZE);

    (void)node_count;  /* mock transport hardcodes 2 nodes */
    UMMConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.memory_size = node_size;
    cfg.my_node_id  = my_node;

    int rc = vtbl->init(ctx, &cfg);
    ASSERT_EQ(rc, UMM_OK);
}

static void teardown_mock(void)
{
    if (vtbl && ctx) {
        vtbl->deinit(ctx);
        ctx = NULL;
        vtbl = NULL;
    }
}

/* ------------------------------------------------------------------------ */
/* Test 1: transport_init_deinit -- init with 2 nodes, 1 MB each, deinit    */
/* ------------------------------------------------------------------------ */
TEST(transport_init_deinit)
{
    setup_mock(2, 1024ULL * 1024, 0);
    teardown_mock();
}

/* ------------------------------------------------------------------------ */
/* Test 2: put_and_get -- write data then read it back                      */
/* ------------------------------------------------------------------------ */
TEST(put_and_get)
{
    setup_mock(2, 1024ULL * 1024, 0);

    const char *write_data = "Hello, UMM Mock Transport!";
    size_t len = strlen(write_data) + 1;
    gpa_t gpa = make_gpa(0, UMM_TIER_CXL, 0x1000); /* node 0, tier=CXL, offset 0x1000 */

    int rc = vtbl->put(ctx, gpa, len, write_data);
    ASSERT_EQ(rc, UMM_OK);

    char read_buf[64] = {0};
    rc = vtbl->get(ctx, gpa, len, read_buf);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(strcmp(read_buf, write_data), 0);

    teardown_mock();
}

/* ------------------------------------------------------------------------ */
/* Test 3: put_get_offset -- put at specific offset, get from same offset   */
/* ------------------------------------------------------------------------ */
TEST(put_get_offset)
{
    setup_mock(2, 1024ULL * 1024, 0);

    uint64_t test_offset = 0x2000;
    uint64_t test_value  = 0xDEADBEEFCAFEBABEULL;
    gpa_t gpa = make_gpa(0, UMM_TIER_CXL, test_offset);

    int rc = vtbl->put(ctx, gpa, sizeof(uint64_t), &test_value);
    ASSERT_EQ(rc, UMM_OK);

    uint64_t read_value = 0;
    rc = vtbl->get(ctx, gpa, sizeof(uint64_t), &read_value);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(read_value, test_value);

    teardown_mock();
}

/* ------------------------------------------------------------------------ */
/* Test 4: atomic_cas_success -- CAS when expected value matches            */
/* ------------------------------------------------------------------------ */
TEST(atomic_cas_success)
{
    setup_mock(2, 1024ULL * 1024, 0);

    gpa_t gpa = make_gpa(0, UMM_TIER_CXL, 0x3000);
    uint64_t initial = 42;
    uint64_t expected = 42;
    uint64_t desired = 100;
    uint64_t old = 0;

    /* Pre-set the value */
    int rc = vtbl->atomic_set(ctx, gpa, initial);
    ASSERT_EQ(rc, UMM_OK);

    /* CAS should succeed */
    rc = vtbl->atomic_cas(ctx, gpa, expected, desired, &old);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(old, initial);

    /* Verify the new value */
    uint64_t current = 0;
    rc = vtbl->get(ctx, gpa, sizeof(uint64_t), &current);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(current, desired);

    teardown_mock();
}

/* ------------------------------------------------------------------------ */
/* Test 5: atomic_cas_fail -- CAS when expected value does not match        */
/* ------------------------------------------------------------------------ */
TEST(atomic_cas_fail)
{
    setup_mock(2, 1024ULL * 1024, 0);

    gpa_t gpa = make_gpa(0, UMM_TIER_CXL, 0x4000);
    uint64_t initial = 42;
    uint64_t expected = 99; /* wrong expected */
    uint64_t desired = 100;
    uint64_t old = 0;

    /* Pre-set the value */
    int rc = vtbl->atomic_set(ctx, gpa, initial);
    ASSERT_EQ(rc, UMM_OK);

    /* CAS should report success (operation completed) but not swap */
    rc = vtbl->atomic_cas(ctx, gpa, expected, desired, &old);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(old, initial); /* old value is still 42 */

    /* Verify the value was NOT changed */
    uint64_t current = 0;
    rc = vtbl->get(ctx, gpa, sizeof(uint64_t), &current);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(current, initial);

    teardown_mock();
}

/* ------------------------------------------------------------------------ */
/* Test 6: atomic_fetch_add -- fetch and add                                */
/* ------------------------------------------------------------------------ */
TEST(atomic_fetch_add)
{
    setup_mock(2, 1024ULL * 1024, 0);

    gpa_t gpa = make_gpa(0, UMM_TIER_CXL, 0x5000);
    uint64_t initial = 10;
    uint64_t result = 0;

    /* Pre-set the value */
    int rc = vtbl->atomic_set(ctx, gpa, initial);
    ASSERT_EQ(rc, UMM_OK);

    /* Fetch and add 5 */
    rc = vtbl->atomic_fetch_add(ctx, gpa, 5, &result);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(result, 10); /* old value */

    /* Verify the new value */
    uint64_t current = 0;
    rc = vtbl->get(ctx, gpa, sizeof(uint64_t), &current);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(current, 15);

    teardown_mock();
}

/* ------------------------------------------------------------------------ */
/* Test 7: atomic_set -- atomic set                                         */
/* ------------------------------------------------------------------------ */
TEST(atomic_set)
{
    setup_mock(2, 1024ULL * 1024, 0);

    gpa_t gpa = make_gpa(0, UMM_TIER_CXL, 0x6000);

    int rc = vtbl->atomic_set(ctx, gpa, 0x123456789ABCDEF0ULL);
    ASSERT_EQ(rc, UMM_OK);

    uint64_t current = 0;
    rc = vtbl->get(ctx, gpa, sizeof(uint64_t), &current);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(current, 0x123456789ABCDEF0ULL);

    teardown_mock();
}

/* ------------------------------------------------------------------------ */
/* Test 8: fence -- fence does not crash                                    */
/* ------------------------------------------------------------------------ */
TEST(fence)
{
    setup_mock(2, 1024ULL * 1024, 0);

    /* Fence should execute without crashing */
    vtbl->fence(ctx);

    teardown_mock();
}

/* ------------------------------------------------------------------------ */
/* Test 9: cross_node -- put on node 0, get on node 0 (mock is single proc) */
/* ------------------------------------------------------------------------ */
TEST(cross_node)
{
    /* Mock transport allocates buffers for all nodes in the same process */
    setup_mock(2, 1024ULL * 1024, 0);

    /* Write to node 0 */
    gpa_t gpa0 = make_gpa(0, UMM_TIER_CXL, 0x7000);
    uint64_t val0 = 0x11111111;
    int rc = vtbl->put(ctx, gpa0, sizeof(uint64_t), &val0);
    ASSERT_EQ(rc, UMM_OK);

    /* Read back from node 0 */
    uint64_t read0 = 0;
    rc = vtbl->get(ctx, gpa0, sizeof(uint64_t), &read0);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(read0, val0);

    /* Write to node 1 */
    gpa_t gpa1 = make_gpa(1, UMM_TIER_CXL, 0x8000);
    uint64_t val1 = 0x22222222;
    rc = vtbl->put(ctx, gpa1, sizeof(uint64_t), &val1);
    ASSERT_EQ(rc, UMM_OK);

    /* Read back from node 1 */
    uint64_t read1 = 0;
    rc = vtbl->get(ctx, gpa1, sizeof(uint64_t), &read1);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(read1, val1);

    teardown_mock();
}

/* ------------------------------------------------------------------------ */
/* Test 10: barrier_quiet -- barrier and quiet do not crash                 */
/* ------------------------------------------------------------------------ */
TEST(barrier_quiet)
{
    setup_mock(2, 1024ULL * 1024, 0);

    /* These are no-ops in the mock transport; just ensure they don't crash */
    vtbl->barrier_all(ctx);
    vtbl->quiet(ctx);

    teardown_mock();
}

/* ------------------------------------------------------------------------ */
/* Main                                                                     */
/* ------------------------------------------------------------------------ */
TEST_SUITE("Mock Transport")
{
    RUN_TEST(transport_init_deinit);
    RUN_TEST(put_and_get);
    RUN_TEST(put_get_offset);
    RUN_TEST(atomic_cas_success);
    RUN_TEST(atomic_cas_fail);
    RUN_TEST(atomic_fetch_add);
    RUN_TEST(atomic_set);
    RUN_TEST(fence);
    RUN_TEST(cross_node);
    RUN_TEST(barrier_quiet);
}
END_TEST_SUITE()