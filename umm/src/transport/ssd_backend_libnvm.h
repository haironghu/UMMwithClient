/*
 * ssd_backend_libnvm.h — libnvm 用户态 NVMe 库后端（NPU_Direct_Storage）
 *
 * 通过 dlopen 加载 libnvm_host.so（软依赖，未安装该库的机器上
 * libumm 其余功能不受影响）。libnvm 负责控制器接管与队列驱动，
 * UMM 只使用其同步 read/write 作为数据通路；空间分配语义全部
 * 保留在 UMM 的 ssd_pool/位图层。
 *
 * 设备路径约定："libnvm:<ctrl_path>[@<ns_id>][+<base_off>]"
 *   例: "libnvm:/dev/libnvm_helper0@1"              （ns_id 缺省为 1）
 *       "libnvm:/dev/libnvm_helper0@1+0x40000000"   （窗口基址 1GB）
 *   base_off 为可选的物理盘字节偏移（支持 0x 十六进制）：所有 I/O
 *   在窗口内相对编址，落盘时加上 base_off。用于在闲置盘上划出
 *   安全窗口、避开 LBA0 区域；缺省为 0（从盘首开始管理）。
 *
 * 容量说明：libnvm_host 的 disk_info 暂不包含盘总容量，
 * 因此配置中必须显式给出容量（umms.yaml: "libnvm:/dev/libnvm_helper0@1:500G"）。
 */
#ifndef UMM_SSD_BACKEND_LIBNVM_H
#define UMM_SSD_BACKEND_LIBNVM_H

#include <stdint.h>

typedef struct SsdLibnvmBackend SsdLibnvmBackend;

/* 打开 libnvm 控制器（spec 不含 "libnvm:" 前缀，形如 "/dev/libnvm_helper0@1"） */
int      ssd_libnvm_open(const char *spec, SsdLibnvmBackend **out);

/* 单次 I/O 上限（disk_info.max_data_size）与逻辑块尺寸 */
uint64_t ssd_libnvm_max_io(SsdLibnvmBackend *b);
uint32_t ssd_libnvm_block_size(SsdLibnvmBackend *b);

/* 同步读写（offset 为字节偏移；内部按 max_data_size 分段） */
int      ssd_libnvm_read (SsdLibnvmBackend *b, uint64_t offset,
                          uint64_t len, void *buf);
int      ssd_libnvm_write(SsdLibnvmBackend *b, uint64_t offset,
                          uint64_t len, const void *buf);

void     ssd_libnvm_close(SsdLibnvmBackend *b);

#endif /* UMM_SSD_BACKEND_LIBNVM_H */
