/*
 * ssd_backend_libnvm.c — libnvm 后端实现（dlopen 软依赖）
 *
 * 依赖的外部符号（libnvm_host.so，NPU_Direct_Storage 项目）：
 *   nvm_host_init / nvm_host_free / nvm_host_get_disk_info
 *   nvm_host_read / nvm_host_write / nvm_host_is_initialized
 *
 * 加载策略：
 *   1. 环境变量 UMM_LIBNVM_PATH 指定的完整路径（测试/自定义安装）
 *   2. 系统库路径中的 "libnvm_host.so"
 */
#include "ssd_backend_libnvm.h"
#include "../common/log.h"
#include "../../include/umm.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIBNVM_DEFAULT_SONAME  "libnvm_host.so"
#define LIBNVM_QUEUE_DEPTH     64
#define LIBNVM_MAX_CTXS        16
#define LIBNVM_DEFAULT_CTXS    4

/* ---- libnvm_host 公开 API 的本地声明（与 nvm_host.h 一致）---- */
typedef struct nvm_host_ctx nvm_host_ctx_t;

typedef struct {
    uint32_t ns_id;
    size_t   page_size;
    size_t   max_data_size;
    size_t   block_size;
} nvm_host_disk_info_t;

typedef int  (*fn_init_t)(const char *, uint32_t, size_t, nvm_host_ctx_t **);
typedef void (*fn_free_t)(nvm_host_ctx_t *);
typedef int  (*fn_info_t)(nvm_host_ctx_t *, nvm_host_disk_info_t *);
typedef int  (*fn_write_t)(nvm_host_ctx_t *, void *, size_t, uint64_t, size_t *);
typedef int  (*fn_read_t)(nvm_host_ctx_t *, void *, size_t, uint64_t, size_t *);

struct SsdLibnvmBackend {
    void               *dlh;        /* dlopen 句柄 */
    /* ctx 池：单 ctx = 单 SQ/CQ 同步语义，且 libnvm 线程安全未证实(R6)。
     * 多 ctx（各自独立队列）+ 每 ctx 一把互斥锁——既不依赖 libnvm
     * 内部线程安全性，又能让多线程真正并行（UMM_LIBNVM_CTXS 可调） */
    nvm_host_ctx_t     *ctxs[LIBNVM_MAX_CTXS];
    uint32_t            n_ctxs;
    pthread_mutex_t     io_locks[LIBNVM_MAX_CTXS];
    uint64_t            rr;         /* 轮询分派计数 */
    uint64_t            max_io;     /* 单次 I/O 上限（字节） */
    uint64_t            base_off;   /* 窗口基址（spec "+<off>" 后缀，缺省 0） */
    uint32_t            block_size; /* 逻辑块尺寸 */
    fn_free_t           fn_free;
    fn_write_t          fn_write;
    fn_read_t           fn_read;
};

static void *load_symbol(void *dlh, const char *name)
{
    void *sym = dlsym(dlh, name);
    if (!sym)
        umm_log_error(
                      "libnvm: symbol %s not found: %s", name, dlerror());
    return sym;
}

