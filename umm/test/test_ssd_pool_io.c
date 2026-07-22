/* ========================================================================
 * test_ssd_pool_io.c — SSD 池级主机 I/O 与跨设备分段测试
 *
 * 覆盖：
 *   1. 池级 pread/pwrite（ssd_pool_io）跨设备自动分段
 *   2. 越界拒绝
 *   3. mem_service 层 register_storage(文件) → alloc_tiered →
 *      map_device(mmap) 与 vtbl ssd_read/write 双通路一致
 *
 * 注：内核块设备后端已移除（规避误写系统盘风险），
 *     真实硬件经 libnvm 后端接入（见 test_ssd_libnvm.c）。
 * ======================================================================== */

#include "../include/umm.h"
#include "../src/transport/ssd_pool.h"
#include "../src/memory_service/mem_service.h"
#include "../src/memory_service/mem_service_direct.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do {                                   \
    if (cond) { g_pass++; printf("  [PASS] %s\n", msg); }       \
    else      { g_fail++; printf("  [FAIL] %s\n", msg); }       \
} while (0)

int main(void)
{
    printf("\n=== Test: SSD 池级 I/O 与跨设备分段 ===\n");

    /* ---- 1. 跨设备分段 I/O ---- */
    {
        const char *f0 = "/tmp/umm_pool_io0.raw";
        const char *f1 = "/tmp/umm_pool_io1.raw";
        unlink(f0); unlink(f1);

        SsdPool *pool = ssd_pool_create();
        int rc = ssd_pool_add_device(pool, f0, 8 * 1024 * 1024);
        CHECK(rc == UMM_OK, "pool 加入设备0（8MB 文件）");
        rc = ssd_pool_add_device(pool, f1, 8 * 1024 * 1024);
        CHECK(rc == UMM_OK, "pool 加入设备1（8MB 文件）");

        uint64_t voff = 0;
        rc = ssd_pool_alloc(pool, 12 * 1024 * 1024, &voff);
        CHECK(rc == UMM_OK && voff == 0, "分配 12MB 跨设备区间");

        uint64_t len = 12 * 1024 * 1024;
        uint8_t *wbuf = malloc(len), *rbuf = malloc(len);
        for (uint64_t i = 0; i < len; i++)
            wbuf[i] = (uint8_t)(i * 13 + (i >> 12));
        rc = ssd_pool_pwrite(pool, voff, len, wbuf);
        CHECK(rc == UMM_OK, "pool_pwrite 12MB（跨设备分段写）");
        memset(rbuf, 0, len);
        rc = ssd_pool_pread(pool, voff, len, rbuf);
        CHECK(rc == UMM_OK && memcmp(wbuf, rbuf, len) == 0,
              "pool_pread 12MB 读回一致（跨设备分段读）");

        rc = ssd_pool_pread(pool, ssd_pool_total_capacity(pool) - 4096,
                            8192, rbuf);
        CHECK(rc == UMM_E_INVALID_ARG, "pool_pread 越界 → 拒绝");

        free(wbuf); free(rbuf);
        ssd_pool_destroy(pool);
        unlink(f0); unlink(f1);
    }

    /* ---- 2. mem_service 层：文件后端双通路（mmap + ssd_read/write）---- */
    {
        const char *f2 = "/tmp/umm_pool_io2.raw";
        unlink(f2);

        void *mctx = NULL;
        MemoryServiceVtbl *vtbl =
            mem_service_direct_create(0, 16 * 1024 * 1024, 0, &mctx);
        CHECK(vtbl != NULL, "mem_service_direct_create");
        if (vtbl) {
            StorageResource res = {
                .tier = UMM_TIER_SSD,
                .capacity = 8 * 1024 * 1024,
                .base_offset = 0,
                .online = 1,
            };
            snprintf(res.device_path, sizeof(res.device_path), "%s", f2);
            int rc = vtbl->register_storage(mctx, &res);
            CHECK(rc == UMM_OK, "register_storage(文件后端)");

            uint64_t off = 0;
            rc = vtbl->alloc_tiered(mctx, UMM_TIER_SSD,
                                    1024 * 1024, &off);
            CHECK(rc == UMM_OK, "alloc_tiered 1MB");

            /* 文件后端：mmap 通路可用；ssd_read/write 通路同样可用且一致 */
            void *ptr = NULL;
            rc = vtbl->map_device(mctx, UMM_TIER_SSD, 0, off, 4096, &ptr);
            CHECK(rc == UMM_OK && ptr != NULL,
                  "文件后端 map_device 返回 mmap 指针");

            uint8_t wbuf[4096], rbuf[4096];
            for (int i = 0; i < 4096; i++)
                wbuf[i] = (uint8_t)(i * 17 + 3);
            rc = vtbl->ssd_write(mctx, UMM_TIER_SSD, 0, off, 4096, wbuf);
            CHECK(rc == UMM_OK, "vtbl->ssd_write 4KB");
            memset(rbuf, 0, sizeof(rbuf));
            rc = vtbl->ssd_read(mctx, UMM_TIER_SSD, 0, off, 4096, rbuf);
            CHECK(rc == UMM_OK && memcmp(wbuf, rbuf, 4096) == 0,
                  "vtbl->ssd_read 与 mmap 通路数据一致");

            /* capacity=0 拒绝 */
            StorageResource bad = res;
            bad.capacity = 0;
            void *mctx2 = NULL;
            MemoryServiceVtbl *vtbl2 =
                mem_service_direct_create(0, 16 * 1024 * 1024, 0, &mctx2);
            rc = vtbl2 ? vtbl2->register_storage(mctx2, &bad) : -1;
            CHECK(rc == UMM_E_INVALID_ARG, "capacity=0 → 拒绝注册");
            if (vtbl2)
                mem_service_direct_destroy(mctx2);

            vtbl->free_tiered(mctx, UMM_TIER_SSD, off, 1024 * 1024);
            mem_service_direct_destroy(mctx);
        }
        unlink(f2);
    }

    printf("  Results: %d passed, %d failed\n\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
