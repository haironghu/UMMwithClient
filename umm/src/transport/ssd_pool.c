/* ========================================================================
 * ssd_pool.c — Multi-device SSD pool with contiguous virtual address space
 *
 * Maps N physical SSD devices into a single contiguous virtual address space.
 * Each device has its own SsdBackend (bitmap allocator + mmap).
 * The pool adds a global virtual bitmap + virtual→physical translation layer.
 * ======================================================================== */

#include "ssd_pool.h"
#include "../common/error_codes.h"
#include "../common/log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>           /* BLKGETSIZE64 */
#include <pthread.h>
#include <errno.h>

/* ========================================================================
 * SSD Backend implementation (merged from ssd_backend.c)
 * ======================================================================== */

#define SSD_PAGE_SIZE   4096
#define SSD_BITS_PER_U64 64

#include "ssd_backend_libnvm.h"
#include "ssd_backend_nds.h"

struct SsdBackend {
    char       *device_path;    /* path to device file */
    int         fd;             /* device file descriptor */
    void       *mmap_base;      /* mmap base address (文件后端; libnvm/nds 为 MAP_FAILED) */
    uint64_t    capacity;       /* total capacity (bytes) */
    uint64_t    total_pages;    /* capacity / page_size */
    uint64_t    free_pages;     /* number of free pages */
    uint64_t   *bitmap;         /* allocation bitmap */
    uint64_t    bitmap_words;   /* number of uint64_t in bitmap */
    int         is_libnvm;      /* 1 = libnvm userspace NVMe backend */
    SsdLibnvmBackend *libnvm;   /* libnvm handle (is_libnvm only) */
    int         is_nds;         /* 1 = NDS NPU 直驱后端 */
    SsdNdsBackend *nds;         /* nds handle (is_nds only) */
    int         is_nds_meta;    /* 1 = nds-meta 纯分配后端（无后端资源） */
    pthread_mutex_t lock;
};

/* ------------------------------------------------------------------------ */
/* Bitmap helpers                                                           */
/* ------------------------------------------------------------------------ */

static inline int bitmap_test(uint64_t *bm, uint64_t page)
{
    return (bm[page / SSD_BITS_PER_U64] >> (page % SSD_BITS_PER_U64)) & 1ULL;
}

static inline void bitmap_set(uint64_t *bm, uint64_t page)
{
    bm[page / SSD_BITS_PER_U64] |= (1ULL << (page % SSD_BITS_PER_U64));
}

static inline void bitmap_clear(uint64_t *bm, uint64_t page)
{
    bm[page / SSD_BITS_PER_U64] &= ~(1ULL << (page % SSD_BITS_PER_U64));
}

/**
 * Find 'npages' consecutive free pages starting from page 'start'.
 * Returns the starting page index, or total_pages (not found).
 */
static uint64_t bitmap_find_free(uint64_t *bm, uint64_t total_pages,
                                  uint64_t start, uint64_t npages)
{
    if (npages == 0 || start >= total_pages)
        return total_pages;

    uint64_t found = start;
    uint64_t consecutive = 0;

    for (uint64_t p = start; p < total_pages; p++) {
        if (!bitmap_test(bm, p)) {
            if (consecutive == 0)
                found = p;
            consecutive++;
            if (consecutive >= npages)
                return found;
        } else {
            consecutive = 0;
        }
    }
    return total_pages; /* not found */
}

/* ------------------------------------------------------------------------ */
/* ssd_backend_create                                                       */
/* ------------------------------------------------------------------------ */

