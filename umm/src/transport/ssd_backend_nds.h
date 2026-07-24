/*
 * ssd_backend_nds.h — NDS NPU2SSD 直驱后端（dlopen 软依赖，纯 C 接口）
 *
 * 依赖 libnds_aiv.so（C++ 库，单例类 NDS，见 include/nds_aiv.h）。
 * UMM 核心保持纯 C：不引入 C++ 编译/链接到 UMM 核心，而是通过
 * dlopen + dlsym 解析 Itanium ABI mangled 符号（成员函数按 Itanium
 * ABI 以 this 为首参的函数指针调用）。因此本头文件可安全进入纯 C
 * 编译单元。
 *
 * 设备路径约定："nds:<device_id>[+<base_off>]"
 *   例: "nds:0"                （NPU 设备 0，从盘首开始管理）
 *       "nds:0+0x40000000"     （窗口基址 1GB）
 *   base_off 语义与 libnvm 后端完全相同：所有 I/O 在窗口内相对编址，
 *   落盘 f_offset = base_off + 窗口内偏移。用于避开 LBA0 区域。
 *
 * 容量说明：NDS 无容量/磁盘信息查询接口，配置中必须显式给出容量
 * （umms.yaml: "nds:0+0x40000000:16G"），capacity=0 拒绝。
 *
 * 与 libnvm 后端的关键差异：
 *   1. 数据缓冲是 NPU device 内存（device 虚拟地址），不是 host
 *      buffer——I/O 前必须 ssd_nds_register_mem() 注册设备内存段，
 *      未注册的 read/write/batch 一律拒绝（UMM_E_INVALID_ARG）。
 *      支持多段注册（至多 16 段，与 NDS 提供方真实测试代码的连续
 *      多段 nds_register 用法一致）；单条 IO 的 vaddr 必须完整落在
 *      某一个段内，跨段 IO 拒绝。
 *   2. NDS 全部接口 void 返回：无法在调用后感知错误，只能在调用前
 *      做参数校验（page 对齐、长度上限、已注册状态）。若真机运行期
 *      出错，NDS 侧无法上报——这是 NDS API 的固有限制。
 *   3. nds_init 是单例一次性初始化：进程内多 backend 打开同一
 *      device_id 时按引用计数复用（仅首个 open 调 nds_init，最后
 *      一个 close 调 nds_uninit）。
 *   4. 单次 I/O 上限 = UMM_NDS_MAX_IO（缺省 1MB，env 可调；与
 *      nds_init 的 max_page_num 解耦——后者是 NDS 内部 IO 跟踪
 *      资源池规模，对齐提供方测试代码的 7M，不是单次 IO 上限）；
 *      offset 与 len 必须 page_size 对齐。无 bounce buffer（host
 *      无法 memcpy device 内存）。
 *   5. open 不做 I/O 探针（与 libnvm 不同：无已注册 device 内存，
 *      无法构造合法探针）。
 */
#ifndef UMM_SSD_BACKEND_NDS_H
#define UMM_SSD_BACKEND_NDS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 与 NDS IOVec 标准布局一致的 POD（vaddr 为 NPU device 虚拟地址，
 * offset 为窗口内 SSD 字节偏移） */
typedef struct UmmNdsIOVec {
    void    *vaddr;             /* NPU device 虚拟地址 */
    uint64_t length;
    uint64_t offset;            /* 窗口内 SSD 字节偏移 */
} UmmNdsIOVec;

typedef struct SsdNdsBackend SsdNdsBackend;

/* 打开 NDS 设备（spec 不含 "nds:" 前缀，形如 "0" 或 "0+0x40000000"） */
int      ssd_nds_open(const char *spec, SsdNdsBackend **out);

/* 单次 I/O 上限（UMM_NDS_MAX_IO，缺省 1MB）与页尺寸（对齐单位） */
uint64_t ssd_nds_max_io(SsdNdsBackend *b);
uint32_t ssd_nds_block_size(SsdNdsBackend *b);   /* = page_size */

/* 注册 NPU device 内存段：dev_mem 为 device 侧基址，aligned_size
 * 须按 page_size 对齐。I/O 前必须调用；支持多段注册（至多 16 段，
 * 与 NDS 提供方真实测试代码一致——其样例连续注册 4 个独立段），
 * 超上限返回 UMM_E_NO_MEMORY；完全相同（base 与 size 均相同）的
 * 重复注册幂等返回 UMM_OK（不重复下发 nds_register） */
int      ssd_nds_register_mem(SsdNdsBackend *b, void *dev_mem,
                              uint64_t aligned_size);

/* 同步读写（offset 为窗口内字节偏移，须 page_size 对齐；dev_vaddr 为
 * 已注册的 device 虚拟地址；内部按 max_io 分段调 single_read/write） */
int      ssd_nds_read (SsdNdsBackend *b, uint64_t offset,
                       uint64_t len, void *dev_vaddr);
int      ssd_nds_write(SsdNdsBackend *b, uint64_t offset,
                       uint64_t len, const void *dev_vaddr);

/* 批量 I/O：逐 iov 校验（已注册、对齐、0 < length <= max_io），
 * iov.offset 加 base_off 后一次下发。n_iov==0 为 no-op */
int      ssd_nds_batch_read (SsdNdsBackend *b, UmmNdsIOVec *iovs,
                             size_t n_iov);
int      ssd_nds_batch_write(SsdNdsBackend *b, const UmmNdsIOVec *iovs,
                             size_t n_iov);

void     ssd_nds_close(SsdNdsBackend *b);

#ifdef __cplusplus
}
#endif

#endif /* UMM_SSD_BACKEND_NDS_H */
