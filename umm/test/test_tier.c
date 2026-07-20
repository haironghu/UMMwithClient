/* ========================================================================
 * test_tier.c -- Multi-Tier Storage System Test (v3.0 - Phase 3)
 *
 * Tests the refactored tier architecture:
 *   - ssd_backend: direct file-backed alloc/get_ptr/free/sync/recover
 *   - transport_ssd: pure I/O executor using map_device from umms
 *   - tier_router: pure routing tier_vtbls[tier] with equal citizen tiers
 *
 * Phase 3 changes:
 *   - transport_ssd no longer manages ssd_backend internally
 *   - transport_ssd uses mem_vtbl->map_device() to obtain pointers
 *   - No chunk priming needed; map_device does bounds checking
 * ======================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "../include/umm.h"
#include "../src/transport/transport.h"
#include "../src/transport/transport_ssd.h"
#include "../src/transport/ssd_pool.h"
#include "../src/transport/tier_router.h"

#include "test_framework.h"

#define TEST_SSD_DIR  "/tmp/umm_test_ssd.raw"
#define TEST_SSD_SIZE (256ULL * 1024 * 1024)  /* 256 MB */

/* ------------------------------------------------------------------------ */
/* Cleanup helper                                                           */
/* ------------------------------------------------------------------------ */
static void cleanup_ssd_dir(void)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -f %s", TEST_SSD_DIR);
    system(cmd);
}

/* ------------------------------------------------------------------------ */
/* Test 1: SSD backend -- basic alloc / get_ptr / free                      */
/* ------------------------------------------------------------------------ */
TEST(ssd_backend_basic)
{
    cleanup_ssd_dir();

    SsdBackend *sb = ssd_backend_create(TEST_SSD_DIR, TEST_SSD_SIZE);
    ASSERT_NOT_NULL(sb);

    uint64_t offset;
    uint64_t size = 4096;
    void *ptr = NULL;

    int rc = ssd_backend_alloc(sb, size, &offset);
    ASSERT_EQ(rc, UMM_OK);
    ptr = ssd_backend_get_ptr(sb, offset);
    ASSERT_NOT_NULL(ptr);

    /* Write and read back */
    const char *msg = "Hello SSD Backend!";
    memcpy(ptr, msg, strlen(msg) + 1);

    char buf[256];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, ptr, strlen(msg) + 1);
    ASSERT_EQ(strcmp(buf, msg), 0);

    /* Free */
    ssd_backend_free(sb, offset, size);

    /* Verify freed -- page can be re-allocated */
    uint64_t offset2;
    rc = ssd_backend_alloc(sb, size, &offset2);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(offset2, offset);  /* same page recycled */

    ssd_backend_destroy(sb);
    cleanup_ssd_dir();
}

/* ------------------------------------------------------------------------ */
/* Test 2: SSD backend -- multiple chunks                                   */
/* ------------------------------------------------------------------------ */
TEST(ssd_backend_multiple)
{
    cleanup_ssd_dir();

    SsdBackend *sb = ssd_backend_create(TEST_SSD_DIR, TEST_SSD_SIZE);
    ASSERT_NOT_NULL(sb);

    /* Create 3 chunks */
    uint64_t offsets[3];
    int rc;
    for (int i = 0; i < 3; i++) {
        uint64_t offset;
        rc = ssd_backend_alloc(sb, 4096, &offset);
        ASSERT_EQ(rc, UMM_OK);
        offsets[i] = offset;
        void *ptr = ssd_backend_get_ptr(sb, offset);
        ASSERT_NOT_NULL(ptr);

        uint64_t val = 0xDEADBEEF00000000ULL + (uint64_t)i;
        memcpy(ptr, &val, sizeof(val));
    }

    /* Verify data */
    for (int i = 0; i < 3; i++) {
        void *ptr = ssd_backend_get_ptr(sb, offsets[i]);
        ASSERT_NOT_NULL(ptr);
        uint64_t val = 0;
        memcpy(&val, ptr, sizeof(val));
        ASSERT_EQ(val, 0xDEADBEEF00000000ULL + (uint64_t)i);
    }

    /* Free middle chunk */
    ssd_backend_free(sb, offsets[1], 4096);

    /* Verify page was recycled -- can re-alloc same offset */
    uint64_t recycled;
    rc = ssd_backend_alloc(sb, 4096, &recycled);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(recycled, offsets[1]);

    /* Other chunks still accessible */
    void *ptr = ssd_backend_get_ptr(sb, offsets[0]);
    ASSERT_NOT_NULL(ptr);
    ptr = ssd_backend_get_ptr(sb, offsets[2]);
    ASSERT_NOT_NULL(ptr);

    ssd_backend_destroy(sb);
    cleanup_ssd_dir();
}