SsdBackend* ssd_backend_create(const char *device_path, uint64_t capacity)
{
    if (!device_path)
        return NULL;

    if (capacity == 0) {
        umm_log_error(
                      "ssd_backend: explicit capacity required (path=%s)",
                      device_path);
        return NULL;
    }

    SsdBackend *sb = calloc(1, sizeof(SsdBackend));
    if (!sb)
        return NULL;

    sb->device_path = strdup(device_path);
    sb->fd          = -1;
    sb->mmap_base   = MAP_FAILED;
    pthread_mutex_init(&sb->lock, NULL);

    /* ---- libnvm 路径："libnvm:<ctrl>@<ns>"，用户态 NVMe 库驱动 ---- */
    if (strncmp(device_path, "libnvm:", 7) == 0) {
        if (capacity == 0) {
            /* libnvm disk_info 暂无总容量字段，必须显式配置 */
            umm_log_error(
                          "ssd_backend: libnvm backend requires explicit "
                          "capacity (path=%s)", device_path);
            goto fail;
        }
        capacity &= ~(uint64_t)(SSD_PAGE_SIZE - 1);
        if (ssd_libnvm_open(device_path + 7, &sb->libnvm) != UMM_OK)
            goto fail;
        sb->is_libnvm   = 1;
        sb->capacity    = capacity;
        sb->total_pages = capacity / SSD_PAGE_SIZE;
        sb->free_pages  = sb->total_pages;
        umm_log_info(
                     "ssd_backend: libnvm device=%s, managed=%lu MB",
                     device_path,
                     (unsigned long)(capacity / 1024 / 1024));
        goto init_bitmap;
    }

    /* ---- nds-meta 路径："nds-meta:<device_id>[+<base_off>]"，纯分配后端 ----
     * 部署形态（NPU2SSD，解决 umms 与 worker 双进程同抢一个 NPU 设备 /
     * RPC server 单客户端串行导致的 nds_init 卡死）：
     *   umms:   ssd_devices: "nds-meta:0+0x40000000:16G"  ← 纯分配簿记
     *   worker: spec "nds:0+0x40000000"（同窗口同容量）    ← 唯一 NDS 客户端
     * umms 只做 bitmap 分配簿记，不 dlopen/不 nds_init/不占 RPC 连接/
     * 不 mmap（因此无需 UMM_NDS_PATH/UMM_NDS_PRELOAD/UMM_NDS_RPC_SOCKET）；
     * 数据面 I/O 一律 UMM_E_UNSUPPORTED（见下方各接口分支）。
     * 注意 strncmp(path,"nds:",4) 不会误匹配 "nds-meta:"（第 4 字符是
     * '-' 不是 ':'），但为可读性仍将本分支放在 nds 之前。
     * spec 语义与 nds 完全一致（device_id/base_off/显式容量/页对齐
     * 截断），仅校验格式不打开设备；bitmap 分配簿记与 nds 一致
     * （走 init_bitmap 建池内 bitmap）。 */
    if (strncmp(device_path, "nds-meta:", 9) == 0) {
        const char *spec = device_path + 9;
        /* 轻量格式校验（复用 nds 的 spec 形态：<id>[+<off>]），
         * 不解析保存——簿记只需要容量 */
        char spec_buf[64];
        if (strlen(spec) >= sizeof(spec_buf))
            goto fail;
        strcpy(spec_buf, spec);
        char *plus = strrchr(spec_buf, '+');
        if (plus)
            *plus = '\0';
        char *end = NULL;
        (void)strtoul(spec_buf, &end, 10);
        if (spec_buf[0] == '\0' || end == spec_buf || *end != '\0') {
            umm_log_error(
                          "ssd_backend: nds-meta spec 非法（须为 "
                          "<device_id>[+<base_off>]）：%s", spec);
            goto fail;
        }
        capacity &= ~(uint64_t)(SSD_PAGE_SIZE - 1);
        sb->is_nds_meta = 1;
        sb->capacity    = capacity;
        sb->total_pages = capacity / SSD_PAGE_SIZE;
        sb->free_pages  = sb->total_pages;
        umm_log_info(
                     "ssd_backend: nds-meta 纯分配后端 device=%s, "
                     "managed=%lu MB（不 nds_init，数据面在 worker）",
                     device_path,
                     (unsigned long)(capacity / 1024 / 1024));
        goto init_bitmap;
    }

    /* ---- nds 路径："nds:<device_id>[+<base_off>]"，NDS NPU 直驱后端 ----
     * 无 mmap：数据通路为 NPU device 内存（pread/pwrite 的 buf 语义为
     * device 虚拟地址），I/O 前必须 ssd_backend_register_dev_mem 注册；
     * NDS 无容量查询接口，容量必须显式（顶部已统一拒绝 capacity=0） */
    if (strncmp(device_path, "nds:", 4) == 0) {
        capacity &= ~(uint64_t)(SSD_PAGE_SIZE - 1);
        if (ssd_nds_open(device_path + 4, &sb->nds) != UMM_OK)
            goto fail;
        sb->is_nds      = 1;
        sb->capacity    = capacity;
        sb->total_pages = capacity / SSD_PAGE_SIZE;
        sb->free_pages  = sb->total_pages;
        umm_log_info(
                     "ssd_backend: nds device=%s, managed=%lu MB",
                     device_path,
                     (unsigned long)(capacity / 1024 / 1024));
        goto init_bitmap;
    }

    /* ---- 文件路径（原有模拟逻辑）---- */
    /* Align capacity to page boundary */
    capacity = (capacity + SSD_PAGE_SIZE - 1) & ~(uint64_t)(SSD_PAGE_SIZE - 1);

    sb->capacity    = capacity;
    sb->total_pages = capacity / SSD_PAGE_SIZE;
    sb->free_pages  = sb->total_pages;

    /* Auto-create parent directory if needed */
    char *dir = strdup(device_path);
    char *last_slash = strrchr(dir, '/');
    if (last_slash && last_slash != dir) {
        *last_slash = '\0';
        mkdir(dir, 0755);  /* ignore errors (may already exist) */
    }
    free(dir);

    /* Create or open the device file */
    sb->fd = open(device_path, O_RDWR | O_CREAT, 0644);
    if (sb->fd < 0) {
        umm_log_error(
                      "ssd_backend: open(%s) failed: %s",
                      device_path, strerror(errno));
        goto fail;
    }

    /* 块设备（如 QEMU/SAN 共享的 /dev/nvme0n1）：不能 ftruncate，
     * 直接 ioctl 取设备容量并要求 >= 配置窗口；普通文件照旧 sparse。 */
    struct stat st;
    int         is_blk = (fstat(sb->fd, &st) == 0) && S_ISBLK(st.st_mode);
    if (is_blk) {
        uint64_t blk_bytes = 0;
        if (ioctl(sb->fd, BLKGETSIZE64, &blk_bytes) != 0) {
            umm_log_error(
                          "ssd_backend: BLKGETSIZE64(%s) failed: %s",
                          device_path, strerror(errno));
            goto fail;
        }
        if (blk_bytes < capacity) {
            umm_log_error(
                          "ssd_backend: block device %s too small: "
                          "%lu < %lu bytes",
                          device_path, (unsigned long)blk_bytes,
                          (unsigned long)capacity);
            goto fail;
        }
        umm_log_info(
                     "ssd_backend: block device=%s, size=%lu MB, "
                     "window=%lu MB (no ftruncate)",
                     device_path,
                     (unsigned long)(blk_bytes / (1024 * 1024)),
                     (unsigned long)(capacity / (1024 * 1024)));
    } else if (ftruncate(sb->fd, (off_t)capacity) != 0) {
        umm_log_error(
                      "ssd_backend: ftruncate(%s, %lu) failed: %s",
                      device_path, (unsigned long)capacity, strerror(errno));
        goto fail;
    }

    /* mmap the entire device */
    sb->mmap_base = mmap(NULL, (size_t)capacity, PROT_READ | PROT_WRITE,
                         MAP_SHARED, sb->fd, 0);
    if (sb->mmap_base == MAP_FAILED) {
        umm_log_error(
                      "ssd_backend: mmap(%s, %lu) failed: %s",
                      device_path, (unsigned long)capacity, strerror(errno));
        goto fail;
    }

    umm_log_info(
                 "ssd_backend: created file device=%s, capacity=%lu MB, "
                 "pages=%lu, mmap=%p",
                 device_path,
                 (unsigned long)(capacity / (1024 * 1024)),
                 (unsigned long)sb->total_pages,
                 sb->mmap_base);

init_bitmap:
    /* Allocate bitmap */
    sb->bitmap_words = (sb->total_pages + SSD_BITS_PER_U64 - 1)
                       / SSD_BITS_PER_U64;
    sb->bitmap = calloc(sb->bitmap_words, sizeof(uint64_t));
    if (!sb->bitmap) {
        umm_log_error(
                      "ssd_backend: bitmap alloc failed");
        goto fail;
    }
    return sb;

fail:
    umm_log_error(
                  "ssd_backend_create FAILED: errno=%d (%s), device=%s, "
                  "capacity=%lu",
                  errno, strerror(errno),
                  device_path, (unsigned long)capacity);
    if (sb->mmap_base != MAP_FAILED && sb->mmap_base != NULL)
        munmap(sb->mmap_base, (size_t)sb->capacity);
    if (sb->fd >= 0)
        close(sb->fd);
    free(sb->bitmap);
    free(sb->device_path);
    free(sb);
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* ssd_backend_destroy                                                      */
/* ------------------------------------------------------------------------ */

void ssd_backend_destroy(SsdBackend *sb)
{
    if (!sb)
        return;

    pthread_mutex_lock(&sb->lock);

    if (sb->mmap_base != MAP_FAILED && sb->mmap_base != NULL) {
        msync(sb->mmap_base, (size_t)sb->capacity, MS_SYNC);
        munmap(sb->mmap_base, (size_t)sb->capacity);
    }
    if (sb->fd >= 0)
        close(sb->fd);
    if (sb->libnvm)
        ssd_libnvm_close(sb->libnvm);
    if (sb->nds)
        ssd_nds_close(sb->nds);

    umm_log_info(
                 "ssd_backend: destroyed device=%s, used=%lu/%lu pages",
                 sb->device_path ? sb->device_path : "?",
                 (unsigned long)(sb->total_pages - sb->free_pages),
                 (unsigned long)sb->total_pages);

    pthread_mutex_unlock(&sb->lock);
    pthread_mutex_destroy(&sb->lock);

    free(sb->bitmap);
    free(sb->device_path);
    free(sb);
}

/* ------------------------------------------------------------------------ */
/* ssd_backend_alloc — bitmap allocation                                    */
/* ------------------------------------------------------------------------ */

int ssd_backend_alloc(SsdBackend *sb, uint64_t size, uint64_t *out_offset)
{
    if (!sb || !out_offset || size == 0)
        return UMM_E_INVALID_ARG;

    uint64_t npages = (size + SSD_PAGE_SIZE - 1) / SSD_PAGE_SIZE;

    pthread_mutex_lock(&sb->lock);

    if (sb->free_pages < npages) {
        pthread_mutex_unlock(&sb->lock);
        umm_log_error(
                      "ssd_backend: out of space (need %lu pages, "
                      "free %lu)",
                      (unsigned long)npages,
                      (unsigned long)sb->free_pages);
        return UMM_E_NO_MEMORY;
    }

    uint64_t page = bitmap_find_free(sb->bitmap, sb->total_pages, 0, npages);
    if (page >= sb->total_pages) {
        pthread_mutex_unlock(&sb->lock);
        umm_log_error(
                      "ssd_backend: no contiguous %lu-page region",
                      (unsigned long)npages);
        return UMM_E_NO_MEMORY;
    }

    /* Mark pages as allocated */
    for (uint64_t i = 0; i < npages; i++)
        bitmap_set(sb->bitmap, page + i);

    sb->free_pages -= npages;
    *out_offset = page * SSD_PAGE_SIZE;

    pthread_mutex_unlock(&sb->lock);

    umm_log_debug(
                  "ssd_backend: allocated %lu pages at offset %lu "
                  "(free=%lu/%lu)",
                  (unsigned long)npages, (unsigned long)*out_offset,
                  (unsigned long)sb->free_pages,
                  (unsigned long)sb->total_pages);
    return UMM_OK;
}

/* ------------------------------------------------------------------------ */
/* ssd_backend_free                                                         */
/* ------------------------------------------------------------------------ */

void ssd_backend_free(SsdBackend *sb, uint64_t offset, uint64_t size)
{
    if (!sb || size == 0 || offset >= sb->capacity)
        return;

    uint64_t start_page = offset / SSD_PAGE_SIZE;
    uint64_t npages = (size + SSD_PAGE_SIZE - 1) / SSD_PAGE_SIZE;

    /* Clamp to device bounds */
    if (start_page + npages > sb->total_pages)
        npages = sb->total_pages - start_page;

    pthread_mutex_lock(&sb->lock);

    for (uint64_t i = 0; i < npages; i++) {
        if (bitmap_test(sb->bitmap, start_page + i)) {
            bitmap_clear(sb->bitmap, start_page + i);
            sb->free_pages++;
        }
    }

    pthread_mutex_unlock(&sb->lock);

    umm_log_debug(
                  "ssd_backend: freed %lu pages at offset %lu "
                  "(free=%lu/%lu)",
                  (unsigned long)npages, (unsigned long)offset,
                  (unsigned long)sb->free_pages,
                  (unsigned long)sb->total_pages);
}

/* ------------------------------------------------------------------------ */
/* ssd_backend_get_ptr                                                      */
/* ------------------------------------------------------------------------ */

void* ssd_backend_get_ptr(SsdBackend *sb, uint64_t offset)
{
    if (!sb || offset >= sb->capacity || sb->mmap_base == MAP_FAILED)
        return NULL;

    /* Pure I/O: no bitmap check. Transport layer is a dumb executor.
     * Allocation validation happens at the alloc layer (umms bitmap).
     * After free, the pointer is still valid (mmap is intact) but
     * should not be used — that's a user-level bug. */
    return (uint8_t *)sb->mmap_base + offset;
}

/* ------------------------------------------------------------------------ */
/* ssd_backend_sync                                                         */
/* ------------------------------------------------------------------------ */

int ssd_backend_sync(SsdBackend *sb, uint64_t offset, uint64_t size)
{
    if (!sb || sb->mmap_base == MAP_FAILED)
        return UMM_E_INVALID_ARG;

    if (offset + size > sb->capacity)
        size = sb->capacity - offset;

    int rc = msync((uint8_t *)sb->mmap_base + offset, (size_t)size, MS_SYNC);
    if (rc != 0) {
        umm_log_error(
                      "ssd_backend: msync failed: %s", strerror(errno));
        return UMM_E_TRANSPORT_ERROR;
    }
    return UMM_OK;
}

/* ------------------------------------------------------------------------ */
/* ssd_backend_invalidate — 丢弃本进程视角的缓存页（msync MS_INVALIDATE），   */
/* 使后续读重新从设备取数。共享盘读共享场景：另一节点写完落盘后，本节点必须   */
/* 先 invalidate 才能看到新数据（mmap 页缓存不会自动感知外部写入）。          */
/* 注意：会丢弃本映射的脏页——调用方应确保本区域无未落盘的写。                 */
/* ------------------------------------------------------------------------ */

int ssd_backend_invalidate(SsdBackend *sb, uint64_t offset, uint64_t size)
{
    if (!sb || sb->mmap_base == MAP_FAILED)
        return UMM_E_INVALID_ARG;

    if (offset + size > sb->capacity)
        size = sb->capacity - offset;

    int rc = msync((uint8_t *)sb->mmap_base + offset, (size_t)size,
                   MS_INVALIDATE);
    if (rc != 0) {
        umm_log_error(
                      "ssd_backend: msync(MS_INVALIDATE) failed: %s",
                      strerror(errno));
        return UMM_E_TRANSPORT_ERROR;
    }
    return UMM_OK;
}

/* ------------------------------------------------------------------------ */
/* 块设备辅助接口与主机侧 I/O（pread/pwrite）                                  */
/* ------------------------------------------------------------------------ */

uint64_t ssd_backend_capacity(const SsdBackend *sb)
{
    return sb ? sb->capacity : 0;
}

uint64_t ssd_backend_max_io(const SsdBackend *sb)
{
    if (!sb)
        return 0;
    /* 文件后端无单次上限（pread/pwrite 全量循环）；
     * libnvm 为 disk_info 值；nds 为 page_size*max_page_num */
    if (sb->is_libnvm)
        return ssd_libnvm_max_io(sb->libnvm);
    if (sb->is_nds)
        return ssd_nds_max_io(sb->nds);
    /* nds-meta 纯分配后端无 IO 概念，回落默认 0 */
    return 0;
}

/* 全量 pread：处理短读与 EINTR，直到读满 len 或出错 */
int ssd_backend_pread(SsdBackend *sb, uint64_t offset, uint64_t len, void *buf)
{
    if (!sb || (!buf && len > 0))
        return UMM_E_INVALID_ARG;
    if (offset + len > sb->capacity)
        return UMM_E_INVALID_ARG;  /* 越界 */

    if (sb->is_nds_meta) {
        umm_log_error(
                      "ssd_backend: nds-meta 为纯分配后端，pread 不受理——"
                      "数据面在 worker 进程（nds: 后端）");
        return UMM_E_UNSUPPORTED;
    }
    if (sb->is_libnvm)
        return ssd_libnvm_read(sb->libnvm, offset, len, buf);
    if (sb->is_nds)
        /* nds 后端：buf 语义为已注册的 NPU device 虚拟地址 */
        return ssd_nds_read(sb->nds, offset, len, buf);

    uint8_t *p = (uint8_t *)buf;
    uint64_t done = 0;
    while (done < len) {
        ssize_t n = pread(sb->fd, p + done, (size_t)(len - done),
                          (off_t)(offset + done));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            umm_log_error(
                          "ssd_backend: pread(%s, off=%lu) failed: %s",
                          sb->device_path, (unsigned long)(offset + done),
                          strerror(errno));
            return UMM_E_IO;
        }
        if (n == 0)
            return UMM_E_IO;            /* EOF：不应发生（已做边界检查） */
        done += (uint64_t)n;
    }
    return UMM_OK;
}

