/* ========================================================================
 * test_ssd_libnvm.c — libnvm 后端接入测试
 *
 * 用 test/stub_libnvm_host.c 编出的桩库（文件模拟控制器）验证：
 *   1. dlopen + 符号解析 + nvm_host_init 通路
 *   2. backend_create("libnvm:...") 分派、容量、get_ptr==NULL
 *   3. 分配 + pwrite/pread 数据一致性（含 >max_io 的分段读写）
 *   4. 池级多后端混合（libnvm + 文件）下的跨后端分段 I/O
 *
 * 环境变量 UMM_LIBNVM_PATH 指向桩库；未设置时自动用默认路径查找。
 *
 * 真机运行（UMM_TEST_LIBNVM_CTRL/UMM_TEST_LIBNVM_OFF，见 main 注释）：
 *   用例 2/3 由测试在 pwrite/pread 偏移上手动加 base_off；
 *   用例 4/5 通过 spec "+<base_off>" 窗口后缀由后端统一加基址——
 *   两种方式落盘范围均限制在 [base_off, base_off+16MB)，不触碰 LBA0。
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
    printf("\n=== Test: SSD libnvm 后端 ===\n");

    /* 桩库路径：默认与测试二进制同目录的 libnvm_host.so */
    if (!getenv("UMM_LIBNVM_PATH"))
        setenv("UMM_LIBNVM_PATH", "./libnvm_host.so", 0);
    unlink("/tmp/libnvm_stub_disk.raw");

    /* 真机运行控制：
     *   UMM_TEST_LIBNVM_CTRL  控制器路径（默认桩用 fake 路径）
     *   UMM_TEST_LIBNVM_OFF   写测试的基址偏移（避开 LBA0 区域，默认 0）
     *   UMM_TEST_LIBNVM_INITONLY=1  只做 init/free 冒烟，零写入风险 */
    const char *ctrl = getenv("UMM_TEST_LIBNVM_CTRL");
    if (!ctrl)
        ctrl = "libnvm:/dev/fake_ctrl0@1";
    uint64_t base_off = 0;
    const char *off_env = getenv("UMM_TEST_LIBNVM_OFF");
    if (off_env)
        base_off = strtoull(off_env, NULL, 0);
    int init_only = getenv("UMM_TEST_LIBNVM_INITONLY") != NULL;

    /* 真机带基址偏移时，容量需覆盖 base_off + 测试用量 */
    uint64_t cap = 16 * 1024 * 1024;
    if (base_off)
        cap = base_off + 16 * 1024 * 1024;

    /* ---- 1. 创建 libnvm 后端 ---- */
    SsdBackend *sb = ssd_backend_create(ctrl, cap);
    CHECK(sb != NULL, "backend_create(libnvm:) 分派成功");
    if (!sb) {
        printf("  [SKIP] 桩库不可用，后续用例跳过\n");
        printf("  Results: %d passed, %d failed\n\n", g_pass, g_fail);
        return g_fail ? 1 : 0;
    }
    CHECK(ssd_backend_capacity(sb) == cap,
          "容量 = 配置值（disk_info 无容量字段）");
    CHECK(ssd_backend_get_ptr(sb, 0) == NULL,
          "libnvm 后端 get_ptr 返回 NULL（无 mmap）");

    if (init_only) {
        printf("  [INFO] INITONLY 模式：init/free 冒烟通过，未做任何写入\n");
        ssd_backend_destroy(sb);
        printf("  Results: %d passed, %d failed (init-only)\n\n",
               g_pass, g_fail);
        return g_fail ? 1 : 0;
    }
    if (base_off)
        printf("  [INFO] 写测试基址偏移: %lu (0x%lx)\n",
               (unsigned long)base_off, (unsigned long)base_off);

    /* ---- 2. 分配 + 小 IO 读写回环 ---- */
    uint64_t off = 0;
    int rc = ssd_backend_alloc(sb, 4 * 1024 * 1024, &off);
    CHECK(rc == UMM_OK && off == 0, "分配 4MB");

    uint8_t wbuf[8192], rbuf[8192];
    for (int i = 0; i < 8192; i++)
        wbuf[i] = (uint8_t)(i * 7 + 11);
    rc = ssd_backend_pwrite(sb, base_off + off, 8192, wbuf);
    CHECK(rc == UMM_OK, "pwrite 8KB");
    memset(rbuf, 0, sizeof(rbuf));
    rc = ssd_backend_pread(sb, base_off + off, 8192, rbuf);
    CHECK(rc == UMM_OK && memcmp(wbuf, rbuf, 8192) == 0,
          "pread 读回一致");

    /* ---- 3. 大 IO（> max_data_size，触发内部分段）---- */
    uint64_t big = 512 * 1024;
    uint64_t mio = ssd_backend_max_io(sb);
    char segmsg[128];
    snprintf(segmsg, sizeof(segmsg), "pwrite 512KB（%lu 段 x %luKB）",
             (unsigned long)((big + mio - 1) / mio),
             (unsigned long)(mio / 1024));
    uint8_t *wb = malloc(big), *rb = malloc(big);
    for (uint64_t i = 0; i < big; i++)
        wb[i] = (uint8_t)(i * 13 + (i >> 16));
    rc = ssd_backend_pwrite(sb, base_off + off, big, wb);
    CHECK(rc == UMM_OK, segmsg);
    memset(rb, 0, big);
    rc = ssd_backend_pread(sb, base_off + off, big, rb);
    CHECK(rc == UMM_OK && memcmp(wb, rb, big) == 0,
          "pread 512KB 分段读回一致");
    free(wb); free(rb);
    ssd_backend_destroy(sb);

    /* ---- 4. 池级混合后端：libnvm + 文件，跨后端分段 ----
     * 真机：spec 带 "+base_off" 窗口后缀，池内 I/O 落盘于
     * [base_off, base_off+8MB)，不触碰 LBA0 区域 */
    char spec4[320];
    if (base_off)
        snprintf(spec4, sizeof(spec4), "%s+%lu",
                 ctrl, (unsigned long)base_off);
    else
        snprintf(spec4, sizeof(spec4), "%s", ctrl);
    const char *f1 = "/tmp/umm_libnvm_mix1.raw";
    unlink(f1);
    SsdPool *pool = ssd_pool_create();
    rc = ssd_pool_add_device(pool, spec4, 8 * 1024 * 1024);
    CHECK(rc == UMM_OK, "pool 加入 libnvm 设备（8MB）");
    rc = ssd_pool_add_device(pool, f1, 8 * 1024 * 1024);
    CHECK(rc == UMM_OK, "pool 加入文件设备（8MB）");

    uint64_t voff = 0;
    rc = ssd_pool_alloc(pool, 12 * 1024 * 1024, &voff);
    CHECK(rc == UMM_OK && voff == 0, "分配 12MB 跨 libnvm+文件 后端");

    uint64_t len = 12 * 1024 * 1024;
    uint8_t *w2 = malloc(len), *r2 = malloc(len);
    for (uint64_t i = 0; i < len; i++)
        w2[i] = (uint8_t)(i * 29 + (i >> 10));
    rc = ssd_pool_pwrite(pool, voff, len, w2);
    CHECK(rc == UMM_OK, "pool_pwrite 跨后端分段写");
    memset(r2, 0, len);
    rc = ssd_pool_pread(pool, voff, len, r2);
    CHECK(rc == UMM_OK && memcmp(w2, r2, len) == 0,
          "pool_pread 跨后端分段读回一致");

    free(w2); free(r2);
    ssd_pool_destroy(pool);
    unlink(f1);

    /* ---- 5. mem_service 层：register_storage("libnvm:...") 全链路 ----
     * 验证 umms 配置路径（libnvm: 前缀不再被误解析为目录追加 pool.raw） */
    {
        void *mctx = NULL;
        MemoryServiceVtbl *vtbl =
            mem_service_direct_create(0, 16 * 1024 * 1024, 0, &mctx);
        CHECK(vtbl != NULL, "mem_service_direct_create");
        if (vtbl) {
            StorageResource res = {
                .tier = UMM_TIER_SSD,
                .capacity = 16 * 1024 * 1024,
                .base_offset = 0,
                .online = 1,
            };
            /* 真机同样带窗口基址，写入限于 [base_off, base_off+16MB) */
            if (base_off)
                snprintf(res.device_path, sizeof(res.device_path),
                         "%s+%lu", ctrl, (unsigned long)base_off);
            else
                snprintf(res.device_path, sizeof(res.device_path),
                         "%s", ctrl);
            int rc = vtbl->register_storage(mctx, &res);
            CHECK(rc == UMM_OK,
                  "register_storage(libnvm:) 路径正确解析");

            uint64_t total = 0, used = 0, freeb = 0;
            rc = vtbl->get_tier_stats(mctx, UMM_TIER_SSD,
                                      &total, &used, &freeb);
            CHECK(rc == UMM_OK && total == 16 * 1024 * 1024,
                  "tier 容量 = 配置值");

            uint64_t off = 0;
            rc = vtbl->alloc_tiered(mctx, UMM_TIER_SSD,
                                    1024 * 1024, &off);
            CHECK(rc == UMM_OK, "alloc_tiered 1MB");

            /* map_device 失败（无 mmap）→ vtbl->ssd_write/read 接管 */
            void *ptr = NULL;
            rc = vtbl->map_device(mctx, UMM_TIER_SSD, 0, off, 4096, &ptr);
            CHECK(rc != UMM_OK, "map_device 对 libnvm 失败（预期）");

            uint8_t wbuf[4096], rbuf[4096];
            for (int i = 0; i < 4096; i++)
                wbuf[i] = (uint8_t)(i * 41 + 5);
            rc = vtbl->ssd_write(mctx, UMM_TIER_SSD, 0, off, 4096, wbuf);
            CHECK(rc == UMM_OK, "vtbl->ssd_write → libnvm 4KB");
            memset(rbuf, 0, sizeof(rbuf));
            rc = vtbl->ssd_read(mctx, UMM_TIER_SSD, 0, off, 4096, rbuf);
            CHECK(rc == UMM_OK && memcmp(wbuf, rbuf, 4096) == 0,
                  "vtbl->ssd_read → libnvm 读回一致");

            /* capacity=0 必须被拒绝（libnvm 无法探测容量） */
            StorageResource bad = res;
            bad.capacity = 0;
            void *mctx2 = NULL;
            MemoryServiceVtbl *vtbl2 =
                mem_service_direct_create(0, 16 * 1024 * 1024, 0, &mctx2);
            rc = vtbl2 ? vtbl2->register_storage(mctx2, &bad) : -1;
            CHECK(rc == UMM_E_INVALID_ARG,
                  "libnvm capacity=0 → 拒绝注册");
            if (vtbl2)
                mem_service_direct_destroy(mctx2);

            vtbl->free_tiered(mctx, UMM_TIER_SSD, off, 1024 * 1024);
            mem_service_direct_destroy(mctx);
        }
    }

    /* ---- 6. 开机只读探针：损坏 ctx 剔除 / 全灭报错（桩库故障注入，
     *        仅桩库模式运行——真机模式无注入环境变量，探针应全过）---- */
    if (!init_only && !getenv("UMM_TEST_LIBNVM_CTRL")) {
        setenv("STUB_SEQ_RESET", "1", 1);
        setenv("STUB_READ_FAIL_ON_IDS", "1,2,3", 1);
        SsdBackend *sb2 = ssd_backend_create(ctrl, cap);
        CHECK(sb2 != NULL, "探针剔除 3/4 损坏 ctx 后降级打开");
        if (sb2) {
            uint8_t pbuf6[4096];
            memset(pbuf6, 0xA5, sizeof(pbuf6));
            rc = ssd_backend_pwrite(sb2, off, sizeof(pbuf6), pbuf6);
            CHECK(rc == UMM_OK, "降级池（仅剩好 ctx）I/O 可用");
            ssd_backend_destroy(sb2);
        }
        unsetenv("STUB_READ_FAIL_ON_IDS");

        setenv("STUB_SEQ_RESET", "1", 1);
        setenv("STUB_READ_FAIL_ON_IDS", "0,1,2,3", 1);
        sb2 = ssd_backend_create(ctrl, cap);
        CHECK(sb2 == NULL, "全 ctx 探针失败 → 打开报错（启动期可见）");
        unsetenv("STUB_READ_FAIL_ON_IDS");
    }

    unlink("/tmp/libnvm_stub_disk.raw");

    printf("  Results: %d passed, %d failed\n\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
