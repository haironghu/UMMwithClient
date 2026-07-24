/*
 * stub_nds_aiv.cpp — libnds_aiv.so 的测试桩
 *
 * 用常规文件 /tmp/nds_stub_disk.raw 模拟 SSD，实现 nds_aiv.h 的 NDS
 * 单例类全部成员函数，用于在没有真实 NDS 库/NPU 硬件的环境中测试
 * ssd_backend_nds（验证 dlsym Itanium mangled 符号方案端到端可行：
 * 真实 libnds_aiv.so 只要按 nds_aiv.h 的 ABI 编译即可直接接入）。
 *
 * 桩的违规语义：single/batch 调用前校验失败（未注册、vaddr 未完整
 * 落在任一注册段内、超 max_io、未按 page 对齐）直接打印原因并
 * abort()——桩用于测试，违规即测试 bug，宁 abort 不静默。
 * 多段注册：nds_register 追加段（至多 16 段），对齐 NDS 提供方
 * 真实测试代码的连续多段注册用法。
 *
 * 构建: g++ -shared -fPIC -std=c++11 -o libnds_aiv.so stub_nds_aiv.cpp
 */
#include "../include/nds_aiv.h"

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

#define STUB_DISK "/tmp/nds_stub_disk.raw"

/* 桩内单次 IO 上限：与 UMM 的 UMM_NDS_MAX_IO 解耦的独立常量（1MB）。
 * 真实 NDS 的单次 IO 上限待库方文档确认（提供方样例 per-iov 8192） */
#define STUB_MAX_IO       (1024 * 1024)
/* 与 UMM 侧 NDS_MAX_REGIONS 一致：多段注册（对齐提供方测试代码的
 * 连续 4 段 nds_register 用法） */
#define STUB_MAX_REGIONS  16

struct NDS::Impl {
    int      fd           = -1;
    uint32_t device_id    = 0;
    size_t   queue_depth  = 0;
    size_t   core_num     = 0;
    uint64_t page_size    = 0;
    uint64_t max_page_num = 0;      /* NDS 内部 IO 资源池规模（非单次上限） */
    uint64_t max_io       = STUB_MAX_IO;
    /* 多段注册表：n_regions>0 即"已注册"；IO 的 vaddr 必须完整落在
     * 某一个段内（跨段 abort） */
    void    *regions[STUB_MAX_REGIONS]      = {};
    uint64_t region_sizes[STUB_MAX_REGIONS] = {};
    uint32_t n_regions    = 0;
};

NDS &NDS::Instance() noexcept
{
    static NDS inst;
    return inst;
}

NDS::NDS() : m_impl(new Impl) {}

NDS::~NDS()
{
    if (m_impl->fd >= 0)
        close(m_impl->fd);
    delete m_impl;
}

void NDS::nds_init(uint32_t deviceId, size_t queueDepth, size_t coreNum,
                   uint64_t page_size, uint64_t max_page_num)
{
    /* 允许重复 init（多 device 场景由 UMM 注册表保证每 device 一次；
     * 桩只保留最近一次 init 的参数——测试只用单 device） */
    if (m_impl->fd >= 0)
        close(m_impl->fd);
    int fd = open(STUB_DISK, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        fprintf(stderr, "stub_nds_aiv: open(%s) failed\n", STUB_DISK);
        abort();
    }
    /* 模拟设备语义：任意偏移恒可读。稀疏扩展到 1GB，
     * 否则空文件 pread 返回 0（短读） */
    ftruncate(fd, 1LL << 30);
    m_impl->fd           = fd;
    m_impl->device_id    = deviceId;
    m_impl->queue_depth  = queueDepth;
    m_impl->core_num     = coreNum;
    m_impl->page_size    = page_size;
    m_impl->max_page_num = max_page_num;
    m_impl->max_io       = STUB_MAX_IO;
    m_impl->n_regions    = 0;
    for (int i = 0; i < STUB_MAX_REGIONS; i++) {
        m_impl->regions[i]      = nullptr;
        m_impl->region_sizes[i] = 0;
    }
    fprintf(stderr, "stub_nds_aiv: nds_init(dev=%u, qd=%zu, cores=%zu, "
            "page=%lu, max_pages=%lu)\n", deviceId, queueDepth, coreNum,
            (unsigned long)page_size, (unsigned long)max_page_num);
}

void NDS::nds_uninit()
{
    if (m_impl->fd >= 0)
        close(m_impl->fd);
    m_impl->fd        = -1;
    m_impl->n_regions = 0;
    for (int i = 0; i < STUB_MAX_REGIONS; i++) {
        m_impl->regions[i]      = nullptr;
        m_impl->region_sizes[i] = 0;
    }
}

