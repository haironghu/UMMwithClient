/* ========================================================================
 * ssd_pool.h — Multi-device SSD pool with contiguous virtual address space
 *
 * Presents multiple physical SSD devices as a single contiguous virtual
 * address space. Allocation returns virtual offsets; the pool internally
 * maps them to (device, physical_offset) pairs.
 *
 * Virtual address space layout (example with 3 devices):
 *   virtual [0, 256GB)      → device_0  physical [0, 256GB)
 *   virtual [256GB, 512GB)  → device_1  physical [0, 256GB)
 *   virtual [512GB, 1TB)    → device_2  physical [0, 512GB)
 *
 * Lifecycle:
 *   ssd_pool_create()                          ← create empty pool
 *   ssd_pool_add_device(pool, "/dev/sda", cap)  ← add physical device
 *   ssd_pool_add_device(pool, "/dev/sdb", cap)  ← add another
 *   ssd_pool_alloc(pool, size, &voff)          ← alloc from virtual space
 *   ssd_pool_translate(pool, voff, &dev, &poff) ← virtual → physical
 *   ssd_pool_get_ptr(pool, voff)               ← direct pointer (mmap)
 *   ssd_pool_sync(pool, voff, size)            ← msync
 *   ssd_pool_free(pool, voff, size)            ← free virtual range
 *   ssd_pool_destroy(pool)                     ← cleanup
 * ======================================================================== */

#ifndef SSD_POOL_H
#define SSD_POOL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SsdPool SsdPool;

/* ========================================================================
 * SsdBackend (formerly in ssd_backend.h)
 * ======================================================================== */

typedef struct SsdBackend SsdBackend;

SsdBackend* ssd_backend_create(const char *device_path, uint64_t capacity);
void        ssd_backend_destroy(SsdBackend *sb);
int         ssd_backend_alloc(SsdBackend *sb, uint64_t size, uint64_t *out_offset);
void        ssd_backend_free(SsdBackend *sb, uint64_t offset, uint64_t size);
void*       ssd_backend_get_ptr(SsdBackend *sb, uint64_t offset);
int         ssd_backend_sync(SsdBackend *sb, uint64_t offset, uint64_t size);
int         ssd_backend_recover(SsdBackend *sb);

/* ------------------------------------------------------------------------
 * 后端类型（ssd_backend_create 按路径自动分派）：
 *   - 常规文件（*.raw 等）：ftruncate + mmap 模拟盘（测试/开发）
 *   - libnvm:<ctrl>@<ns>：libnvm 用户态 NVMe 驱动（生产路径）
 *
 * 注意：不支持内核块设备（/dev/nvmeXnY）直写——避免误写系统盘/数据盘
 * 的风险；真实硬件必须经过 libnvm 接管后接入。
 * ---------------------------------------------------------------------- */

uint64_t ssd_backend_capacity(const SsdBackend *sb);

/* 单次 I/O 上限（字节）：0 = 不限制（文件后端）；
 * libnvm 后端为 disk_info.max_data_size（并发引擎按此做分段/队列调优） */
uint64_t ssd_backend_max_io(const SsdBackend *sb);

/** 主机侧 I/O（pread/pwrite）。文件后端与块设备后端均可用；
 *  块设备后端无 mmap，这是其唯一的主机数据通路。 */
int      ssd_backend_pread (SsdBackend *sb, uint64_t offset, uint64_t len, void *buf);
int      ssd_backend_pwrite(SsdBackend *sb, uint64_t offset, uint64_t len, const void *buf);

/** ssd_pool_create — Create an empty SSD pool. */
SsdPool* ssd_pool_create(void);

/** ssd_pool_destroy — Destroy pool and all underlying devices. */
void ssd_pool_destroy(SsdPool *pool);

/**
 * ssd_pool_add_device — Add a physical SSD device to the pool.
 *
 * @param pool        SSD pool.
 * @param device_path Path to device file (e.g., "/dev/sda" or "/data/ssd0.raw").
 * @param capacity    Device capacity in bytes (page-aligned).
 * @return            UMM_OK on success, error code on failure.
 *
 * Devices are appended in order. Their virtual address ranges are
 * contiguous: device N starts at the end of device N-1.
 */
int ssd_pool_add_device(SsdPool *pool, const char *device_path, uint64_t capacity);

/** Get total virtual capacity of all devices combined. */
uint64_t ssd_pool_total_capacity(const SsdPool *pool);

/** Get number of devices in the pool. */
uint32_t ssd_pool_num_devices(const SsdPool *pool);

/**
 * ssd_pool_alloc — Allocate space from the virtual address pool.
 *
 * @param pool         SSD pool.
 * @param size         Number of bytes (page-aligned).
 * @param out_voffset  Receives virtual offset within the pool.
 * @return             UMM_OK on success.
 */
int ssd_pool_alloc(SsdPool *pool, uint64_t size, uint64_t *out_voffset);

/** ssd_pool_free — Free a virtual range. */
void ssd_pool_free(SsdPool *pool, uint64_t voffset, uint64_t size);

/**
 * ssd_pool_translate — Translate virtual offset to (device, physical_offset).
 *
 * @param pool      SSD pool.
 * @param voffset   Virtual offset (from ssd_pool_alloc).
 * @param out_dev   Receives device index (0-based).
 * @param out_poff  Receives physical offset within that device.
 * @return          UMM_OK on success, UMM_E_INVALID_ARG if out of range.
 */
int ssd_pool_translate(SsdPool *pool, uint64_t voffset,
                        uint32_t *out_dev, uint64_t *out_poff);

/**
 * ssd_pool_get_ptr — Get direct memory pointer for a virtual offset.
 *
 * Internally translates voffset → (device, poffset) → mmap_base + poffset.
 * 块设备后端无 mmap，返回 NULL（调用方应改用 ssd_pool_pread/pwrite）。
 */
void* ssd_pool_get_ptr(SsdPool *pool, uint64_t voffset);

/** ssd_pool_sync — msync a virtual range to disk. */
int ssd_pool_sync(SsdPool *pool, uint64_t voffset, uint64_t size);

/**
 * 池级主机 I/O：按虚拟偏移 pread/pwrite，自动跨设备分段。
 * 用于块设备后端（无 mmap）的 CPU 数据通路。
 */
int ssd_pool_pread (SsdPool *pool, uint64_t voffset, uint64_t len, void *buf);
int ssd_pool_pwrite(SsdPool *pool, uint64_t voffset, uint64_t len, const void *buf);

#ifdef __cplusplus
}
#endif

#endif /* SSD_POOL_H */