int ssd_libnvm_open(const char *spec, SsdLibnvmBackend **out)
{
    if (!spec || !out)
        return UMM_E_INVALID_ARG;

    /* 先剥离可选的 "+<base_off>" 窗口基址后缀（须位于 @ns 之后，
     * 支持 0x 十六进制），再解析 "ctrl_path@ns_id"（@ 可省，缺省 ns=1） */
    char spec_buf[320];
    if (strlen(spec) >= sizeof(spec_buf))
        return UMM_E_INVALID_ARG;
    strcpy(spec_buf, spec);
    uint64_t base_off = 0;
    char *plus = strrchr(spec_buf, '+');
    if (plus) {
        *plus = '\0';
        base_off = strtoull(plus + 1, NULL, 0);
    }

    char ctrl_path[256];
    uint32_t ns_id = 1;
    const char *at = strrchr(spec_buf, '@');
    if (at) {
        size_t plen = (size_t)(at - spec_buf);
        if (plen == 0 || plen >= sizeof(ctrl_path))
            return UMM_E_INVALID_ARG;
        memcpy(ctrl_path, spec_buf, plen);
        ctrl_path[plen] = '\0';
        ns_id = (uint32_t)strtoul(at + 1, NULL, 10);
        if (ns_id == 0)
            ns_id = 1;
    } else {
        if (strlen(spec_buf) >= sizeof(ctrl_path))
            return UMM_E_INVALID_ARG;
        strcpy(ctrl_path, spec_buf);
    }

    /* dlopen：先环境变量，再系统库路径 */
    const char *env = getenv("UMM_LIBNVM_PATH");
    void *dlh = dlopen(env && env[0] ? env : LIBNVM_DEFAULT_SONAME,
                       RTLD_NOW | RTLD_LOCAL);
    if (!dlh) {
        umm_log_error(
                      "libnvm: dlopen failed: %s "
                      "(若报错为其他 .so not found, 属传递依赖缺失: "
                      "ldd 检查 libnvm_host.so 并把缺失库所在目录加入 "
                      "LD_LIBRARY_PATH; 库本身路径用 UMM_LIBNVM_PATH 指定)",
                      dlerror());
        return UMM_E_NOT_FOUND;
    }

    fn_init_t fn_init = (fn_init_t)load_symbol(dlh, "nvm_host_init");
    fn_info_t fn_info = (fn_info_t)load_symbol(dlh, "nvm_host_get_disk_info");
    fn_read_t fn_read = (fn_read_t)load_symbol(dlh, "nvm_host_read");
    fn_write_t fn_write = (fn_write_t)load_symbol(dlh, "nvm_host_write");
    fn_free_t fn_free = (fn_free_t)load_symbol(dlh, "nvm_host_free");
    if (!fn_init || !fn_info || !fn_read || !fn_write || !fn_free) {
        dlclose(dlh);
        return UMM_E_NOT_FOUND;
    }

    /* ctx 池规模：UMM_LIBNVM_CTXS 环境变量（缺省 4，钳制 1..16）。
     * init 顺序执行（建池阶段单线程），运行期并发使用 */
    uint32_t n_ctxs = LIBNVM_DEFAULT_CTXS;
    const char *env_n = getenv("UMM_LIBNVM_CTXS");
    if (env_n && env_n[0]) {
        unsigned long v = strtoul(env_n, NULL, 10);
        if (v >= 1 && v <= LIBNVM_MAX_CTXS)
            n_ctxs = (uint32_t)v;
    }

    SsdLibnvmBackend *b = calloc(1, sizeof(*b));
    if (!b) {
        dlclose(dlh);
        return UMM_E_NO_MEMORY;
    }

    uint32_t inited = 0;
    for (; inited < n_ctxs; inited++) {
        nvm_host_ctx_t *ctx = NULL;
        int rc = fn_init(ctrl_path, ns_id, LIBNVM_QUEUE_DEPTH, &ctx);
        if (rc != 0 || !ctx) {
            umm_log_error(
                          "libnvm: nvm_host_init(%s, ns=%u) [%u/%u] failed rc=%d",
                          ctrl_path, ns_id, inited + 1, n_ctxs, rc);
            break;
        }
        b->ctxs[inited] = ctx;
        pthread_mutex_init(&b->io_locks[inited], NULL);
    }
    if (inited == 0) {
        dlclose(dlh);
        free(b);
        return UMM_E_IO;
    }
    b->n_ctxs = inited;
    if (inited < n_ctxs)
        umm_log_warn(
                     "libnvm: ctx pool degraded: %u/%u ctxs",
                     inited, n_ctxs);

    /* 提前挂好清理所需字段，失败路径直接 ssd_libnvm_close */
    b->dlh      = dlh;
    b->fn_free  = fn_free;
    b->fn_write = fn_write;
    b->fn_read  = fn_read;

    nvm_host_disk_info_t info;
    memset(&info, 0, sizeof(info));
    int rc = fn_info(b->ctxs[0], &info);
    if (rc != 0) {
        umm_log_error(
                      "libnvm: get_disk_info failed rc=%d", rc);
        ssd_libnvm_close(b);
        return UMM_E_IO;
    }

    b->max_io     = info.max_data_size ? info.max_data_size
                                       : (64ULL * 1024);
    b->base_off   = base_off;
    b->block_size = info.block_size ? (uint32_t)info.block_size : 512;

    /* ---- 开机只读探针：逐 ctx 读窗口首块，剔除静默损坏的 ctx ----
     * 真机实证：libnvm_host 同进程多次 nvm_host_init 后，先建的 ctx
     * 在首次 I/O 才报 Invalid namespace（疑进程级全局状态被后建 init
     * 破坏）。init 成功≠可用，必须 I/O 级自检；只读不触碰数据。 */
    {
        size_t probe_len = b->block_size > 4096 ? 4096
                           : (b->block_size ? b->block_size : 512);
        uint8_t *pbuf = malloc(probe_len);
        if (!pbuf) {
            ssd_libnvm_close(b);
            return UMM_E_NO_MEMORY;
        }
        uint32_t good = 0;
        for (uint32_t i = 0; i < b->n_ctxs; i++) {
            size_t xferred = 0;
            int prc = b->fn_read(b->ctxs[i], pbuf, probe_len,
                                 b->base_off, &xferred);
            if (prc == 0 && xferred == probe_len) {
                if (good != i) {
                    b->ctxs[good] = b->ctxs[i];  /* 紧凑到前部 */
                    b->ctxs[i] = NULL;
                }
                good++;
            } else {
                umm_log_warn(
                    "libnvm: probe ctx[%u/%u] FAILED (rc=%d, xferred=%zu), 剔除",
                    i + 1, b->n_ctxs, prc, xferred);
                b->fn_free(b->ctxs[i]);
                b->ctxs[i] = NULL;
            }
        }
        free(pbuf);

        if (good == 0) {
            umm_log_error(
                "libnvm: 所有 ctx 只读探针失败（同进程多 init 破坏？"
                "请用 UMM_LIBNVM_CTXS=1 重试并向 libnvm 提供方反馈）");
            ssd_libnvm_close(b);   /* ctxs 已逐个 free 置 NULL，安全 */
            return UMM_E_IO;
        }
        if (good < b->n_ctxs)
            umm_log_warn(
                "libnvm: ctx pool probe-degraded: %u/%u usable",
                good, b->n_ctxs);
        b->n_ctxs = good;
    }

    umm_log_info(
                 "libnvm: opened %s ns=%u, block=%u, max_io=%lu KB, "
                 "base_off=0x%lx, ctxs=%u",
                 ctrl_path, ns_id, b->block_size,
                 (unsigned long)(b->max_io / 1024),
                 (unsigned long)b->base_off, b->n_ctxs);
    *out = b;
    return UMM_OK;
}