/* 全量 pwrite：处理短写与 EINTR */
int ssd_backend_pwrite(SsdBackend *sb, uint64_t offset, uint64_t len,
                       const void *buf)
{
    if (!sb || (!buf && len > 0))
        return UMM_E_INVALID_ARG;
    if (offset + len > sb->capacity)
        return UMM_E_INVALID_ARG;  /* 越界 */

    if (sb->is_nds_meta) {
        umm_log_error(
                      "ssd_backend: nds-meta 为纯分配后端，pwrite 不受理——"
                      "数据面在 worker 进程（nds: 后端）");
        return UMM_E_UNSUPPORTED;
    }
    if (sb->is_libnvm)
        return ssd_libnvm_write(sb->libnvm, offset, len, buf);
    if (sb->is_nds)
        /* nds 后端：buf 语义为已注册的 NPU device 虚拟地址 */
        return ssd_nds_write(sb->nds, offset, len, buf);

    const uint8_t *p = (const uint8_t *)buf;
    uint64_t done = 0;
    while (done < len) {
        ssize_t n = pwrite(sb->fd, p + done, (size_t)(len - done),
                           (off_t)(offset + done));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            umm_log_error(
                          "ssd_backend: pwrite(%s, off=%lu) failed: %s",
                          sb->device_path, (unsigned long)(offset + done),
                          strerror(errno));
            return UMM_E_IO;
        }
        done += (uint64_t)n;
    }
    return UMM_OK;
}

