/* Device allocation invariants, using temporary file backends only. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../include/umm.h"
#include "../src/transport/ssd_pool.h"
#include "../src/protocol/mem_protocol.h"
#include "../src/metadata_service/meta_service_direct.h"

#define PAGE 4096ULL
#define CAP (64 * PAGE)
static char dir[] = "/tmp/umm_device_test_XXXXXX";
static char paths[2][256];
static SsdPool *pool;
static uint64_t offsets[32];
static void *allocate_thread(void *arg)
{
    size_t i = (size_t)arg;
    assert(ssd_pool_alloc_on_device(pool, 1, PAGE, &offsets[i]) == UMM_OK);
    return NULL;
}

static void pool_checks(void)
{
    pool = ssd_pool_create();
    assert(pool);
    for (int d = 0; d < 2; d++)
        assert(ssd_pool_add_device(pool, paths[d], CAP) == UMM_OK);
    uint64_t off, other, total, free_bytes;
    assert(ssd_pool_alloc_on_device(pool, 2, PAGE, &off) == UMM_E_INVALID_ARG);
    assert(ssd_pool_alloc_on_device(pool, 0, UINT64_MAX, &off) == UMM_E_INVALID_ARG);
    assert(ssd_pool_alloc_on_device(pool, 0, 0, &off) == UMM_E_INVALID_ARG);
    assert(ssd_pool_alloc_on_device(pool, 1, CAP, &off) == UMM_OK);
    assert(off == CAP);
    uint32_t dev;
    assert(ssd_pool_translate(pool, off + CAP - 1, &dev, NULL) == UMM_OK && dev == 1);
    assert(ssd_pool_alloc_on_device(pool, 1, 1, &other) == UMM_E_NO_MEMORY);
    assert(ssd_pool_alloc(pool, PAGE, &other) == UMM_OK && other == 0);
    assert(ssd_pool_free(pool, off, CAP) == UMM_OK);
    assert(ssd_pool_free(pool, other, PAGE) == UMM_OK);

    pthread_t threads[32];
    for (size_t i = 0; i < 32; i++)
        assert(pthread_create(&threads[i], NULL, allocate_thread, (void *)i) == 0);
    for (int i = 0; i < 32; i++) assert(pthread_join(threads[i], NULL) == 0);
    for (int i = 0; i < 32; i++) {
        assert(offsets[i] >= CAP && offsets[i] < 2 * CAP);
        for (int j = 0; j < i; j++) assert(offsets[i] != offsets[j]);
    }
    assert(ssd_pool_alloc(pool, CAP, &off) == UMM_OK && off == 0);
    assert(ssd_pool_free(pool, off, CAP) == UMM_OK);
    for (int i = 0; i < 32; i++) assert(ssd_pool_free(pool, offsets[i], PAGE) == UMM_OK);

    /* Fragmented target must fail despite enough aggregate free pages. */
    uint64_t slots[64];
    for (int i = 0; i < 64; i++)
        assert(ssd_pool_alloc_on_device(pool, 1, PAGE, &slots[i]) == UMM_OK);
    for (int i = 0; i < 64; i += 2) assert(ssd_pool_free(pool, slots[i], PAGE) == UMM_OK);
    assert(ssd_pool_alloc_on_device(pool, 1, 2 * PAGE, &off) == UMM_E_NO_MEMORY);
    for (int i = 1; i < 64; i += 2) assert(ssd_pool_free(pool, slots[i], PAGE) == UMM_OK);
    ssd_pool_get_usage(pool, &total, &free_bytes);
    assert(total == 2 * CAP && free_bytes == total);
    ssd_pool_destroy(pool);
}

/* Inject a metadata registration failure after the SSD reservation. */
static int fail_registration;
static MetadataServiceVtbl *real_meta;
static int register_with_failure(void *ctx, const char *name, gpa_t gpa,
                                  uint64_t size, chunk_id_t *out)
{
    if (fail_registration) {
        fail_registration = 0;
        return UMM_E_NO_MEMORY;
    }
    return real_meta->register_chunk(ctx, name, gpa, size, out);
}
MetadataServiceVtbl *__real_meta_service_direct_create(void **out_ctx);
MetadataServiceVtbl *__wrap_meta_service_direct_create(void **out_ctx)
{
    static MetadataServiceVtbl wrapper;
    real_meta = __real_meta_service_direct_create(out_ctx);
    if (!real_meta) return NULL;
    wrapper = *real_meta;
    wrapper.register_chunk = register_with_failure;
    return &wrapper;
}