/* ------------------------------------------------------------------------ */
/* Test 3: SSD backend -- sync and recover                                  */
/* ------------------------------------------------------------------------ */
TEST(ssd_backend_sync_recover)
{
    cleanup_ssd_dir();

    /* Phase 1: Create chunks and write data */
    {
        SsdBackend *sb = ssd_backend_create(TEST_SSD_DIR, TEST_SSD_SIZE);
        ASSERT_NOT_NULL(sb);

        for (int i = 0; i < 3; i++) {
            uint64_t offset;
            int rc = ssd_backend_alloc(sb, 4096, &offset);
            ASSERT_EQ(rc, UMM_OK);
            void *ptr = ssd_backend_get_ptr(sb, offset);
            ASSERT_NOT_NULL(ptr);

            uint64_t val = 0xCAFEBABE00000000ULL + (uint64_t)i;
            memcpy(ptr, &val, sizeof(val));

            rc = ssd_backend_sync(sb, offset, 4096);
            ASSERT_EQ(rc, UMM_OK);
        }

        ssd_backend_destroy(sb);  /* Destroy without deleting files */
    }

    /* Phase 2: New instance -- backing file should still exist */
    {
        SsdBackend *sb = ssd_backend_create(TEST_SSD_DIR, TEST_SSD_SIZE);
        ASSERT_NOT_NULL(sb);

        ASSERT_EQ(access(TEST_SSD_DIR, F_OK), 0);  /* file exists */

        ssd_backend_destroy(sb);
    }

    cleanup_ssd_dir();
}

/* ------------------------------------------------------------------------ */
/* Test 4: SSD transport -- vtbl get/put (Phase 3: no chunk priming)        */
/* ------------------------------------------------------------------------ */
TEST(transport_ssd_vtbl)
{
    cleanup_ssd_dir();

    void *ssd_ctx = NULL;
    MemoryTransportVtbl *ssd_vtbl = ssd_transport_create(
        TEST_SSD_DIR, TEST_SSD_SIZE, &ssd_ctx);
    ASSERT_NOT_NULL(ssd_vtbl);
    ASSERT_NOT_NULL(ssd_ctx);

    /* Phase 3: transport_ssd is a pure I/O executor.
     * map_device() performs bounds checking against total_size.
     * No chunk priming via ssd_backend_alloc needed. */
    gpa_t gpa = make_gpa(0, UMM_TIER_SSD, 0x2000);
    const char *msg = "Hello SSD Transport!";
    int rc = ssd_vtbl->put(ssd_ctx, gpa, strlen(msg) + 1, msg);
    ASSERT_EQ(rc, UMM_OK);

    /* Read back via transport vtbl */
    char buf[256];
    memset(buf, 0, sizeof(buf));
    rc = ssd_vtbl->get(ssd_ctx, gpa, strlen(msg) + 1, buf);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(strcmp(buf, msg), 0);

    ssd_transport_destroy(ssd_ctx);
    /* ssd_transport_destroy also frees the vtbl; do NOT free ssd_vtbl here */
    cleanup_ssd_dir();
}