/* ------------------------------------------------------------------------ */
/* device 内存注册与后端级批量 I/O（nds 后端；文件后端批量为模拟通路）        */
/* ------------------------------------------------------------------------ */

int ssd_backend_register_dev_mem(SsdBackend *sb, void *dev_mem,
                                 uint64_t aligned_size)
{
    if (!sb || !dev_mem || aligned_size == 0)
        return UMM_E_INVALID_ARG;
    if (sb->is_nds_meta) {
        umm_log_error(
                      "ssd_backend: nds-meta 为纯分配后端，register_dev_mem "
                      "不受理——数据面在 worker 进程（nds: 后端）");
        return UMM_E_UNSUPPORTED;
    }
    if (!sb->is_nds) {
        umm_log_error(
                      "ssd_backend: register_dev_mem 仅 nds 后端支持 "
                      "(device=%s)", sb->device_path);
        return UMM_E_INVALID_ARG;
    }
    return ssd_nds_register_mem(sb->nds, dev_mem, aligned_size);
}

/* 后端级 batch 逐 iov 越界校验（对全部后端统一前置，文件后端双保险
 * ——其模拟通路内的 pread/pwrite 还会再查一次）。
 * 防溢出写法：不做 offset+length 加法，避免回绕绕过校验 */
static int ssd_backend_batch_check_bounds(SsdBackend *sb,
                                          const UmmNdsIOVec *iovs,
                                          size_t n_iov,
                                          const char *what)
{
    for (size_t i = 0; i < n_iov; i++) {
        if (iovs[i].length > sb->capacity ||
            iovs[i].offset > sb->capacity - iovs[i].length) {
            umm_log_error(
                          "ssd_backend: %s iov[%zu] 越界 "
                          "(offset=%lu, length=%lu, capacity=%lu)",
                          what, i,
                          (unsigned long)iovs[i].offset,
                          (unsigned long)iovs[i].length,
                          (unsigned long)sb->capacity);
            return UMM_E_INVALID_ARG;
        }
    }
    return UMM_OK;
}