uint64_t ssd_libnvm_max_io(SsdLibnvmBackend *b)
{
    return b ? b->max_io : 0;
}

uint32_t ssd_libnvm_block_size(SsdLibnvmBackend *b)
{
    return b ? b->block_size : 0;
}

static int ssd_libnvm_io(SsdLibnvmBackend *b, uint64_t offset,
                         uint64_t len, void *buf, int is_write)
{
    if (!b || (!buf && len > 0))
        return UMM_E_INVALID_ARG;

    /* 轮询选一个 ctx，本次调用的全部分段固定在该 ctx 上；
     * 每 ctx 一把互斥锁——调用间并行，ctx 内串行（不依赖
     * libnvm 内部线程安全性，R6 由构造保证） */
    uint32_t slot = (uint32_t)(__sync_fetch_and_add(&b->rr, 1) % b->n_ctxs);
    nvm_host_ctx_t *ctx = b->ctxs[slot];
    pthread_mutex_lock(&b->io_locks[slot]);

    uint8_t *p = (uint8_t *)buf;
    uint64_t done = 0;
    int out = UMM_OK;
    while (done < len) {
        size_t chunk = (size_t)((len - done) > b->max_io
                                ? b->max_io : (len - done));
        uint64_t real_off = b->base_off + offset + done;  /* 窗口内相对 → 物理 */
        size_t xferred = 0;
        int rc = is_write
            ? b->fn_write(ctx, p + done, chunk, real_off, &xferred)
            : b->fn_read (ctx, p + done, chunk, real_off, &xferred);
        if (rc != 0) {
            umm_log_error(
                          "libnvm: %s(off=%lu, len=%zu) failed rc=%d",
                          is_write ? "write" : "read",
                          (unsigned long)real_off, chunk, rc);
            out = UMM_E_IO;
            break;
        }
        if (xferred != chunk) {
            umm_log_error(
                          "libnvm: short %s: %zu/%zu bytes",
                          is_write ? "write" : "read", xferred, chunk);
            out = UMM_E_IO;
            break;
        }
        done += chunk;
    }

    pthread_mutex_unlock(&b->io_locks[slot]);
    return out;
}

int ssd_libnvm_read(SsdLibnvmBackend *b, uint64_t offset,
                    uint64_t len, void *buf)
{
    return ssd_libnvm_io(b, offset, len, buf, 0);
}

int ssd_libnvm_write(SsdLibnvmBackend *b, uint64_t offset,
                     uint64_t len, const void *buf)
{
    /* libnvm_host_write 的 data 参数非常量（内部 DMA staging），
     * 语义上不会修改调用方数据 */
    return ssd_libnvm_io(b, offset, len, (void *)buf, 1);
}

void ssd_libnvm_close(SsdLibnvmBackend *b)
{
    if (!b)
        return;
    for (uint32_t i = 0; i < b->n_ctxs; i++) {
        if (b->ctxs[i] && b->fn_free)
            b->fn_free(b->ctxs[i]);
        pthread_mutex_destroy(&b->io_locks[i]);
    }
    if (b->dlh)
        dlclose(b->dlh);
    free(b);
}
