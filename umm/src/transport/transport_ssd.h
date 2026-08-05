/* ========================================================================
 * transport_ssd.h — SSD transport factory
 *
 * Public interface for creating an SSD-backed MemoryTransportVtbl.
 *
 * Usage:
 *   void *ssd_ctx = NULL;
 *   MemoryTransportVtbl *ssd_vtbl = ssd_transport_create("/tmp/umm_ssd",
 *                                                         1ULL << 40,
 *                                                         &ssd_ctx);
 *   ssd_vtbl->get(ssd_ctx, gpa, len, buf);
 *   ...
 *   ssd_transport_destroy(ssd_ctx);  // also frees vtbl
 * ======================================================================== */

#ifndef TRANSPORT_SSD_H
#define TRANSPORT_SSD_H

#include "../../include/umm.h"
#include "transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ssd_transport_create — Create an SSD transport backend.
 *
 * @param base_dir   Directory for chunk files (created if not exists).
 * @param max_bytes  Maximum total capacity in bytes.
 * @param out_ctx    Receives the transport context (for vtbl->get(ctx, ...)).
 * @return           MemoryTransportVtbl with SSD-backed implementations,
 *                   or NULL on error.
 */
MemoryTransportVtbl* ssd_transport_create(const char *base_dir,
                                           uint64_t max_bytes,
                                           void **out_ctx);

/**
 * ssd_transport_create_multi — Create an SSD transport over N devices.
 *
 * 多设备共享池场景（如 QEMU 共享块设备实验）：pool 虚拟偏移空间按
 * 设备数组顺序拼接（与 umms 侧 cfg.ssd_devices 顺序必须一致），
 * 任一设备注册失败即整体失败并返回 NULL（显式暴露配置错误，
 * 不像单设备版那样留下空池）。
 *
 * @param devs      设备数组（path + size）
 * @param num_devs  设备数（>0）
 * @param out_ctx   Receives the transport context.
 * @return          MemoryTransportVtbl, or NULL on error.
 */
MemoryTransportVtbl* ssd_transport_create_multi(const SsdDeviceConfig *devs,
                                                 uint32_t num_devs,
                                                 void **out_ctx);

/**
 * ssd_transport_destroy — Destroy SSD transport and free all resources.
 *
 * This frees both the context and the vtable returned by ssd_transport_create.
 * Do NOT call vtbl->deinit(ctx) directly — use this function instead.
 */
void ssd_transport_destroy(void *ctx);

/**
 * ssd_transport_invalidate — 丢弃 SSD 数据面的缓存页视图
 * （msync MS_INVALIDATE，跨设备分段）。
 *
 * 共享盘读共享场景：对端节点 umm_write + umm_fence 落盘后，本端须先
 * invalidate 再读，否则 mmap 页缓存返回旧数据。
 *
 * @param ctx   SSD transport 上下文。
 * @param gpa   目标 GPA（取 offset 位定位 pool 虚拟空间）。
 * @param len   失效长度（字节）。
 */
int ssd_transport_invalidate(void *ctx, gpa_t gpa, uint64_t len);

#ifdef __cplusplus
}
#endif

#endif /* TRANSPORT_SSD_H */