int ssd_backend_batch_read(SsdBackend *sb, UmmNdsIOVec *iovs, size_t n_iov)
{
    if (!sb || (!iovs && n_iov > 0))
        return UMM_E_INVALID_ARG;
    int rc = ssd_backend_batch_check_bounds(sb, iovs, n_iov,
                                            "batch_read");
    if (rc != UMM_OK)
        return rc;
    if (sb->is_nds_meta) {
        umm_log_error(
                      "ssd_backend: nds-meta 为纯分配后端，batch_read "
                      "不受理——数据面在 worker 进程（nds: 后端）");
        return UMM_E_UNSUPPORTED;
    }
    if (sb->is_nds)
        return ssd_nds_batch_read(sb->nds, iovs, n_iov);
    /* 文件后端：pread 逐 iov 循环模拟（vaddr 为 host buffer），
     * 便于无 NPU 环境测试池级批量通路 */
    for (size_t i = 0; i < n_iov; i++) {
        rc = ssd_backend_pread(sb, iovs[i].offset, iovs[i].length,
                               iovs[i].vaddr);
        if (rc != UMM_OK)
            return rc;
    }
    return UMM_OK;
}

int ssd_backend_batch_write(SsdBackend *sb, const UmmNdsIOVec *iovs,
                            size_t n_iov)
{
    if (!sb || (!iovs && n_iov > 0))
        return UMM_E_INVALID_ARG;
    int rc = ssd_backend_batch_check_bounds(sb, iovs, n_iov,
                                            "batch_write");
    if (rc != UMM_OK)
        return rc;
    if (sb->is_nds_meta) {
        umm_log_error(
                      "ssd_backend: nds-meta 为纯分配后端，batch_write "
                      "不受理——数据面在 worker 进程（nds: 后端）");
        return UMM_E_UNSUPPORTED;
    }
    if (sb->is_nds)
        return ssd_nds_batch_write(sb->nds, iovs, n_iov);
    /* 文件后端：pwrite 逐 iov 循环模拟 */
    for (size_t i = 0; i < n_iov; i++) {
        int rc = ssd_backend_pwrite(sb, iovs[i].offset, iovs[i].length,
                                    iovs[i].vaddr);
        if (rc != UMM_OK)
            return rc;
    }
    return UMM_OK;
}

/* ------------------------------------------------------------------------ */
/* ssd_backend_recover                                                      */
/* ------------------------------------------------------------------------ */

int ssd_backend_recover(SsdBackend *sb)
{
    if (!sb)
        return UMM_E_INVALID_ARG;

    /* For now: clear all allocations (fresh start).
     * In production: scan journal/headers to rebuild bitmap. */
    pthread_mutex_lock(&sb->lock);

    memset(sb->bitmap, 0, sb->bitmap_words * sizeof(uint64_t));
    sb->free_pages = sb->total_pages;

    /* Optional: fsync the underlying file */
    if (sb->fd >= 0)
        fsync(sb->fd);

    pthread_mutex_unlock(&sb->lock);

    umm_log_info(
                 "ssd_backend: recovered device=%s, all %lu pages free",
                 sb->device_path ? sb->device_path : "?",
                 (unsigned long)sb->total_pages);
    return UMM_OK;
}


/* ========================================================================
 * SSD Pool implementation
 * ======================================================================== */

#define SSD_POOL_PAGE_SIZE   4096
#define SSD_POOL_BITS_PER_U64 64
#define SSD_POOL_MAX_DEVICES  16

typedef struct {
    SsdBackend *backend;        /* underlying ssd_backend */
    uint64_t    virtual_base;   /* virtual offset where this device starts */
    uint64_t    capacity;       /* device capacity in bytes */
} SsdDevice;

struct SsdPool {
    SsdDevice   devices[SSD_POOL_MAX_DEVICES];
    uint32_t    num_devices;
    uint64_t    total_capacity; /* sum of all device capacities */
    uint64_t    total_pages;    /* total_capacity / page_size */
    uint64_t    free_pages;
    uint64_t   *bitmap;         /* global virtual allocation bitmap */
    uint64_t    bitmap_words;
    pthread_mutex_t lock;
};

/* ------------------------------------------------------------------------ */
/* Bitmap helpers (same logic as ssd_backend)                               */
/* ------------------------------------------------------------------------ */

static inline int bm_test(uint64_t *bm, uint64_t page)
{
    return (bm[page / SSD_POOL_BITS_PER_U64] >> (page % SSD_POOL_BITS_PER_U64)) & 1ULL;
}

static inline void bm_set(uint64_t *bm, uint64_t page)
{
    bm[page / SSD_POOL_BITS_PER_U64] |= (1ULL << (page % SSD_POOL_BITS_PER_U64));
}

static inline void bm_clear(uint64_t *bm, uint64_t page)
{
    bm[page / SSD_POOL_BITS_PER_U64] &= ~(1ULL << (page % SSD_POOL_BITS_PER_U64));
}

static uint64_t bm_find_free(uint64_t *bm, uint64_t total_pages,
                              uint64_t start, uint64_t npages)
{
    if (npages == 0 || start >= total_pages)
        return total_pages;

    uint64_t found = start;
    uint64_t consecutive = 0;

    for (uint64_t p = start; p < total_pages; p++) {
        if (!bm_test(bm, p)) {
            if (consecutive == 0)
                found = p;
            consecutive++;
            if (consecutive >= npages)
                return found;
        } else {
            consecutive = 0;
        }
    }
    return total_pages;
}

/* ------------------------------------------------------------------------ */
/* ssd_pool_create / destroy                                                */
/* ------------------------------------------------------------------------ */

SsdPool* ssd_pool_create(void)
{
    SsdPool *pool = calloc(1, sizeof(SsdPool));
    if (!pool)
        return NULL;
    pthread_mutex_init(&pool->lock, NULL);
    return pool;
}

void ssd_pool_destroy(SsdPool *pool)
{
    if (!pool)
        return;

    pthread_mutex_lock(&pool->lock);

    for (uint32_t i = 0; i < pool->num_devices; i++) {
        if (pool->devices[i].backend)
            ssd_backend_destroy(pool->devices[i].backend);
    }
    free(pool->bitmap);

    umm_log_info(
                 "ssd_pool: destroyed %u devices, total=%lu MB",
                 pool->num_devices,
                 (unsigned long)(pool->total_capacity / (1024 * 1024)));

    pthread_mutex_unlock(&pool->lock);
    pthread_mutex_destroy(&pool->lock);
    free(pool);
}

/* ------------------------------------------------------------------------ */
/* ssd_pool_add_device                                                      */
/* ------------------------------------------------------------------------ */

