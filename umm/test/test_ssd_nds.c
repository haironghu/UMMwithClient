/* ========================================================================
 * test_ssd_nds.c — NDS NPU2SSD 直驱后端接入测试
 *
 * 用 test/stub_nds_aiv.cpp 编出的桩库（文件模拟 SSD）验证：
 *   1. backend_create("nds:...") 分派、容量、get_ptr==NULL、capacity=0 拒绝
 *   2. 未注册 device 内存时 I/O 拒绝（UMM_E_INVALID_ARG）
 *   3. register_dev_mem 后小 IO（8KB）写读回环一致
 *   4. 大 IO（3MB > max_io 1MB）内部分段写读一致
 *   5. 非 page 对齐 offset/len 拒绝
 *   6. 后端级 batch_write/batch_read 一致性 + 违规 iov 拒绝
 *   7. 池级混合后端（nds + 文件）跨后端分段 I/O
 *   8. 池级 batch（iov.offset 为池虚拟偏移）+ 跨设备 iov 拒绝
 *   9. mem_service 全链路（register_storage("nds:...")）
 *  10. 多段注册（对齐 NDS 提供方真实测试代码）：连续注册 3 个独立
 *      段全部成功；完全相同（同 base 同 size）重复注册幂等 UMM_OK；
 *      段1/段3 各自写读回环一致；vaddr 落在段间空隙（未注册 malloc）
 *      → 拒绝；单条 IO 跨出段1边界 → 拒绝；batch 夹跨段 iov →
 *      整体拒绝且先校验后下发
 *  11. R4 负路径：vaddr 越出注册区间（单条未注册 buffer / 尾部跨界 /
 *      batch 夹非法 iov）→ 拒绝，且先校验后下发（合法 iov 不落盘）
 *  12. R2 负路径：后端级 batch iov.offset+length 超 capacity → 拒绝；
 *      边界 == capacity → 成功（防误杀）
 *  13. R1 并发：同进程双 backend 打开同一 nds:0（refcount 共享
 *      entry），register 一次后 4 线程并发写读回环——per-device
 *      io_lock 下无死锁、数据一致
 *  15. batch 单发模拟兜底（过渡措施）：setenv UMM_NDS_BATCH_*_EMULATE=1
 *      后重开 backend，batch_write/batch_read 经逐 iov 单发循环，
 *      数据一致性 PASS；用毕 unsetenv（env 仅 open 时读取）
 *
 * 环境变量：
 *   UMM_NDS_PATH         桩库路径（缺省 ./libnds_aiv.so）
 *   UMM_TEST_NDS_SPEC    设备 spec（缺省 nds:0；真机如 nds:0）
 *   UMM_TEST_NDS_OFF     写测试基址偏移（避开 LBA0 区域，默认 0）
 *   UMM_TEST_NDS_INITONLY=1  只做 open/close 冒烟，零写入风险
 *
 * 测试中用 malloc 的 host 内存模拟 NPU device 内存（fake_dev）：
 * 桩库按 host 指针直接 pread/pwrite，真实 NDS 中该地址为 device vaddr。
 * ======================================================================== */

#include "../include/umm.h"
#include "../src/transport/ssd_pool.h"
#include "../src/memory_service/mem_service.h"
#include "../src/memory_service/mem_service_direct.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do {                                   \
    if (cond) { g_pass++; printf("  [PASS] %s\n", msg); }       \
    else      { g_fail++; printf("  [FAIL] %s\n", msg); }       \
} while (0)

#define FAKE_DEV_SIZE (16 * 1024 * 1024)

static void fill_pattern(uint8_t *p, uint64_t len, uint64_t seed)
{
    for (uint64_t i = 0; i < len; i++)
        p[i] = (uint8_t)(i * 7 + seed + (i >> 12));
}

/* ---- 用例13（R1）并发线程：固定一个 backend，在独占的 1MB 盘区域内
 * 循环 32KB 写后读回校验（每次落点在该 1MB 内轮转） ---- */
typedef struct {
    SsdBackend *sb;
    uint8_t    *vaddr;          /* 线程独占 device 缓冲（32KB） */
    uint64_t    region_off;     /* 线程独占 1MB 盘区域基址 */
    int         iters;
    int         fails;
} NdsIoThreadArg;

#define NDS_THREAD_IO_LEN (32 * 1024)

static void *nds_io_thread(void *p)
{
    NdsIoThreadArg *a = (NdsIoThreadArg *)p;
    uint8_t exp[NDS_THREAD_IO_LEN];
    for (int it = 0; it < a->iters; it++) {
        uint64_t off = a->region_off +
                       (uint64_t)(it % 32) * NDS_THREAD_IO_LEN;
        fill_pattern(a->vaddr, NDS_THREAD_IO_LEN,
                     (uint64_t)(181 + it));
        if (ssd_backend_pwrite(a->sb, off, NDS_THREAD_IO_LEN,
                               a->vaddr) != UMM_OK) {
            a->fails++;
            continue;
        }
        memset(a->vaddr, 0, NDS_THREAD_IO_LEN);
        if (ssd_backend_pread(a->sb, off, NDS_THREAD_IO_LEN,
                              a->vaddr) != UMM_OK) {
            a->fails++;
            continue;
        }
        fill_pattern(exp, NDS_THREAD_IO_LEN, (uint64_t)(181 + it));
        if (memcmp(a->vaddr, exp, NDS_THREAD_IO_LEN) != 0)
            a->fails++;
    }
    return NULL;
}