void NDS::nds_register(void *dev_mem, uint64_t aligned_read_size)
{
    if (m_impl->fd < 0) {
        fprintf(stderr, "stub_nds_aiv: nds_register before nds_init\n");
        abort();
    }
    if (m_impl->n_regions >= STUB_MAX_REGIONS) {
        fprintf(stderr, "stub_nds_aiv: nds_register 超段数上限 %d\n",
                STUB_MAX_REGIONS);
        abort();
    }
    m_impl->regions[m_impl->n_regions]      = dev_mem;
    m_impl->region_sizes[m_impl->n_regions] = aligned_read_size;
    m_impl->n_regions++;
}

/* 调用前校验（违规 = 测试 bug，打印原因并 abort）。
 * 用宏实现：NDS::Impl 是 private 嵌套类型，自由函数无法 naming，
 * 宏只在成员函数内展开（m_impl 处类型上下文合法） */
#define STUB_CHECK_IO(im, vaddr, bytes, f_offset, what) do {            \
    if ((im)->fd < 0) {                                                 \
        fprintf(stderr, "stub_nds_aiv: %s: nds_init 未调用\n", what);   \
        abort();                                                        \
    }                                                                   \
    if ((im)->n_regions == 0) {                                         \
        fprintf(stderr, "stub_nds_aiv: %s: 未 nds_register\n", what);   \
        abort();                                                        \
    }                                                                   \
    if ((bytes) == 0 || (bytes) > (im)->max_io) {                       \
        fprintf(stderr, "stub_nds_aiv: %s: bytes=%lu 超 max_io=%lu\n",  \
                what, (unsigned long)(bytes),                           \
                (unsigned long)(im)->max_io);                           \
        abort();                                                        \
    }                                                                   \
    if (((bytes) % (im)->page_size) != 0 ||                             \
        ((f_offset) % (im)->page_size) != 0) {                          \
        fprintf(stderr, "stub_nds_aiv: %s: bytes/off 未按 page=%lu "    \
                "对齐\n", what, (unsigned long)(im)->page_size);        \
        abort();                                                        \
    }                                                                   \
    /* 多段校验：vaddr 必须完整落在某一个注册段内（跨段 abort） */     \
    unsigned char *va_   = (unsigned char *)(vaddr);                    \
    int in_seg_ = 0;                                                    \
    for (uint32_t si_ = 0; si_ < (im)->n_regions; si_++) {              \
        unsigned char *base_ = (unsigned char *)(im)->regions[si_];     \
        if (va_ >= base_ &&                                             \
            va_ + (bytes) <= base_ + (im)->region_sizes[si_]) {         \
            in_seg_ = 1;                                                \
            break;                                                      \
        }                                                               \
    }                                                                   \
    if (!in_seg_) {                                                     \
        fprintf(stderr, "stub_nds_aiv: %s: vaddr=%p 未完整落在任一 "    \
                "注册段内（共 %u 段）\n", what, (void *)(vaddr),        \
                (im)->n_regions);                                       \
        abort();                                                        \
    }                                                                   \
} while (0)

void NDS::nds_single_write(void *vaddr, uint64_t bytes, uint64_t f_offset)
{
    STUB_CHECK_IO(m_impl, vaddr, bytes, f_offset, "single_write");
    uint64_t done = 0;
    while (done < bytes) {
        ssize_t n = pwrite(m_impl->fd, (char *)vaddr + done,
                           (size_t)(bytes - done), (off_t)(f_offset + done));
        if (n <= 0) {
            fprintf(stderr, "stub_nds_aiv: single_write pwrite failed\n");
            abort();
        }
        done += (uint64_t)n;
    }
}

void NDS::nds_single_read(void *vaddr, uint64_t bytes, uint64_t f_offset)
{
    STUB_CHECK_IO(m_impl, vaddr, bytes, f_offset, "single_read");
    uint64_t done = 0;
    while (done < bytes) {
        ssize_t n = pread(m_impl->fd, (char *)vaddr + done,
                          (size_t)(bytes - done), (off_t)(f_offset + done));
        if (n <= 0) {
            fprintf(stderr, "stub_nds_aiv: single_read pread failed\n");
            abort();
        }
        done += (uint64_t)n;
    }
}

void NDS::nds_batch_write(IOVec *iovecs, size_t n_iov)
{
    for (size_t i = 0; i < n_iov; i++)
        nds_single_write(iovecs[i].vaddr, iovecs[i].length,
                         iovecs[i].offset);
}

void NDS::nds_batch_read(IOVec *iovecs, size_t n_iov)
{
    for (size_t i = 0; i < n_iov; i++)
        nds_single_read(iovecs[i].vaddr, iovecs[i].length,
                        iovecs[i].offset);
}