int ssd_pool_add_device(SsdPool *pool, const char *device_path, uint64_t capacity)
{
    if (!pool || !device_path)
        return UMM_E_INVALID_ARG;
    if (pool->num_devices >= SSD_POOL_MAX_DEVICES)
        return UMM_E_NO_MEMORY;

    /* 块设备允许 capacity=0（=整盘），实际容量以后端探测为准；
     * 文件后端必须显式给容量（backend_create 内部校验） */
    if (capacity > 0) {
        capacity = (capacity + SSD_POOL_PAGE_SIZE - 1)
                   & ~(uint64_t)(SSD_POOL_PAGE_SIZE - 1);
    }

    pthread_mutex_lock(&pool->lock);

    uint32_t idx = pool->num_devices;
    uint64_t virtual_base = pool->total_capacity;

    /* Create underlying ssd_backend */
    SsdBackend *sb = ssd_backend_create(device_path, capacity);
    if (!sb) {
        pthread_mutex_unlock(&pool->lock);
        umm_log_error(
                      "ssd_pool: failed to create backend for %s", device_path);
        return UMM_E_TRANSPORT_ERROR;
    }

    /* 以后端实际纳管容量为准（块设备可能做了整盘探测/页对齐截断） */
    capacity = ssd_backend_capacity(sb);
    if (capacity == 0) {
        ssd_backend_destroy(sb);
        pthread_mutex_unlock(&pool->lock);
        return UMM_E_INVALID_ARG;
    }

    pool->devices[idx].backend      = sb;
    pool->devices[idx].virtual_base = virtual_base;
    pool->devices[idx].capacity     = capacity;
    pool->num_devices++;
    pool->total_capacity += capacity;

    /* Resize global virtual bitmap */
    uint64_t new_total_pages = pool->total_capacity / SSD_POOL_PAGE_SIZE;
    uint64_t new_words = (new_total_pages + SSD_POOL_BITS_PER_U64 - 1)
                         / SSD_POOL_BITS_PER_U64;

    uint64_t *new_bm = calloc(new_words, sizeof(uint64_t));
    if (!new_bm) {
        /* rollback */
        pool->total_capacity -= capacity;
        pool->num_devices--;
        ssd_backend_destroy(sb);
        pthread_mutex_unlock(&pool->lock);
        return UMM_E_NO_MEMORY;
    }

    if (pool->bitmap) {
        /* Copy old bitmap (old pages remain at the same positions) */
        memcpy(new_bm, pool->bitmap, pool->bitmap_words * sizeof(uint64_t));
        free(pool->bitmap);
    }

    pool->bitmap = new_bm;
    pool->bitmap_words = new_words;
    pool->total_pages = new_total_pages;
    pool->free_pages += (capacity / SSD_POOL_PAGE_SIZE);

    pthread_mutex_unlock(&pool->lock);

    umm_log_info(
                 "ssd_pool: added device[%u] %s, capacity=%lu MB, "
                 "virtual_base=0x%lx, total_pool=%lu MB",
                 idx, device_path,
                 (unsigned long)(capacity / (1024 * 1024)),
                 (unsigned long)virtual_base,
                 (unsigned long)(pool->total_capacity / (1024 * 1024)));
    return UMM_OK;
}

/* ------------------------------------------------------------------------ */
/* Query helpers                                                            */
/* ------------------------------------------------------------------------ */

uint64_t ssd_pool_total_capacity(const SsdPool *pool)
{
    return pool ? pool->total_capacity : 0;
}

uint32_t ssd_pool_num_devices(const SsdPool *pool)
{
    return pool ? pool->num_devices : 0;
}

/* ------------------------------------------------------------------------ */
/* ssd_pool_alloc — allocate from global virtual address space              */
/* ------------------------------------------------------------------------ */

int ssd_pool_alloc(SsdPool *pool, uint64_t size, uint64_t *out_voffset)
{
    if (!pool || !out_voffset || size == 0)
        return UMM_E_INVALID_ARG;

    uint64_t npages = (size + SSD_POOL_PAGE_SIZE - 1) / SSD_POOL_PAGE_SIZE;

    pthread_mutex_lock(&pool->lock);

    if (pool->free_pages < npages) {
        pthread_mutex_unlock(&pool->lock);
        umm_log_error(
                      "ssd_pool: out of space (need %lu pages, free %lu)",
                      (unsigned long)npages, (unsigned long)pool->free_pages);
        return UMM_E_NO_MEMORY;
    }

    uint64_t page = bm_find_free(pool->bitmap, pool->total_pages, 0, npages);
    if (page >= pool->total_pages) {
        pthread_mutex_unlock(&pool->lock);
        umm_log_error(
                      "ssd_pool: no contiguous %lu-page region",
                      (unsigned long)npages);
        return UMM_E_NO_MEMORY;
    }

    for (uint64_t i = 0; i < npages; i++)
        bm_set(pool->bitmap, page + i);

    pool->free_pages -= npages;
    *out_voffset = page * SSD_POOL_PAGE_SIZE;

    pthread_mutex_unlock(&pool->lock);

    umm_log_debug(
                  "ssd_pool: allocated %lu pages at voffset=0x%lx "
                  "(free=%lu/%lu)",
                  (unsigned long)npages, (unsigned long)*out_voffset,
                  (unsigned long)pool->free_pages,
                  (unsigned long)pool->total_pages);
    return UMM_OK;
}

/* ------------------------------------------------------------------------ */
/* ssd_pool_free                                                            */
/* ------------------------------------------------------------------------ */

int ssd_pool_free(SsdPool *pool, uint64_t voffset, uint64_t size)
{
    if (!pool || size == 0 || voffset >= pool->total_capacity)
        return UMM_E_INVALID_ARG;

    uint64_t start_page = voffset / SSD_POOL_PAGE_SIZE;
    uint64_t npages = (size + SSD_POOL_PAGE_SIZE - 1) / SSD_POOL_PAGE_SIZE;

    if (start_page + npages > pool->total_pages)
        return UMM_E_INVALID_ARG;   /* 越界释放（原为静默截断） */

    pthread_mutex_lock(&pool->lock);

    /* 双重释放检测：区间内所有页必须处于已分配态
     * （升级为 int 返回值前，重复 free 会被静默吞掉） */
    for (uint64_t i = 0; i < npages; i++) {
        if (!bm_test(pool->bitmap, start_page + i)) {
            pthread_mutex_unlock(&pool->lock);
            umm_log_warn("ssd_pool: double free rejected at voffset=0x%lx "
                         "(page %lu not allocated)",
                         (unsigned long)voffset,
                         (unsigned long)(start_page + i));
            return UMM_E_INVALID_ARG;
        }
    }

    for (uint64_t i = 0; i < npages; i++) {
        bm_clear(pool->bitmap, start_page + i);
        pool->free_pages++;
    }

    pthread_mutex_unlock(&pool->lock);

    umm_log_debug(
                  "ssd_pool: freed %lu pages at voffset=0x%lx "
                  "(free=%lu/%lu)",
                  (unsigned long)npages, (unsigned long)voffset,
                  (unsigned long)pool->free_pages,
                  (unsigned long)pool->total_pages);
    return UMM_OK;
}