int main(void)
{
    printf("\n=== Test: SSD nds 后端（NPU2SSD 直驱）===\n");

    /* 桩库路径：默认与测试二进制同目录的 libnds_aiv.so */
    if (!getenv("UMM_NDS_PATH"))
        setenv("UMM_NDS_PATH", "./libnds_aiv.so", 0);
    unlink("/tmp/nds_stub_disk.raw");

    const char *spec = getenv("UMM_TEST_NDS_SPEC");
    if (!spec)
        spec = "nds:0";
    uint64_t base_off = 0;
    const char *off_env = getenv("UMM_TEST_NDS_OFF");
    if (off_env)
        base_off = strtoull(off_env, NULL, 0);
    int init_only = getenv("UMM_TEST_NDS_INITONLY") != NULL;

    /* 真机带基址偏移时，容量需覆盖 base_off + 测试用量 */
    uint64_t cap = 16 * 1024 * 1024;
    if (base_off)
        cap = base_off + 16 * 1024 * 1024;

    /* ---- 1. 创建 nds 后端 ---- */
    SsdBackend *sb = ssd_backend_create(spec, cap);
    CHECK(sb != NULL, "backend_create(nds:) 分派成功");
    if (!sb) {
        printf("  [SKIP] 桩库不可用，后续用例跳过\n");
        printf("  Results: %d passed, %d failed\n\n", g_pass, g_fail);
        return g_fail ? 1 : 0;
    }
    CHECK(ssd_backend_capacity(sb) == cap,
          "容量 = 配置值（NDS 无容量查询接口）");
    CHECK(ssd_backend_get_ptr(sb, 0) == NULL,
          "nds 后端 get_ptr 返回 NULL（无 mmap）");
    CHECK(ssd_backend_create(spec, 0) == NULL,
          "capacity=0 拒绝（NDS 无法探测容量）");

    if (init_only) {
        printf("  [INFO] INITONLY 模式：open/close 冒烟通过，未做任何写入\n");
        ssd_backend_destroy(sb);
        printf("  Results: %d passed, %d failed (init-only)\n\n",
               g_pass, g_fail);
        return g_fail ? 1 : 0;
    }
    if (base_off)
        printf("  [INFO] 写测试基址偏移: %lu (0x%lx)\n",
               (unsigned long)base_off, (unsigned long)base_off);

    /* ---- 2. 未注册 device 内存 → I/O 拒绝 ---- */
    uint8_t wbuf[8192];
    fill_pattern(wbuf, sizeof(wbuf), 11);
    int rc = ssd_backend_pwrite(sb, base_off, 8192, wbuf);
    CHECK(rc == UMM_E_INVALID_ARG,
          "未注册 device 内存时 pwrite → UMM_E_INVALID_ARG");

    /* fake_dev：malloc 模拟 NPU device 内存（桩按 host 指针读写） */
    uint8_t *fake_dev = malloc(FAKE_DEV_SIZE);
    if (!fake_dev) {
        printf("  [FAIL] malloc fake_dev 失败\n");
        ssd_backend_destroy(sb);
        return 1;
    }

    /* ---- 3. 注册 + 小 IO（8KB）写读回环 ---- */
    rc = ssd_backend_register_dev_mem(sb, fake_dev, FAKE_DEV_SIZE);
    CHECK(rc == UMM_OK, "register_dev_mem(fake_dev, 16MB)");

    uint8_t *wa = fake_dev;              /* 写源区域 */
    uint8_t *ra = fake_dev + 8 * 1024 * 1024;  /* 读回区域 */
    fill_pattern(wa, 8192, 23);
    rc = ssd_backend_pwrite(sb, base_off, 8192, wa);
    CHECK(rc == UMM_OK, "pwrite 8KB（device 内存 → SSD）");
    memset(ra, 0, 8192);
    rc = ssd_backend_pread(sb, base_off, 8192, ra);
    CHECK(rc == UMM_OK && memcmp(wa, ra, 8192) == 0,
          "pread 8KB 读回一致");

    /* ---- 4. 大 IO（3MB > max_io 1MB，触发内部分段）---- */
    uint64_t big = 3 * 1024 * 1024;
    uint64_t mio = ssd_backend_max_io(sb);
    char segmsg[128];
    snprintf(segmsg, sizeof(segmsg), "pwrite 3MB（%lu 段 x %luKB）",
             (unsigned long)((big + mio - 1) / mio),
             (unsigned long)(mio / 1024));
    fill_pattern(wa, big, 37);
    rc = ssd_backend_pwrite(sb, base_off + 1024 * 1024, big, wa);
    CHECK(rc == UMM_OK, segmsg);
    memset(ra, 0, big);
    rc = ssd_backend_pread(sb, base_off + 1024 * 1024, big, ra);
    CHECK(rc == UMM_OK && memcmp(wa, ra, big) == 0,
          "pread 3MB 分段读回一致");

    /* ---- 5. 非 page 对齐 offset/len 拒绝 ---- */
    rc = ssd_backend_pwrite(sb, base_off, 1000, wa);
    CHECK(rc == UMM_E_INVALID_ARG, "len=1000 非页对齐 → 拒绝");
    rc = ssd_backend_pread(sb, base_off + 100, 4096, ra);
    CHECK(rc == UMM_E_INVALID_ARG, "offset 非页对齐 → 拒绝");

    /* ---- 6. 后端级 batch：4 个 iov 写读一致 + 违规 iov 拒绝 ---- */
    {
        UmmNdsIOVec iovs[4];
        uint64_t voff_addr[4] = {0, 4 * 1024 * 1024, 8 * 1024 * 1024,
                                 12 * 1024 * 1024};
        uint64_t disk_off[4]  = {3 * 1024 * 1024, 5 * 1024 * 1024,
                                 2 * 1024 * 1024, 6 * 1024 * 1024};
        for (int i = 0; i < 4; i++) {
            iovs[i].vaddr  = fake_dev + voff_addr[i];
            iovs[i].length = 8192;
            iovs[i].offset = base_off + disk_off[i];
            fill_pattern(iovs[i].vaddr, 8192, (uint64_t)(51 + i));
        }
        rc = ssd_backend_batch_write(sb, iovs, 4);
        CHECK(rc == UMM_OK, "batch_write 4 iov（散布 vaddr/offset）");
        for (int i = 0; i < 4; i++)
            memset(iovs[i].vaddr, 0, 8192);
        rc = ssd_backend_batch_read(sb, iovs, 4);
        int ok = (rc == UMM_OK);
        for (int i = 0; i < 4 && ok; i++) {
            uint8_t *exp = malloc(8192);
            fill_pattern(exp, 8192, (uint64_t)(51 + i));
            ok = (memcmp(iovs[i].vaddr, exp, 8192) == 0);
            free(exp);
        }
        CHECK(ok, "batch_read 逐 iov 读回一致");

        UmmNdsIOVec bad = {fake_dev, 2 * 1024 * 1024, base_off};
        rc = ssd_backend_batch_write(sb, &bad, 1);
        CHECK(rc == UMM_E_INVALID_ARG,
              "batch iov length > max_io → 拒绝");
        bad.length = 1000;
        rc = ssd_backend_batch_read(sb, &bad, 1);
        CHECK(rc == UMM_E_INVALID_ARG,
              "batch iov length 非页对齐 → 拒绝");
    }
    ssd_backend_destroy(sb);

    /* ---- 7. 池级混合后端：nds + 文件，跨后端分段 ----
     * 真机：spec 带 "+base_off" 窗口后缀，池内 I/O 落盘于
     * [base_off, base_off+8MB)，不触碰 LBA0 区域 */
    char spec7[320];
    if (base_off)
        snprintf(spec7, sizeof(spec7), "%s+%lu",
                 spec, (unsigned long)base_off);
    else
        snprintf(spec7, sizeof(spec7), "%s", spec);
    const char *f1 = "/tmp/umm_nds_mix1.raw";
    unlink(f1);
    SsdPool *pool = ssd_pool_create();
    rc = ssd_pool_add_device(pool, spec7, 8 * 1024 * 1024);
    CHECK(rc == UMM_OK, "pool 加入 nds 设备（8MB）");
    rc = ssd_pool_add_device(pool, f1, 8 * 1024 * 1024);
    CHECK(rc == UMM_OK, "pool 加入文件设备（8MB）");
    rc = ssd_pool_register_dev_mem(pool, fake_dev, FAKE_DEV_SIZE);
    CHECK(rc == UMM_OK, "pool_register_dev_mem（转发 nds 设备）");

    uint64_t voff = 0;
    rc = ssd_pool_alloc(pool, 12 * 1024 * 1024, &voff);
    CHECK(rc == UMM_OK && voff == 0, "分配 12MB 跨 nds+文件 后端");

    uint64_t len = 12 * 1024 * 1024;
    uint8_t *w2 = malloc(len);
    fill_pattern(fake_dev, len, 67);
    memcpy(w2, fake_dev, len);
    rc = ssd_pool_pwrite(pool, voff, len, fake_dev);
    CHECK(rc == UMM_OK, "pool_pwrite 跨后端分段写（buf=device 内存）");
    memset(fake_dev, 0, len);
    rc = ssd_pool_pread(pool, voff, len, fake_dev);
    CHECK(rc == UMM_OK && memcmp(w2, fake_dev, len) == 0,
          "pool_pread 跨后端分段读回一致");
    free(w2);
    ssd_pool_destroy(pool);

    /* ---- 8. 池级 batch：iov.offset 为池虚拟偏移 + 跨设备 iov 拒绝 ---- */
    pool = ssd_pool_create();
    rc = ssd_pool_add_device(pool, spec7, 8 * 1024 * 1024);
    CHECK(rc == UMM_OK, "pool(8) 加入 nds 设备");
    rc = ssd_pool_add_device(pool, f1, 8 * 1024 * 1024);
    CHECK(rc == UMM_OK, "pool(8) 加入文件设备");
    rc = ssd_pool_register_dev_mem(pool, fake_dev, FAKE_DEV_SIZE);
    CHECK(rc == UMM_OK, "pool(8) register_dev_mem");
    {
        /* 4 个 iov：前两个落 nds 设备（池虚拟 [0,8MB)），
         * 后两个落文件设备（池虚拟 [8MB,16MB)） */
        UmmNdsIOVec iovs[4];
        uint64_t vaddr_off[4] = {0, 8192, 16384, 24576};
        uint64_t pool_off[4]  = {0, 4 * 1024 * 1024,
                                 8 * 1024 * 1024, 12 * 1024 * 1024};
        for (int i = 0; i < 4; i++) {
            iovs[i].vaddr  = fake_dev + vaddr_off[i];
            iovs[i].length = 8192;
            iovs[i].offset = pool_off[i];
            fill_pattern(iovs[i].vaddr, 8192, (uint64_t)(91 + i));
        }
        rc = ssd_pool_batch_write(pool, iovs, 4);
        CHECK(rc == UMM_OK, "pool_batch_write（iov 跨两个设备）");
        for (int i = 0; i < 4; i++)
            memset(iovs[i].vaddr, 0, 8192);
        rc = ssd_pool_batch_read(pool, iovs, 4);
        int ok = (rc == UMM_OK);
        for (int i = 0; i < 4 && ok; i++) {
            uint8_t *exp = malloc(8192);
            fill_pattern(exp, 8192, (uint64_t)(91 + i));
            ok = (memcmp(iovs[i].vaddr, exp, 8192) == 0);
            free(exp);
        }
        CHECK(ok, "pool_batch_read 逐 iov 读回一致");

        /* 跨设备 iov：[8MB-4KB, +8KB) 横跨 nds 与文件设备 → 拒绝 */
        UmmNdsIOVec cross = {fake_dev + 32768, 8192,
                             8 * 1024 * 1024 - 4096};
        rc = ssd_pool_batch_write(pool, &cross, 1);
        CHECK(rc == UMM_E_INVALID_ARG, "跨设备 iov → UMM_E_INVALID_ARG");
    }
    ssd_pool_destroy(pool);
    unlink(f1);

    /* 纯文件池（无 nds 设备）register → 拒绝 */
    pool = ssd_pool_create();
    rc = ssd_pool_add_device(pool, f1, 8 * 1024 * 1024);
    if (rc == UMM_OK) {
        rc = ssd_pool_register_dev_mem(pool, fake_dev, FAKE_DEV_SIZE);
        CHECK(rc == UMM_E_INVALID_ARG,
              "池内无 nds 设备时 register_dev_mem → 拒绝");
    }
    ssd_pool_destroy(pool);
    unlink(f1);

    /* ---- 9. mem_service 层：register_storage("nds:...") 全链路 ----
     * 注册状态按 device 共享（NDS 单例语义）：测试自持一个 backend
     * 完成 register，mem_service 内部 backend 随即具备 I/O 能力 */
    {
        SsdBackend *sb9 = ssd_backend_create(spec7, cap);
        CHECK(sb9 != NULL, "用例9：自持 backend（同 device 复用单例）");
        rc = sb9 ? ssd_backend_register_dev_mem(sb9, fake_dev,
                                                FAKE_DEV_SIZE)
                 : -1;
        CHECK(rc == UMM_OK, "用例9：register_dev_mem（按 device 共享）");

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
            snprintf(res.device_path, sizeof(res.device_path),
                     "%.255s", spec7);
            rc = vtbl->register_storage(mctx, &res);
            CHECK(rc == UMM_OK,
                  "register_storage(nds:) 路径正确解析");

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
            CHECK(rc != UMM_OK, "map_device 对 nds 失败（预期）");

            uint8_t *wb9 = fake_dev + 8 * 1024 * 1024;
            uint8_t *rb9 = fake_dev + 12 * 1024 * 1024;
            fill_pattern(wb9, 4096, 105);
            rc = vtbl->ssd_write(mctx, UMM_TIER_SSD, 0, off, 4096, wb9);
            CHECK(rc == UMM_OK, "vtbl->ssd_write → nds 4KB");
            memset(rb9, 0, 4096);
            rc = vtbl->ssd_read(mctx, UMM_TIER_SSD, 0, off, 4096, rb9);
            CHECK(rc == UMM_OK && memcmp(wb9, rb9, 4096) == 0,
                  "vtbl->ssd_read → nds 读回一致");

            /* capacity=0 必须被拒绝（NDS 无法探测容量） */
            StorageResource bad = res;
            bad.capacity = 0;
            void *mctx2 = NULL;
            MemoryServiceVtbl *vtbl2 =
                mem_service_direct_create(0, 16 * 1024 * 1024, 0, &mctx2);
            rc = vtbl2 ? vtbl2->register_storage(mctx2, &bad) : -1;
            CHECK(rc == UMM_E_INVALID_ARG,
                  "nds capacity=0 → 拒绝注册");
            if (vtbl2)
                mem_service_direct_destroy(mctx2);

            vtbl->free_tiered(mctx, UMM_TIER_SSD, off, 1024 * 1024);
            mem_service_direct_destroy(mctx);
        }
        if (sb9)
            ssd_backend_destroy(sb9);
    }

    /* ---- 10. 多段注册（对齐 NDS 提供方真实测试代码：样例连续
     * nds_register 4 个独立内存段，多段注册是 API 预期用法） ----
     * 用例9 的 backend 已全部 close（refcount 归零、device 已 uninit），
     * 此处 open 重建 entry */
    SsdBackend *sb10 = ssd_backend_create(spec, cap);
    CHECK(sb10 != NULL, "用例10：backend_create（多段/R4/R2 用例共用）");
    uint8_t *seg2    = malloc(4 * 1024 * 1024);
    uint8_t *seg3    = malloc(4 * 1024 * 1024);
    uint8_t *gap_buf = malloc(8192);   /* 未注册的独立 malloc（段外空隙） */
    if (sb10 && seg2 && seg3 && gap_buf) {
        /* 连续注册 3 个独立段 → 全部成功 */
        rc = ssd_backend_register_dev_mem(sb10, fake_dev, FAKE_DEV_SIZE);
        CHECK(rc == UMM_OK, "用例10：注册段1（fake_dev, 16MB）");
        rc = ssd_backend_register_dev_mem(sb10, seg2, 4 * 1024 * 1024);
        CHECK(rc == UMM_OK, "用例10：注册段2（4MB，多段追加）");
        rc = ssd_backend_register_dev_mem(sb10, seg3, 4 * 1024 * 1024);
        CHECK(rc == UMM_OK, "用例10：注册段3（4MB，多段追加）");
        /* 完全相同（同 base 同 size）重复注册 → 幂等 UMM_OK */
        rc = ssd_backend_register_dev_mem(sb10, seg2, 4 * 1024 * 1024);
        CHECK(rc == UMM_OK,
              "用例10：完全相同重复 register → 幂等 UMM_OK");

        /* 向段1、段3 各做写读回环（vaddr 落在对应段内即放行） */
        fill_pattern(fake_dev, 8192, 61);
        rc = ssd_backend_pwrite(sb10, base_off + 13 * 1024 * 1024,
                                8192, fake_dev);
        memset(fake_dev + 8192, 0, 8192);
        rc = rc == UMM_OK ?
             ssd_backend_pread(sb10, base_off + 13 * 1024 * 1024,
                               8192, fake_dev + 8192) : rc;
        {
            uint8_t *exp = malloc(8192);
            fill_pattern(exp, 8192, 61);
            CHECK(rc == UMM_OK &&
                  memcmp(fake_dev + 8192, exp, 8192) == 0,
                  "用例10：段1 写读回环一致");
            free(exp);
        }
        fill_pattern(seg3, 8192, 71);
        rc = ssd_backend_pwrite(sb10, base_off + 14 * 1024 * 1024,
                                8192, seg3);
        memset(seg3 + 8192, 0, 8192);
        rc = rc == UMM_OK ?
             ssd_backend_pread(sb10, base_off + 14 * 1024 * 1024,
                               8192, seg3 + 8192) : rc;
        {
            uint8_t *exp = malloc(8192);
            fill_pattern(exp, 8192, 71);
            CHECK(rc == UMM_OK && memcmp(seg3 + 8192, exp, 8192) == 0,
                  "用例10：段3 写读回环一致");
            free(exp);
        }

        /* vaddr 落在段外空隙（未注册 malloc）→ 拒绝 */
        fill_pattern(gap_buf, 8192, 83);
        rc = ssd_backend_pwrite(sb10, base_off, 8192, gap_buf);
        CHECK(rc == UMM_E_INVALID_ARG,
              "用例10：vaddr 落在注册段外空隙 → 拒绝");

        /* 单条 IO 从段1尾部跨出段界（vaddr=段1末尾页、len=2页）→
         * 拒绝：单条 IO 不允许横跨/跨出注册段 */
        uint8_t *tail1 = fake_dev + FAKE_DEV_SIZE - 4096;
        rc = ssd_backend_pwrite(sb10, base_off, 8192, tail1);
        CHECK(rc == UMM_E_INVALID_ARG,
              "用例10：单条 IO 跨出段1边界 → 拒绝");

        /* batch 中一个 iov 跨段 → 整体拒绝且先校验后下发：
         * 合法 iov 目标盘区域预埋旧数据，拒绝后必须未被改写 */
        {
            UmmNdsIOVec xiov[2];
            xiov[0].vaddr  = seg3;
            xiov[0].length = 8192;
            xiov[0].offset = base_off + 15 * 1024 * 1024 - 8192;
            xiov[1].vaddr  = tail1;     /* 跨出段1末尾的非法 iov */
            xiov[1].length = 8192;
            xiov[1].offset = base_off + 15 * 1024 * 1024;
            uint8_t *exp = malloc(8192);
            fill_pattern(seg3, 8192, 91);            /* 预埋旧数据 */
            rc = ssd_backend_pwrite(sb10, xiov[0].offset, 8192, seg3);
            CHECK(rc == UMM_OK, "用例10：预埋合法 iov 旧数据");
            fill_pattern(seg3, 8192, 97);            /* 换成新数据 */
            rc = ssd_backend_batch_write(sb10, xiov, 2);
            CHECK(rc == UMM_E_INVALID_ARG,
                  "用例10：batch 夹跨段 iov → 整体拒绝");
            memset(seg3, 0, 8192);
            rc = ssd_backend_pread(sb10, xiov[0].offset, 8192, seg3);
            fill_pattern(exp, 8192, 91);
            CHECK(rc == UMM_OK && memcmp(seg3, exp, 8192) == 0,
                  "用例10：先校验后下发，合法 iov 盘区域未被写");
            free(exp);
        }
    }

    /* ---- 11. R4 负路径：vaddr 越出已注册区间拦截 ---- */
    uint8_t *unreg_buf = malloc(8192);      /* 未注册的区间外 buffer */
    if (sb10 && unreg_buf) {
        fill_pattern(unreg_buf, 8192, 113);
        rc = ssd_backend_pwrite(sb10, base_off, 8192, unreg_buf);
        CHECK(rc == UMM_E_INVALID_ARG,
              "用例11/R4：未注册 buffer vaddr pwrite → 拒绝");

        /* vaddr 起于区间尾内一页，但 len 越出区间尾 */
        uint8_t *tail = fake_dev + FAKE_DEV_SIZE - 4096;
        rc = ssd_backend_pwrite(sb10, base_off, 8192, tail);
        CHECK(rc == UMM_E_INVALID_ARG,
              "用例11/R4：vaddr 在区间内但 len 越出区间尾 → 拒绝");

        /* batch：4 个 iov 中间夹一个区间外 vaddr → 整体拒绝，
         * 且先校验后下发：合法 iov 目标盘区域不得被写 */
        {
            UmmNdsIOVec biov[4];
            uint64_t bdisk[4]  = {9 * 1024 * 1024, 10 * 1024 * 1024,
                                  11 * 1024 * 1024, 12 * 1024 * 1024};
            uint8_t *bvaddr[4] = {fake_dev, unreg_buf,
                                  fake_dev + 2 * 1024 * 1024,
                                  fake_dev + 4 * 1024 * 1024};
            uint8_t *verify = fake_dev + 6 * 1024 * 1024;
            uint8_t *exp = malloc(8192);
            /* 先在合法 iov 的目标盘区域写入旧数据 */
            for (int i = 0; i < 4; i++) {
                if (i == 1)
                    continue;               /* 非法 iov 无需预埋 */
                fill_pattern(bvaddr[i], 8192, (uint64_t)(131 + i));
                rc = ssd_backend_pwrite(sb10, base_off + bdisk[i],
                                        8192, bvaddr[i]);
                if (rc != UMM_OK)
                    break;
            }
            CHECK(rc == UMM_OK, "用例11/R4：预埋合法 iov 旧数据");
            /* 合法 iov 换成新数据；非法 iov 夹中间 */
            for (int i = 0; i < 4; i++) {
                biov[i].vaddr  = bvaddr[i];
                biov[i].length = 8192;
                biov[i].offset = base_off + bdisk[i];
                if (i != 1)
                    fill_pattern(biov[i].vaddr, 8192,
                                 (uint64_t)(151 + i));
            }
            rc = ssd_backend_batch_write(sb10, biov, 4);
            CHECK(rc == UMM_E_INVALID_ARG,
                  "用例11/R4：batch 夹区间外 iov → 整体拒绝");
            /* 读回合法 iov 目标盘区域，必须仍是旧数据（未下发） */
            int old_ok = 1;
            for (int i = 0; i < 4 && old_ok; i++) {
                if (i == 1)
                    continue;
                memset(verify, 0, 8192);
                rc = ssd_backend_pread(sb10, base_off + bdisk[i],
                                       8192, verify);
                fill_pattern(exp, 8192, (uint64_t)(131 + i));
                old_ok = (rc == UMM_OK &&
                          memcmp(verify, exp, 8192) == 0);
            }
            CHECK(old_ok,
                  "用例11/R4：先校验后下发，合法 iov 盘区域未被写");
            free(exp);
        }

        /* ---- 12. R2 负路径：后端级 batch capacity 越界拦截 ---- */
        {
            /* offset + length 超过 capacity → 拒绝 */
            UmmNdsIOVec oob = {fake_dev, 8192, cap - 4096};
            rc = ssd_backend_batch_write(sb10, &oob, 1);
            CHECK(rc == UMM_E_INVALID_ARG,
                  "用例12/R2：iov offset+length 超 capacity → 拒绝");
            /* 边界：offset + length 恰好 == capacity → 成功（防误杀） */
            fill_pattern(fake_dev, 8192, 167);
            oob.offset = cap - 8192;
            rc = ssd_backend_batch_write(sb10, &oob, 1);
            CHECK(rc == UMM_OK,
                  "用例12/R2：iov offset+length == capacity → 成功");
        }
        ssd_backend_destroy(sb10);
    }
    free(seg2);
    free(seg3);
    free(gap_buf);
    free(unreg_buf);

    /* ---- 13. R1 并发：同进程双 backend 打开同一 device（refcount
     * 共享 entry），register 一次后 4 线程并发 I/O——per-device
     * io_lock 下无死锁、数据一致。桩库单 fd 的 pread/pwrite 本身
     * 线程安全，此用例不会因桩而误报 ---- */
    {
        SsdBackend *sbA = ssd_backend_create(spec, cap);
        SsdBackend *sbB = ssd_backend_create(spec, cap);
        CHECK(sbA != NULL && sbB != NULL,
              "用例13/R1：同 device 双 backend（refcount 共享 entry）");
        rc = sbA ? ssd_backend_register_dev_mem(sbA, fake_dev,
                                                FAKE_DEV_SIZE)
                 : -1;
        CHECK(rc == UMM_OK,
              "用例13/R1：register 一次（entry 共享，双 backend 生效）");
        if (sbA && sbB && rc == UMM_OK) {
            pthread_t th[4];
            NdsIoThreadArg args[4];
            /* 4 线程各占 1MB 盘区域：[12MB, 16MB)，互不重叠 */
            for (int i = 0; i < 4; i++) {
                args[i].sb         = (i % 2 == 0) ? sbA : sbB;
                args[i].vaddr      = fake_dev +
                                     (uint64_t)i * NDS_THREAD_IO_LEN;
                args[i].region_off = base_off + 12 * 1024 * 1024 +
                                     (uint64_t)i * 1024 * 1024;
                args[i].iters      = 32;
                args[i].fails      = 0;
                pthread_create(&th[i], NULL, nds_io_thread, &args[i]);
            }
            int all_ok = 1;
            for (int i = 0; i < 4; i++) {
                pthread_join(th[i], NULL);
                if (args[i].fails != 0)
                    all_ok = 0;
            }
            CHECK(all_ok,
                  "用例13/R1：4 线程 x 32 次 32KB 写读回环一致"
                  "（per-device 锁，无死锁）");
        }
        if (sbA)
            ssd_backend_destroy(sbA);
        if (sbB)
            ssd_backend_destroy(sbB);
    }

    /* ---- 14. nds-meta 纯分配后端（umms 侧簿记形态）----
     * 不 dlopen/不 nds_init/不占 RPC 连接/不 mmap，只做 bitmap 分配 */
    {
        SsdBackend *meta = ssd_backend_create("nds-meta:0", cap);
        CHECK(meta != NULL, "用例14：backend_create(nds-meta:0) 分派成功");
        if (meta) {
            CHECK(ssd_backend_capacity(meta) == cap,
                  "用例14：nds-meta 容量 = 配置值");
            CHECK(ssd_backend_get_ptr(meta, 0) == NULL,
                  "用例14：nds-meta get_ptr 返回 NULL（无 mmap）");
            CHECK(ssd_backend_max_io(meta) == 0,
                  "用例14：nds-meta max_io = 0（alloc-only 无 IO 概念）");
        }
        CHECK(ssd_backend_create("nds-meta:0", 0) == NULL,
              "用例14：nds-meta capacity=0 拒绝");
        CHECK(ssd_backend_create("nds-meta:", cap) == NULL,
              "用例14：nds-meta 空 spec 拒绝");

        if (meta) {
            /* 数据面一律 UMM_E_UNSUPPORTED */
            rc = ssd_backend_pwrite(meta, 0, 8192, fake_dev);
            CHECK(rc == UMM_E_UNSUPPORTED,
                  "用例14：nds-meta pwrite → UMM_E_UNSUPPORTED");
            rc = ssd_backend_pread(meta, 0, 8192, fake_dev);
            CHECK(rc == UMM_E_UNSUPPORTED,
                  "用例14：nds-meta pread → UMM_E_UNSUPPORTED");
            rc = ssd_backend_register_dev_mem(meta, fake_dev,
                                              FAKE_DEV_SIZE);
            CHECK(rc == UMM_E_UNSUPPORTED,
                  "用例14：nds-meta register_dev_mem → UMM_E_UNSUPPORTED");
            UmmNdsIOVec iov = {fake_dev, 8192, 0};
            rc = ssd_backend_batch_write(meta, &iov, 1);
            CHECK(rc == UMM_E_UNSUPPORTED,
                  "用例14：nds-meta batch_write → UMM_E_UNSUPPORTED");
            rc = ssd_backend_batch_read(meta, &iov, 1);
            CHECK(rc == UMM_E_UNSUPPORTED,
                  "用例14：nds-meta batch_read → UMM_E_UNSUPPORTED");

            /* 簿记功能：bitmap 分配/释放与文件/nds 后端一致 */
            uint64_t moff = 0;
            rc = ssd_backend_alloc(meta, 1024 * 1024, &moff);
            CHECK(rc == UMM_OK && moff == 0,
                  "用例14：nds-meta bitmap 分配 1MB");
            ssd_backend_free(meta, moff, 1024 * 1024);

            /* 带窗口基址后缀的 spec 同样解析（与 nds 同语义） */
            SsdBackend *meta_off =
                ssd_backend_create("nds-meta:0+0x40000000", cap);
            CHECK(meta_off != NULL &&
                  ssd_backend_capacity(meta_off) == cap,
                  "用例14：nds-meta:0+0x40000000 解析成功、容量一致");
            if (meta_off)
                ssd_backend_destroy(meta_off);

            /* 与 nds:0 同进程共存（各开各的，互不影响） */
            SsdBackend *nd = ssd_backend_create(spec, cap);
            CHECK(nd != NULL,
                  "用例14：nds:0 与 nds-meta:0 同进程共存");
            if (nd) {
                uint64_t noff = 0;
                rc = ssd_backend_alloc(nd, 1024 * 1024, &noff);
                CHECK(rc == UMM_OK && noff == 0,
                      "用例14：共存时 nds 端分配独立（各簿各的）");
                ssd_backend_free(nd, noff, 1024 * 1024);
                ssd_backend_destroy(nd);
            }
            ssd_backend_destroy(meta);
        }

        /* 池级：nds-meta + 文件设备混合，跨两者分配簿记完整 */
        const char *f14 = "/tmp/umm_nds_meta_mix.raw";
        unlink(f14);
        SsdPool *pool14 = ssd_pool_create();
        rc = ssd_pool_add_device(pool14, "nds-meta:0", 8 * 1024 * 1024);
        CHECK(rc == UMM_OK, "用例14：pool 加入 nds-meta 设备（8MB）");
        rc = ssd_pool_add_device(pool14, f14, 8 * 1024 * 1024);
        CHECK(rc == UMM_OK, "用例14：pool 加入文件设备（8MB）");
        uint64_t voff14 = 0;
        rc = ssd_pool_alloc(pool14, 12 * 1024 * 1024, &voff14);
        CHECK(rc == UMM_OK && voff14 == 0,
              "用例14：分配 12MB 跨 nds-meta+文件 后端（簿记完整）");
        uint32_t dev14;
        uint64_t poff14;
        rc = ssd_pool_translate(pool14, 8 * 1024 * 1024, &dev14, &poff14);
        CHECK(rc == UMM_OK && dev14 == 1 && poff14 == 0,
              "用例14：translate 跨界到文件设备正确");
        ssd_pool_free(pool14, voff14, 12 * 1024 * 1024);
        /* 池级 get_ptr 落 nds-meta 段 → NULL */
        CHECK(ssd_pool_get_ptr(pool14, 0) == NULL,
              "用例14：pool_get_ptr 落 nds-meta 段 → NULL");
        ssd_pool_destroy(pool14);
        unlink(f14);
    }

    /* ---- 15. batch 单发模拟兜底（过渡措施）----
     * setenv UMM_NDS_BATCH_WRITE_EMULATE=1 / UMM_NDS_BATCH_READ_EMULATE=1
     * 后重开 backend（env 在 open 时读取，已开 backend 不受影响）：
     * batch_write/batch_read 走逐 iov 单发循环，数据一致性 PASS。
     * 用毕 unsetenv，避免污染后续用例。 */
    {
        setenv("UMM_NDS_BATCH_WRITE_EMULATE", "1", 1);
        setenv("UMM_NDS_BATCH_READ_EMULATE", "1", 1);
        SsdBackend *sb15 = ssd_backend_create(spec, cap);
        CHECK(sb15 != NULL, "用例15：EMULATE env 置位后 backend_create");
        rc = sb15 ? ssd_backend_register_dev_mem(sb15, fake_dev,
                                                 FAKE_DEV_SIZE)
                  : -1;
        CHECK(rc == UMM_OK, "用例15：register_dev_mem");
        if (sb15 && rc == UMM_OK) {
            UmmNdsIOVec ei[4];
            uint64_t evoff[4] = {0, 4 * 1024 * 1024, 8 * 1024 * 1024,
                                 12 * 1024 * 1024};
            uint64_t edisk[4] = {1 * 1024 * 1024, 7 * 1024 * 1024,
                                 3 * 1024 * 1024, 5 * 1024 * 1024};
            for (int i = 0; i < 4; i++) {
                ei[i].vaddr  = fake_dev + evoff[i];
                ei[i].length = 8192;
                ei[i].offset = base_off + edisk[i];
                fill_pattern(ei[i].vaddr, 8192, (uint64_t)(191 + i));
            }
            rc = ssd_backend_batch_write(sb15, ei, 4);
            CHECK(rc == UMM_OK,
                  "用例15：EMULATE batch_write（单发循环）4 iov");
            for (int i = 0; i < 4; i++)
                memset(ei[i].vaddr, 0, 8192);
            rc = ssd_backend_batch_read(sb15, ei, 4);
            int ok15 = (rc == UMM_OK);
            for (int i = 0; i < 4 && ok15; i++) {
                uint8_t *exp = malloc(8192);
                fill_pattern(exp, 8192, (uint64_t)(191 + i));
                ok15 = (memcmp(ei[i].vaddr, exp, 8192) == 0);
                free(exp);
            }
            CHECK(ok15,
                  "用例15：EMULATE batch_read（单发循环）逐 iov 读回一致");
        }
        if (sb15)
            ssd_backend_destroy(sb15);
        /* 用毕 unsetenv：env 仅在 open 时读取，后续用例的 backend
         * 回到真 batch 路径 */
        unsetenv("UMM_NDS_BATCH_WRITE_EMULATE");
        unsetenv("UMM_NDS_BATCH_READ_EMULATE");
        SsdBackend *sb15b = ssd_backend_create(spec, cap);
        CHECK(sb15b != NULL,
              "用例15：unsetenv 后重开 backend（回到真 batch 路径）");
        if (sb15b)
            ssd_backend_destroy(sb15b);
    }

    free(fake_dev);
    unlink("/tmp/nds_stub_disk.raw");

    printf("  Results: %d passed, %d failed\n\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