/* ------------------------------------------------------------------------ */
/* Test 5: SSD transport -- atomic operations (Phase 3: no chunk priming)   */
/* ------------------------------------------------------------------------ */
TEST(transport_ssd_atomic)
{
    cleanup_ssd_dir();

    void *ssd_ctx = NULL;
    MemoryTransportVtbl *ssd_vtbl = ssd_transport_create(
        TEST_SSD_DIR, TEST_SSD_SIZE, &ssd_ctx);
    ASSERT_NOT_NULL(ssd_vtbl);

    gpa_t gpa = make_gpa(0, UMM_TIER_SSD, 0x3000);

    /* Atomic set */
    int rc = ssd_vtbl->atomic_set(ssd_ctx, gpa, 42);
    ASSERT_EQ(rc, UMM_OK);

    /* Atomic CAS -- success case */
    uint64_t old = 0;
    rc = ssd_vtbl->atomic_cas(ssd_ctx, gpa, 42, 100, &old);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(old, 42);

    /* Atomic CAS -- failure case */
    rc = ssd_vtbl->atomic_cas(ssd_ctx, gpa, 42, 200, &old);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(old, 100);  /* current value is 100 */

    /* Atomic fetch-add */
    rc = ssd_vtbl->atomic_fetch_add(ssd_ctx, gpa, 5, &old);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(old, 100);  /* returned previous value */

    /* Verify final value = 100 + 5 = 105 */
    uint64_t final_val = 0;
    rc = ssd_vtbl->get(ssd_ctx, gpa, sizeof(final_val), &final_val);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(final_val, 105);

    ssd_transport_destroy(ssd_ctx);
    /* ssd_transport_destroy also frees the vtbl */
    cleanup_ssd_dir();
}

/* ------------------------------------------------------------------------ */
/* Fake transport vtbl for router testing (records calls, no real I/O)      */
/* ------------------------------------------------------------------------ */

typedef struct {
    int         last_op;     /* 0=get, 1=put, 2=cas, 3=fadd */
    gpa_t       last_gpa;
    uint64_t    last_value;
    uint64_t    storage;     /* single 8-byte cell for atomic tests */
    pthread_mutex_t lock;
} FakeTierCtx;

static int fake_get(void *ctx, gpa_t gpa, uint64_t len, void *out_buf)
{
    FakeTierCtx *f = ctx;
    pthread_mutex_lock(&f->lock);
    f->last_op = 0;
    f->last_gpa = gpa;
    if (len >= sizeof(f->storage))
        memcpy(out_buf, &f->storage, sizeof(f->storage));
    pthread_mutex_unlock(&f->lock);
    return UMM_OK;
}

static int fake_put(void *ctx, gpa_t gpa, uint64_t len, const void *buf)
{
    FakeTierCtx *f = ctx;
    pthread_mutex_lock(&f->lock);
    f->last_op = 1;
    f->last_gpa = gpa;
    if (len >= sizeof(f->storage))
        memcpy(&f->storage, buf, sizeof(f->storage));
    pthread_mutex_unlock(&f->lock);
    return UMM_OK;
}

static int fake_cas(void *ctx, gpa_t gpa, uint64_t e, uint64_t d, uint64_t *old)
{
    FakeTierCtx *f = ctx;
    pthread_mutex_lock(&f->lock);
    f->last_op = 2;
    f->last_gpa = gpa;
    uint64_t prev = f->storage;
    if (prev == e) f->storage = d;
    if (old) *old = prev;
    pthread_mutex_unlock(&f->lock);
    return UMM_OK;
}

static int fake_fadd(void *ctx, gpa_t gpa, uint64_t v, uint64_t *r)
{
    FakeTierCtx *f = ctx;
    pthread_mutex_lock(&f->lock);
    f->last_op = 3;
    f->last_gpa = gpa;
    uint64_t prev = f->storage;
    f->storage = prev + v;
    if (r) *r = prev;
    pthread_mutex_unlock(&f->lock);
    return UMM_OK;
}