/* ------------------------------------------------------------------------ */
/* ssd_pool_translate — virtual offset → (device, physical_offset)          */
/* ------------------------------------------------------------------------ */

int ssd_pool_translate(SsdPool *pool, uint64_t voffset,
                        uint32_t *out_dev, uint64_t *out_poff)
{
    if (!pool || voffset >= pool->total_capacity)
        return UMM_E_INVALID_ARG;

    for (uint32_t i = 0; i < pool->num_devices; i++) {
        SsdDevice *dev = &pool->devices[i];
        if (voffset >= dev->virtual_base &&
            voffset < dev->virtual_base + dev->capacity) {
            if (out_dev)  *out_dev = i;
            if (out_poff) *out_poff = voffset - dev->virtual_base;
            return UMM_OK;
        }
    }
    return UMM_E_INVALID_ARG;  /* should not reach here */
}

/* ------------------------------------------------------------------------ */
/* ssd_pool_get_ptr                                                         */
/* ------------------------------------------------------------------------ */

void* ssd_pool_get_ptr(SsdPool *pool, uint64_t voffset)
{
    if (!pool)
        return NULL;

    uint32_t dev_idx;
    uint64_t poff;
    if (ssd_pool_translate(pool, voffset, &dev_idx, &poff) != UMM_OK)
        return NULL;

    return ssd_backend_get_ptr(pool->devices[dev_idx].backend, poff);
}

/* ------------------------------------------------------------------------ */
/* ssd_pool_span_in_one_device — mmap 快路径安全性判定                        */
/*                                                                         */
/* [voffset, voffset+len) 完全落在单个设备内 → UMM_OK（map+memcpy 安全）；   */
/* 跨界/越界 → 错误（调用方必须回退 ssd_pool_pread/pwrite 分段通路）。         */
/* 直接返回指针不做长度校验曾在多设备池下 memcpy 冲出设备映射（SEGV）。        */
/* ------------------------------------------------------------------------ */

int ssd_pool_span_in_one_device(SsdPool *pool, uint64_t voffset, uint64_t len)
{
    if (!pool || len == 0)
        return UMM_E_INVALID_ARG;

    uint32_t dev_idx;
    uint64_t poff;
    if (ssd_pool_translate(pool, voffset, &dev_idx, &poff) != UMM_OK)
        return UMM_E_NOT_FOUND;

    uint64_t dev_end = pool->devices[dev_idx].virtual_base
                     + pool->devices[dev_idx].capacity;
    /* len > dev_end - voffset：减法形式避免 voffset+len 溢出 */
    return (len <= dev_end - voffset) ? UMM_OK : UMM_E_INVALID_ARG;
}

/* ------------------------------------------------------------------------ */
/* ssd_pool_get_usage — 池级容量统计（SSD tier stats 数据源）                 */
/* ------------------------------------------------------------------------ */

void ssd_pool_get_usage(SsdPool *pool, uint64_t *out_total,
                        uint64_t *out_free)
{
    if (!pool) {
        if (out_total) *out_total = 0;
        if (out_free)  *out_free  = 0;
        return;
    }
    pthread_mutex_lock(&pool->lock);
    uint64_t total = pool->total_pages * (uint64_t)SSD_POOL_PAGE_SIZE;
    uint64_t freeb = pool->free_pages  * (uint64_t)SSD_POOL_PAGE_SIZE;
    pthread_mutex_unlock(&pool->lock);
    if (out_total) *out_total = total;
    if (out_free)  *out_free  = freeb;
}

/* ------------------------------------------------------------------------ */
/* ssd_pool_sync                                                            */
/* ------------------------------------------------------------------------ */

int ssd_pool_sync(SsdPool *pool, uint64_t voffset, uint64_t size)
{
    if (!pool || size == 0)
        return UMM_E_INVALID_ARG;

    /* Handle cross-device sync */
    uint64_t end = voffset + size;
    if (end > pool->total_capacity)
        end = pool->total_capacity;

    while (voffset < end) {
        uint32_t dev_idx;
        uint64_t poff;
        int rc = ssd_pool_translate(pool, voffset, &dev_idx, &poff);
        if (rc != UMM_OK)
            return rc;

        /* How much remains in this device? */
        uint64_t dev_end = pool->devices[dev_idx].virtual_base
                         + pool->devices[dev_idx].capacity;
        uint64_t chunk = end - voffset;
        if (voffset + chunk > dev_end)
            chunk = dev_end - voffset;

        rc = ssd_backend_sync(pool->devices[dev_idx].backend, poff, chunk);
        if (rc != UMM_OK)
            return rc;

        voffset += chunk;
    }
    return UMM_OK;
}

/* ------------------------------------------------------------------------ */
/* ssd_pool_invalidate — 池级缓存失效（跨设备分段，镜像 ssd_pool_sync）。     */
/* 用途：共享盘读共享——对端节点写入并落盘后，本端 invalidate 再读。           */
/* ------------------------------------------------------------------------ */

int ssd_pool_invalidate(SsdPool *pool, uint64_t voffset, uint64_t size)
{
    if (!pool || size == 0)
        return UMM_E_INVALID_ARG;

    uint64_t end = voffset + size;
    if (end > pool->total_capacity)
        end = pool->total_capacity;

    while (voffset < end) {
        uint32_t dev_idx;
        uint64_t poff;
        int rc = ssd_pool_translate(pool, voffset, &dev_idx, &poff);
        if (rc != UMM_OK)
            return rc;

        uint64_t dev_end = pool->devices[dev_idx].virtual_base
                         + pool->devices[dev_idx].capacity;
        uint64_t chunk = end - voffset;
        if (voffset + chunk > dev_end)
            chunk = dev_end - voffset;

        rc = ssd_backend_invalidate(pool->devices[dev_idx].backend,
                                    poff, chunk);
        if (rc != UMM_OK)
            return rc;

        voffset += chunk;
    }
    return UMM_OK;
}