static void api_checks(void)
{
    UMMConfig cfg = {0};
    cfg.memory_size = 1024 * 1024;
    cfg.my_node_id = 3;
    cfg.ssd_owner_node = UMM_NODE_UNKNOWN;
    assert(umm_init(&cfg) == UMM_OK);
    for (int d = 0; d < 2; d++)
        assert(umm_register_storage_tier(UMM_TIER_SSD, paths[d], CAP) == UMM_OK);
    StorageTopology topo = {0};
    assert(umm_get_topology(&topo) == UMM_OK);
    int nssd = 0;
    for (uint32_t i = 0; i < topo.num_resources; i++) {
        StorageResource *r = &topo.resources[i];
        if (r->tier == UMM_TIER_SSD) {
            assert(!strcmp(r->device_path, paths[nssd]));
            assert(r->base_offset == (uint64_t)nssd * CAP);
            nssd++;
        }
    }
    assert(nssd == 2);
    ChunkDescriptor desc;
    fail_registration = 1;
    assert(umm_alloc_on_device(CAP, UMM_TIER_SSD, 1, &desc) == UMM_E_NO_MEMORY);
    assert(desc.chunk_id == 0 && desc.base_gpa == 0 && desc.user_size == 0);
    /* The whole device must be available again after registration rollback. */
    assert(umm_alloc_on_device(CAP, UMM_TIER_SSD, 1, &desc) == UMM_OK);
    assert(umm_free(&desc) == UMM_OK);
    assert(umm_alloc_on_device(PAGE, UMM_TIER_CXL, 0, &desc) == UMM_E_UNSUPPORTED);
    assert(umm_alloc_on_device(UINT64_MAX, UMM_TIER_SSD, 0, &desc) == UMM_E_INVALID_ARG);
    assert(umm_alloc_on_device(1, UMM_TIER_SSD, 1, &desc) == UMM_OK);
    assert(desc.user_size == PAGE);
    assert(gpa_to_node(desc.base_gpa) == 3 && gpa_to_offset(desc.base_gpa) == CAP);
    char data[PAGE], back[PAGE];
    memset(data, 0x5a, sizeof(data));
    assert(umm_write(&desc, 0, PAGE, data) == UMM_OK);
    assert(umm_read(&desc, 0, PAGE, back) == UMM_OK);
    assert(!memcmp(data, back, PAGE));
    /* Verify the physical file, not merely a symmetric read/write mapping. */
    FILE *f = fopen(paths[1], "rb");
    assert(f && fread(back, 1, PAGE, f) == PAGE);
    fclose(f);
    assert(!memcmp(data, back, PAGE));
    assert(umm_free(&desc) == UMM_OK);
    assert(umm_alloc_on_device(CAP, UMM_TIER_SSD, 1, &desc) == UMM_OK);
    ChunkDescriptor extra;
    assert(umm_alloc_on_device(PAGE, UMM_TIER_SSD, 1, &extra) == UMM_E_NO_MEMORY);
    assert(umm_alloc_tiered(PAGE, UMM_TIER_SSD, &extra) == UMM_OK);
    assert(gpa_to_offset(extra.base_gpa) < CAP);
    assert(umm_free(&extra) == UMM_OK);
    assert(umm_free(&desc) == UMM_OK);
    umm_deinit();
}

static void protocol_checks(void)
{
    UmmProtoBody body;
    MemAllocOnDeviceReq req;
    assert(mem_pack_alloc_on_device(UMM_TIER_SSD, 15, CAP, 0, &body) == UMM_OK);
    assert(mem_unpack_alloc_on_device(&body, &req) == UMM_OK);
    assert(req.tier == UMM_TIER_SSD && req.device_idx == 15 && req.size == CAP);
    body.len--;
    assert(mem_unpack_alloc_on_device(&body, &req) == UMM_E_INVALID_ARG);
    assert(MEM_OP_ALLOC_ON_DEVICE != MEM_OP_DATA_READ);
    assert(MEM_OP_ALLOC_ON_DEVICE != MEM_OP_DATA_WRITE);
}

int main(void)
{
    assert(mkdtemp(dir));
    for (int i = 0; i < 2; i++) snprintf(paths[i], sizeof(paths[i]), "%s/d%d.raw", dir, i);
    pool_checks();
    api_checks();
    protocol_checks();
    for (int i = 0; i < 2; i++) unlink(paths[i]);
    rmdir(dir);
    puts("Device allocation: pool, concurrency, direct API, placement and protocol PASS");
    return 0;
}