static void fake_fence(void *ctx)   { (void)ctx; }
static void fake_barrier(void *ctx) { (void)ctx; }
static void fake_quiet(void *ctx)   { (void)ctx; }
static int  fake_reg(void *ctx, node_id_t n, uint64_t b, uint64_t s, const char *d)
{ (void)ctx; (void)n; (void)b; (void)s; (void)d; return UMM_OK; }
static int  fake_init(void *ctx, const UMMConfig *cfg) { (void)ctx; (void)cfg; return UMM_OK; }
static void fake_deinit(void *ctx) { (void)ctx; }

static void fake_vtbl_fill(MemoryTransportVtbl *v)
{
    v->get              = fake_get;
    v->put              = fake_put;
    v->atomic_cas       = fake_cas;
    v->atomic_fetch_add = fake_fadd;
    v->fence            = fake_fence;
    v->barrier_all      = fake_barrier;
    v->quiet            = fake_quiet;
    v->register_node    = fake_reg;
    v->init             = fake_init;
    v->deinit           = fake_deinit;
}

/* ------------------------------------------------------------------------ */
/* Test 6: Tier router -- routes to correct tier based on GPA tier_id       */
/* ------------------------------------------------------------------------ */
TEST(tier_router_routing)
{
    /* Two fake tiers: CXL (tier 1) and SSD (tier 2) */
    MemoryTransportVtbl cxl_vtbl;  fake_vtbl_fill(&cxl_vtbl);
    MemoryTransportVtbl ssd_vtbl;  fake_vtbl_fill(&ssd_vtbl);

    FakeTierCtx cxl_ctx = { .storage = 0, .last_op = -1 };
    FakeTierCtx ssd_ctx = { .storage = 0, .last_op = -1 };
    pthread_mutex_init(&cxl_ctx.lock, NULL);
    pthread_mutex_init(&ssd_ctx.lock, NULL);

    TierRouter *tr = tier_router_create(&cxl_vtbl, &cxl_ctx,
                                         &ssd_vtbl, &ssd_ctx);
    ASSERT_NOT_NULL(tr);

    MemoryTransportVtbl *router_vtbl = tier_router_get_vtbl(tr);
    ASSERT_NOT_NULL(router_vtbl);

    /* Route to CXL tier (UMM_TIER_CXL = 1) */
    gpa_t gpa_cxl = make_gpa(0, UMM_TIER_CXL, 0x1000);
    const char *msg_cxl = "CXL data";
    int rc = router_vtbl->put(tr, gpa_cxl, strlen(msg_cxl) + 1, msg_cxl);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(cxl_ctx.last_op, 1);        /* put */
    ASSERT_EQ(cxl_ctx.last_gpa, gpa_cxl); /* correct GPA */

    /* Route to SSD tier (UMM_TIER_SSD = 2) */
    gpa_t gpa_ssd = make_gpa(0, UMM_TIER_SSD, 0x2000);
    const char *msg_ssd = "SSD data";
    rc = router_vtbl->put(tr, gpa_ssd, strlen(msg_ssd) + 1, msg_ssd);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(ssd_ctx.last_op, 1);        /* put */
    ASSERT_EQ(ssd_ctx.last_gpa, gpa_ssd); /* correct GPA */

    /* CXL should NOT have been touched by the SSD write */
    ASSERT_EQ(cxl_ctx.last_gpa, gpa_cxl); /* unchanged */

    /* Atomic on SSD tier -- first reset storage to known value */
    ssd_ctx.storage = 0;
    uint64_t old = 0;
    rc = router_vtbl->atomic_cas(tr, gpa_ssd, 0, 123, &old);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(ssd_ctx.last_op, 2);        /* CAS */
    ASSERT_EQ(old, 0);                    /* previous value */
    ASSERT_EQ(ssd_ctx.storage, 123);      /* swapped */

    /* Fence -- should not crash (broadcast to all tiers) */
    router_vtbl->fence(tr);
    router_vtbl->barrier_all(tr);
    router_vtbl->quiet(tr);

    tier_router_destroy(tr);
    pthread_mutex_destroy(&cxl_ctx.lock);
    pthread_mutex_destroy(&ssd_ctx.lock);
}