/* ------------------------------------------------------------------------ */
/* ssd_pool_pread / ssd_pool_pwrite — 池级主机 I/O（跨设备分段）              */
/* ------------------------------------------------------------------------ */

static int ssd_pool_io(SsdPool *pool, uint64_t voffset, uint64_t len,
                       void *buf, int is_write)
{
    if (!pool || (!buf && len > 0))
        return UMM_E_INVALID_ARG;
    if (voffset + len > pool->total_capacity)
        return UMM_E_INVALID_ARG;   /* 越界 */

    uint8_t *p   = (uint8_t *)buf;
    uint64_t end = voffset + len;
    uint64_t cur = voffset;
    uint64_t done = 0;

    while (cur < end) {
        uint32_t dev_idx;
        uint64_t poff;
        int rc = ssd_pool_translate(pool, cur, &dev_idx, &poff);
        if (rc != UMM_OK)
            return rc;

        /* 本设备内可连续读写的长度 */
        uint64_t dev_end = pool->devices[dev_idx].virtual_base
                         + pool->devices[dev_idx].capacity;
        uint64_t chunk = end - cur;
        if (cur + chunk > dev_end)
            chunk = dev_end - cur;

        SsdBackend *sb = pool->devices[dev_idx].backend;
        rc = is_write ? ssd_backend_pwrite(sb, poff, chunk, p + done)
                      : ssd_backend_pread (sb, poff, chunk, p + done);
        if (rc != UMM_OK)
            return rc;

        cur  += chunk;
        done += chunk;
    }
    return UMM_OK;
}

int ssd_pool_pread(SsdPool *pool, uint64_t voffset, uint64_t len, void *buf)
{
    return ssd_pool_io(pool, voffset, len, buf, 0);
}

int ssd_pool_pwrite(SsdPool *pool, uint64_t voffset, uint64_t len,
                    const void *buf)
{
    return ssd_pool_io(pool, voffset, len, (void *)buf, 1);
}

/* ------------------------------------------------------------------------ */
/* ssd_pool_register_dev_mem / 池级批量 I/O                                   */
/* ------------------------------------------------------------------------ */

int ssd_pool_register_dev_mem(SsdPool *pool, void *dev_mem,
                              uint64_t aligned_size)
{
    if (!pool || !dev_mem || aligned_size == 0)
        return UMM_E_INVALID_ARG;

    /* 向池内所有 nds 设备转发 register（非 nds 设备跳过） */
    int nds_seen = 0;
    for (uint32_t i = 0; i < pool->num_devices; i++) {
        SsdBackend *sb = pool->devices[i].backend;
        if (!sb->is_nds)
            continue;
        nds_seen = 1;
        int rc = ssd_backend_register_dev_mem(sb, dev_mem, aligned_size);
        if (rc != UMM_OK)
            return rc;
    }
    if (!nds_seen) {
        umm_log_error(
                      "ssd_pool: register_dev_mem 失败：池内无 nds 设备");
        return UMM_E_INVALID_ARG;
    }
    return UMM_OK;
}

static int ssd_pool_batch_io(SsdPool *pool, const UmmNdsIOVec *iovs,
                             size_t n_iov, int is_write)
{
    if (!pool || (!iovs && n_iov > 0))
        return UMM_E_INVALID_ARG;
    if (n_iov == 0)
        return UMM_OK;          /* no-op */

    /* 内部拷贝临时数组改写 offset，不得修改调用方 iov 数组 */
    UmmNdsIOVec *tmp = malloc(n_iov * sizeof(*tmp));
    uint32_t *dev_of = malloc(n_iov * sizeof(*dev_of));
    if (!tmp || !dev_of) {
        free(tmp);
        free(dev_of);
        return UMM_E_NO_MEMORY;
    }
    memcpy(tmp, iovs, n_iov * sizeof(*tmp));

    /* 先全量校验（translate + 单设备内），全部通过后才下发 I/O，
     * 避免部分 iov 已落盘才报错 */
    for (size_t i = 0; i < n_iov; i++) {
        if (!tmp[i].vaddr || tmp[i].length == 0) {
            umm_log_error(
                          "ssd_pool: batch iov[%zu] invalid vaddr/length",
                          i);
            free(tmp);
            free(dev_of);
            return UMM_E_INVALID_ARG;
        }
        uint32_t dev_idx;
        uint64_t poff;
        int rc = ssd_pool_translate(pool, tmp[i].offset, &dev_idx, &poff);
        if (rc != UMM_OK) {
            free(tmp);
            free(dev_of);
            return rc;
        }
        SsdDevice *dev = &pool->devices[dev_idx];
        if (poff + tmp[i].length > dev->capacity) {
            umm_log_error(
                          "ssd_pool: batch iov[%zu] 跨设备（voff=%lu, "
                          "len=%lu），调用方须按设备边界切分", i,
                          (unsigned long)tmp[i].offset,
                          (unsigned long)tmp[i].length);
            free(tmp);
            free(dev_of);
            return UMM_E_INVALID_ARG;
        }
        tmp[i].offset = poff;       /* 池虚拟偏移 → 设备物理偏移 */
        dev_of[i]     = dev_idx;
    }

    /* 按设备连续段分组下发（同设备相邻 iov 合并为一次后端批量调用） */
    size_t i = 0;
    while (i < n_iov) {
        size_t j = i + 1;
        while (j < n_iov && dev_of[j] == dev_of[i])
            j++;
        SsdBackend *sb = pool->devices[dev_of[i]].backend;
        int rc = is_write
            ? ssd_backend_batch_write(sb, tmp + i, j - i)
            : ssd_backend_batch_read (sb, tmp + i, j - i);
        if (rc != UMM_OK) {
            free(tmp);
            free(dev_of);
            return rc;
        }
        i = j;
    }

    free(tmp);
    free(dev_of);
    return UMM_OK;
}

int ssd_pool_batch_read(SsdPool *pool, UmmNdsIOVec *iovs, size_t n_iov)
{
    return ssd_pool_batch_io(pool, iovs, n_iov, 0);
}

int ssd_pool_batch_write(SsdPool *pool, const UmmNdsIOVec *iovs,
                         size_t n_iov)
{
    return ssd_pool_batch_io(pool, iovs, n_iov, 1);
}
