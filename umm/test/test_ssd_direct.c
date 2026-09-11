#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../src/transport/ssd_pool.h"

/* Window guards, aligned and bounced buffers, stats, EOF/unaligned/bounds.
 * Run on a real filesystem that supports O_DIRECT; no hardware required. */
int main(void)
{
    char path[] = "/tmp/umm_direct_XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    char fill[4096]; memset(fill, 0x5a, sizeof(fill));
    for (int i = 0; i < 4; i++) assert(write(fd, fill, sizeof(fill)) == sizeof(fill));
    assert(fsync(fd) == 0);
    close(fd);
    char spec[256]; snprintf(spec, sizeof(spec), "direct:4096:%s", path);
    SsdPool *pool = ssd_pool_create(); assert(pool);
    assert(ssd_pool_add_device(pool, spec, 8192) == UMM_OK);
    assert(ssd_pool_get_ptr(pool, 0) == NULL); /* Must NEVER use mmap. */
    uint64_t off;
    assert(ssd_pool_alloc_on_device(pool, 0, 8192, &off) == UMM_OK && off == 0);
    SsdDirectStats stats;
    assert(ssd_pool_direct_stats(pool, 0, &stats, 1) == UMM_OK);
    void *aligned; assert(posix_memalign(&aligned, stats.alignment, 8192) == 0);
    memset(aligned, 0x39, 8192);
    assert(ssd_pool_pwrite(pool, 0, 8192, aligned) == UMM_OK);
    assert(ssd_pool_sync(pool, 0, 8192) == UMM_OK);
    memset(aligned, 0, 8192);
    assert(ssd_pool_pread(pool, 0, 8192, aligned) == UMM_OK);
    for (int i = 0; i < 8192; i++) assert(((unsigned char *)aligned)[i] == 0x39);
    assert(ssd_pool_direct_stats(pool, 0, &stats, 1) == UMM_OK);
    assert(stats.read_calls == 1 && stats.write_calls == 1 && stats.bounce_bytes == 0);
    assert(stats.read_bytes == 8192 && stats.write_bytes == 8192 && stats.window_base == 4096);
    char *unaligned = (char *)aligned + 1;
    memset(unaligned, 0x27, 4096);
    assert(ssd_pool_pwrite(pool, 0, 4096, unaligned) == UMM_OK);
    memset(unaligned, 0, 4096);
    assert(ssd_pool_pread(pool, 0, 4096, unaligned) == UMM_OK);
    for (int i = 0; i < 4096; i++) assert((unsigned char)unaligned[i] == 0x27);
    assert(ssd_pool_direct_stats(pool, 0, &stats, 0) == UMM_OK && stats.bounce_bytes == 8192);
    assert(ssd_pool_pwrite(pool, 1, 4096, aligned) == UMM_E_INVALID_ARG);
    assert(ssd_pool_pread(pool, 0, 1, aligned) == UMM_E_INVALID_ARG);
    assert(ssd_pool_pread(pool, 8192, 4096, aligned) == UMM_E_INVALID_ARG);
    assert(ssd_pool_sync(pool, 0, 8192) == UMM_OK);
    ssd_pool_destroy(pool);
    free(aligned);
    fd = open(path, O_RDONLY); assert(fd >= 0);
    assert(pread(fd, fill, 4096, 0) == 4096);
    for (int i = 0; i < 4096; i++) assert(fill[i] == 0x5a);
    assert(pread(fd, fill, 4096, 12288) == 4096);
    for (int i = 0; i < 4096; i++) assert(fill[i] == 0x5a);
    close(fd);
    /* Exercise the normal UMM API too: allocation pool and transport both
     * recognize direct: paths, and unaligned host buffers are supported. */
    UMMConfig cfg = {0}; cfg.memory_size = 1024 * 1024;
    assert(umm_init(&cfg) == UMM_OK);
    assert(umm_register_storage_tier(UMM_TIER_SSD, spec, 8192) == UMM_OK);
    ChunkDescriptor desc;
    assert(umm_alloc_on_device(8192, UMM_TIER_SSD, 0, &desc) == UMM_OK);
    memset(fill, 0x71, sizeof(fill));
    assert(umm_write(&desc, 0, sizeof(fill), fill) == UMM_OK);
    umm_fence();
    memset(fill, 0, sizeof(fill));
    assert(umm_read(&desc, 0, sizeof(fill), fill) == UMM_OK);
    for (int i = 0; i < 4096; i++) assert(fill[i] == 0x71);
    assert(umm_free(&desc) == UMM_OK);
    umm_deinit();

    SsdDirect *bad = NULL;
    assert(ssd_direct_open("-1:/tmp/not-used", 4096, &bad) == UMM_E_INVALID_ARG);
    snprintf(spec, sizeof(spec), "1:%s", path);
    assert(ssd_direct_open(spec, 4096, &bad) == UMM_E_INVALID_ARG);
    snprintf(spec, sizeof(spec), "4096:%s", path);
    assert(ssd_direct_open(spec, 16384, &bad) == UMM_E_INVALID_ARG);
    /* Truncation after open must give a real EOF error, never zero-filled success. */
    assert(ssd_direct_open(spec, 8192, &bad) == UMM_OK);
    assert(truncate(path, 4096) == 0);
    assert(ssd_direct_read(bad, 0, 4096, fill) == UMM_E_IO);
    ssd_direct_close(bad);
    unlink(path);
    puts("direct I/O: window guards, mmap bypass, alignment, counters, bounds and EOF PASS");
    return 0;
}