/* ------------------------------------------------------------------------ */
/* Test 7: Tier router -- SSD-only tier (CXL slot NULL)                     */
/* ------------------------------------------------------------------------ */
TEST(tier_router_ssd_only)
{
    MemoryTransportVtbl ssd_vtbl;  fake_vtbl_fill(&ssd_vtbl);
    FakeTierCtx ssd_ctx = { .storage = 0, .last_op = -1 };
    pthread_mutex_init(&ssd_ctx.lock, NULL);

    /* CXL slot is NULL -- router should handle gracefully */
    TierRouter *tr = tier_router_create(NULL, NULL,
                                         &ssd_vtbl, &ssd_ctx);
    ASSERT_NOT_NULL(tr);

    MemoryTransportVtbl *router_vtbl = tier_router_get_vtbl(tr);

    /* SSD route still works */
    gpa_t gpa_ssd = make_gpa(0, UMM_TIER_SSD, 0x4000);
    int rc = router_vtbl->put(tr, gpa_ssd, 8, "SSDonly!");
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(ssd_ctx.last_op, 1);

    /* CXL route returns error (no CXL tier registered) */
    gpa_t gpa_cxl = make_gpa(0, UMM_TIER_CXL, 0x4000);
    rc = router_vtbl->put(tr, gpa_cxl, 8, "FAIL!");
    ASSERT_EQ(rc, UMM_E_INVALID_ARG);

    tier_router_destroy(tr);
    pthread_mutex_destroy(&ssd_ctx.lock);
}

/* ------------------------------------------------------------------------ */
/* Test 8: Tier router + real SSD transport -- end-to-end                   */
/* ------------------------------------------------------------------------ */
TEST(tier_router_real_ssd)
{
    cleanup_ssd_dir();

    /* Create SSD transport */
    void *ssd_ctx = NULL;
    MemoryTransportVtbl *ssd_vtbl = ssd_transport_create(
        TEST_SSD_DIR, TEST_SSD_SIZE, &ssd_ctx);
    ASSERT_NOT_NULL(ssd_vtbl);

    /* Create router: no CXL, real SSD */
    TierRouter *tr = tier_router_create(NULL, NULL, ssd_vtbl, ssd_ctx);
    ASSERT_NOT_NULL(tr);

    MemoryTransportVtbl *rv = tier_router_get_vtbl(tr);

    /* Write through router */
    gpa_t gpa = make_gpa(0, UMM_TIER_SSD, 0x5000);
    const char *msg = "End-to-end SSD via router!";
    int rc = rv->put(tr, gpa, strlen(msg) + 1, msg);
    ASSERT_EQ(rc, UMM_OK);

    /* Read back */
    char buf[256];
    memset(buf, 0, sizeof(buf));
    rc = rv->get(tr, gpa, strlen(msg) + 1, buf);
    ASSERT_EQ(rc, UMM_OK);
    ASSERT_EQ(strcmp(buf, msg), 0);

    tier_router_destroy(tr);
    ssd_transport_destroy(ssd_ctx);
    /* ssd_transport_destroy also frees the vtbl */
    cleanup_ssd_dir();
}

/* ------------------------------------------------------------------------ */
/* main                                                                     */
/* ------------------------------------------------------------------------ */
TEST_SUITE("Multi-Tier Storage v3.0")
    RUN_TEST(ssd_backend_basic);
    RUN_TEST(ssd_backend_multiple);
    RUN_TEST(ssd_backend_sync_recover);
    RUN_TEST(transport_ssd_vtbl);
    RUN_TEST(transport_ssd_atomic);
    RUN_TEST(tier_router_routing);
    RUN_TEST(tier_router_ssd_only);
    RUN_TEST(tier_router_real_ssd);
END_TEST_SUITE()
