/*
 * ssd_backend_nds.c — NDS NPU2SSD 直驱后端实现（dlopen 软依赖）
 *
 * 依赖的外部符号（libnds_aiv.so，C++ 单例类 NDS，见 include/nds_aiv.h）。
 * UMM 核心是纯 C（gcc, gnu11），为保持 dlopen 软依赖模式、不给 UMM
 * 核心引入 C++ 工具链硬依赖，这里用 dlsym 解析 Itanium ABI mangled
 * 符号（g++/Clang Linux aarch64/x86_64 稳定），成员函数按 Itanium
 * ABI 以 this 为首参的函数指针调用：
 *   _ZN3NDS8InstanceEv              NDS::Instance()
 *   _ZN3NDS8nds_initEjmmmm          nds_init(uint32,size_t,size_t,u64,u64)
 *   _ZN3NDS10nds_uninitEv           nds_uninit()
 *   _ZN3NDS12nds_registerEPvm       nds_register(void*, u64)
 *   _ZN3NDS16nds_single_writeEPvmm  nds_single_write(void*, u64, u64)
 *   _ZN3NDS15nds_single_readEPvmm   nds_single_read(void*, u64, u64)
 *   _ZN3NDS15nds_batch_writeEP5IOVecm  nds_batch_write(IOVec*, size_t)
 *   _ZN3NDS14nds_batch_readEP5IOVecm   nds_batch_read(IOVec*, size_t)
 *
 * 加载策略（与 libnvm 后端完全一致）：
 *   1. 环境变量 UMM_NDS_PATH 指定的完整路径（测试/自定义安装）
 *   2. 系统库路径中的 "libnds_aiv.so"
 *
 * 已知限制（NDS API 固有，调用方须知）：
 *   - NDS 全部接口 void 返回：无法在调用后感知错误，只能在调用前做
 *     参数校验（page 对齐、len <= max_io、已注册、vaddr 完整落在
 *     某一个已注册段内）。
 *   - 线程安全性未证实（同 libnvm R6）：所有 NDS 调用（init/register/
 *     read/write/batch/uninit）由锁串行化。NDS 是进程级单例，锁粒度
 *     必须是 device 级而非 backend 级——同进程多个 backend 打开同一
 *     device_id 时共享注册表 entry，per-backend 锁互不知道对方，
 *     无法保护单例。故 I/O 锁放在 NdsDeviceEntry 内（per-device），
 *     首个 open 建 entry 时 init，最后一个 close（refcount 归零、
 *     uninit 之后）destroy。
 *
 * 全局唯一锁序（任何路径不得反向持锁）：
 *   g_reg_lock → entry->io_lock
 *   - g_reg_lock：串行化注册表增删、init/uninit/register；
 *   - entry->io_lock：串行化该 device 上全部 NDS I/O 调用。
 *
 * batch 单发模拟兜底（过渡措施，库修复后移除）：
 *   真机证据：真实库 nds_batch_write 空转（0.06ms 返回、无内核轨迹、
 *   盘无数据，疑似未实现）；nds_batch_read 有内核轨迹但数据正确性
 *   未证实；单发 read/write 已验证完全正确。故提供两个 env 开关，
 *   open 时读取（per-backend 生效）：
 *     UMM_NDS_BATCH_WRITE_EMULATE=1  ssd_nds_batch_write 不走
 *       fn_batch_write，改为逐 iov 循环 fn_single_write；
 *     UMM_NDS_BATCH_READ_EMULATE=1   同理循环 fn_single_read。
 *   语义差异（调用方须知）：单发循环与真 batch 不等价——
 *   非原子（中途失败/中断时前部 iov 已落盘）、无批量提交（性能低于
 *   真 batch）、completion 语义退化为逐条。仅作库 batch 未实现/异常
 *   时的过渡降级，库修复后移除本开关。
 */
/* _GNU_SOURCE 必须在任何 libc 头之前定义：RTLD_DEFAULT 属 GNU 扩展，
 * 部分工具链（无 _GNU_SOURCE 默认）不暴露，真机编译报"未声明" */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "ssd_backend_nds.h"
#include "../common/log.h"
#include "../../include/umm.h"

#include <dlfcn.h>

/* 兜底：极端老 glibc 即使 _GNU_SOURCE 下也未暴露 RTLD_DEFAULT 时自行定义
 *（RTLD_DEFAULT 语义即 NULL 句柄：在全局符号域中查找） */
#ifndef RTLD_DEFAULT
#define RTLD_DEFAULT ((void *)0)
#endif
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NDS_DEFAULT_SONAME   "libnds_aiv.so"
#define NDS_MAX_DEVICES      4     /* 注册表最多纳管 4 个 NPU device */

/* 运行参数缺省值（env 可调），对齐 NDS 库提供方真实测试代码：
 *   g_nds.nds_init(device, queueDepth, numQueues, 4096, 1024*1024*7)
 * 其中 max_page_num=7M 是 NDS 内部 IO 跟踪资源池规模（支撑 ~10000
 * 个 8KB iov 在途），不是单次 IO 上限。单次 IO 上限拆出独立 env
 * UMM_NDS_MAX_IO（缺省 1MB）：提供方样例 per-iov 为 8192 字节，真实
 * 上限待库方文档确认，此为保守可调上限 */
#define NDS_DEFAULT_QUEUE_DEPTH   64
/* 缺省 1：库方确认当前版本只有一个 qp（单客户端单队列），多队列创建
 * 无实际收益且拉长 init；库支持多 qp 后经 UMM_NDS_CORE_NUM 调大 */
#define NDS_DEFAULT_CORE_NUM      1
#define NDS_DEFAULT_PAGE_SIZE     4096
#define NDS_DEFAULT_MAX_PAGE_NUM  (7 * 1024 * 1024)
#define NDS_DEFAULT_MAX_IO        (1024 * 1024)
/* RPC bind 重试预算缺省值（env UMM_NDS_RPC_WAIT_MS 可调，0=不重试） */
#define NDS_DEFAULT_RPC_WAIT_MS   3000

/* ---- libnds_aiv.so 公开符号的函数指针类型（Itanium ABI，this 为首参）---- */
typedef void *(*fn_instance_t)(void);
typedef void  (*fn_init_t)(void *self, uint32_t device_id,
                           size_t queue_depth, size_t core_num,
                           uint64_t page_size, uint64_t max_page_num);
typedef void  (*fn_uninit_t)(void *self);
typedef void  (*fn_register_t)(void *self, void *dev_mem,
                               uint64_t aligned_read_size);
typedef void  (*fn_single_io_t)(void *self, void *vaddr,
                                uint64_t bytes, uint64_t f_offset);
typedef void  (*fn_batch_io_t)(void *self, UmmNdsIOVec *iovs, size_t n_iov);

/* ---- nvm_host RPC context 引导（双进程架构，env UMM_NDS_RPC_SOCKET
 * 驱动，可选）。类型与语义依据 NDS 提供方真实测试代码：
 *   nvm_host_rpc_ctx_t rpc_ctx = nullptr;
 *   nvm_host_error_t err = nvm_host_bind_remote(sock, &rpc_ctx);
 *   // err != NVM_HOST_OK → 失败
 *   nvm_host_set_rpc_context(rpc_ctx);   // 此后才允许 nds_init
 * 真机故障背景：libnds_aiv.so 内部复用 nvm_host 的 admin queue；
 * 提供方为双进程架构——Process A（RPC server，持有 NVMe 控制器）与
 * NDS 客户端进程分离，客户端必须先建立 RPC context（bind_remote +
 * set_rpc_context）再 nds_init，否则 nds_init 内
 * nvm_host_admin_cq_create 报 "no RPC context" 失败。
 * 两符号为 extern "C" 非 mangled 名（与 nvm_host_init/nvm_host_read
 * 同一家族），nvm_host_error_t 是 int 兼容枚举（NVM_HOST_OK=0）。
 * 连接生命周期（真机实证修复）：每次首个 open bind 一条连接，最后
 * 一个 close 调 nvm_host_rpc_disconnect（可选符号）释放——此前 close
 * 从不断开，反复 open/close 会把真实 RPC server 的僵尸连接占满，
 * 后续 bind 阻塞 ---- */
typedef void *nvm_host_rpc_ctx_t;
typedef int   nvm_host_error_t;   /* NVM_HOST_OK=0 */

/* 单调时钟毫秒差（防 tv_nsec 借位下溢） */
static uint64_t nds_elapsed_ms(const struct timespec *t0,
                               const struct timespec *t1)
{
    int64_t ms = (int64_t)(t1->tv_sec - t0->tv_sec) * 1000 +
                 (int64_t)(t1->tv_nsec - t0->tv_nsec) / 1000000;
    return ms > 0 ? (uint64_t)ms : 0;
}
typedef nvm_host_error_t (*fn_bind_remote_t)(const char *,
                                             nvm_host_rpc_ctx_t *);
typedef void (*fn_set_rpc_ctx_t)(nvm_host_rpc_ctx_t);
/* 可选断开符号（提供方头文件原型：
 *   void nvm_host_rpc_disconnect(nvm_host_rpc_ctx_t rpc_ctx);
 * 老版本 libnvm_host.so 可能不导出，解析失败不视为错误（记 NULL +
 * debug 日志）。生命周期语义：每 device entry 首个 open bind 建连，
 * 最后一个 close（refcount 归零、fn_uninit 之后）disconnect 释放，
 * 防真实 RPC server 侧连接永久泄漏 */
typedef void (*fn_rpc_disconnect_t)(void *);

/* ---- device 级全局注册表：nds_init 是单例一次性初始化，进程内多
 * backend 打开同一 device_id 时引用计数复用（仅首个 open 调 nds_init，
 * 最后一个 close 调 nds_uninit）。nds_register 作用于进程级单例，
 * 注册状态同样按 device 记录在表中、同 device 的所有 backend 共享 ---- */
/* 单 device 最多注册的内存段数（对齐 NDS 提供方测试代码的多段
 * nds_register 用法——提供方样例连续注册 4 个独立段） */
#define NDS_MAX_REGIONS  16

typedef struct {
    int      used;
    uint32_t device_id;
    uint32_t refcount;
    /* 多段注册表（同 device 共享）：n_regions>0 即"已注册"语义；
     * I/O 前区间校验要求 vaddr 完整落在某一个段内（跨段拒绝） */
    void    *regions[NDS_MAX_REGIONS];
    uint64_t region_sizes[NDS_MAX_REGIONS];
    uint32_t n_regions;
    uint64_t page_size;
    uint64_t max_page_num;
    /* nvm_host RPC context（UMM_NDS_RPC_SOCKET 引导，NULL=未启用）。
     * 生命周期随 device entry：首个 open bind 建连，同 device 后续
     * open 复用；最后一个 close（refcount 归零、fn_uninit 之后）调
     * rpc_disconnect 释放 server 侧连接。rpc_disconnect 为可选符号
     * （老库可能不导出），NULL 时跳过断开 */
    void    *rpc_ctx;
    fn_rpc_disconnect_t rpc_disconnect;
    /* per-device I/O 锁：NDS 是进程级单例，同 device 的所有 backend
     * 的 NDS 调用（read/write/batch）都由这一把锁串行化。
     * 首个 open 建 entry 时 init，最后一个 close 时 destroy */
    pthread_mutex_t io_lock;
} NdsDeviceEntry;

static NdsDeviceEntry  g_devs[NDS_MAX_DEVICES];
static pthread_mutex_t g_reg_lock = PTHREAD_MUTEX_INITIALIZER;

struct SsdNdsBackend {
    void            *dlh;       /* dlopen 句柄 */
    void            *nds;       /* NDS 单例地址（fn_instance() 返回值） */
    NdsDeviceEntry  *dev;       /* 注册表条目（device 级共享状态，
                                 * 含 per-device io_lock） */
    uint64_t         max_io;    /* 单次 I/O 上限（env UMM_NDS_MAX_IO） */
    uint64_t         page_size; /* 对齐单位 */
    uint64_t         base_off;  /* 窗口基址（spec "+<off>" 后缀，缺省 0） */
    uint32_t         device_id;
    fn_uninit_t      fn_uninit;
    fn_register_t    fn_register;
    fn_single_io_t   fn_single_read;
    fn_single_io_t   fn_single_write;
    fn_batch_io_t    fn_batch_read;
    fn_batch_io_t    fn_batch_write;
    /* batch 单发模拟兜底开关（open 时读 env，per-backend 生效）：
     * 1 = batch 接口改为逐 iov 循环单发（过渡措施，见头注释） */
    int              batch_write_emulate;
    int              batch_read_emulate;
};

static void *load_symbol(void *dlh, const char *name)
{
    void *sym = dlsym(dlh, name);
    if (!sym)
        umm_log_error(
                      "nds: symbol %s not found: %s", name, dlerror());
    return sym;
}

static uint64_t env_u64(const char *name, uint64_t dflt)
{
    const char *e = getenv(name);
    if (!e || !e[0])
        return dflt;
    unsigned long long v = strtoull(e, NULL, 10);
    return v ? (uint64_t)v : dflt;
}

/* UMM_NDS_PRELOAD 预载记录表（去重防重复日志）。
 * 预载句柄有意不 dlclose：以 RTLD_GLOBAL 预载的符号会被 NDS 库引用，
 * 句柄必须在进程生命周期内保持驻留；dlopen 本身是 refcount 语义，
 * 重复预载同一路径安全，这里仅记录路径避免重复打日志。
 * 表访问由 g_reg_lock 保护（ssd_nds_open 内加锁使用）。 */
#define NDS_MAX_PRELOADED 8
static char g_preloaded[NDS_MAX_PRELOADED][256];
static int  g_n_preloaded;

/* 预载 UMM_NDS_PRELOAD（冒号分隔的 .so 完整路径列表）。
 * 用途：NDS 库存在未声明依赖（DT_NEEDED 缺失）时，RTLD_NOW 会因
 * 未定义符号失败，而 LD_LIBRARY_PATH 无法解决未声明依赖——只能
 * 预载提供方库（RTLD_NOW|RTLD_GLOBAL 把符号导入全局命名空间）。
 * 任一预载失败即 fail-fast 返回 UMM_E_NOT_FOUND（显式声明的依赖，
 * 失败必须立即暴露）。 */
static int nds_preload_from_env(void)
{
    const char *e = getenv("UMM_NDS_PRELOAD");
    if (!e || !e[0])
        return UMM_OK;

    char buf[2048];
    if (strlen(e) >= sizeof(buf)) {
        umm_log_error("nds: UMM_NDS_PRELOAD too long (>= %zu bytes)",
                      sizeof(buf));
        return UMM_E_NOT_FOUND;
    }
    strcpy(buf, e);

    for (char *tok = strtok(buf, ":"); tok; tok = strtok(NULL, ":")) {
        if (!tok[0])
            continue;
        pthread_mutex_lock(&g_reg_lock);
        int seen = 0;
        for (int i = 0; i < g_n_preloaded; i++)
            if (strcmp(g_preloaded[i], tok) == 0) { seen = 1; break; }
        pthread_mutex_unlock(&g_reg_lock);
        if (seen)
            continue;

        void *h = dlopen(tok, RTLD_NOW | RTLD_GLOBAL);
        if (!h) {
            umm_log_error(
                          "nds: UMM_NDS_PRELOAD dlopen(%s) failed: %s",
                          tok, dlerror());
            return UMM_E_NOT_FOUND;
        }
        umm_log_info("nds: preloaded %s (RTLD_NOW|RTLD_GLOBAL, "
                     "句柄常驻不 dlclose)", tok);
        pthread_mutex_lock(&g_reg_lock);
        if (g_n_preloaded < NDS_MAX_PRELOADED) {
            snprintf(g_preloaded[g_n_preloaded],
                     sizeof(g_preloaded[0]), "%s", tok);
            g_n_preloaded++;
        }
        pthread_mutex_unlock(&g_reg_lock);
    }
    return UMM_OK;
}

/* nvm_host RPC context 引导（双进程架构，见类型定义处注释）。
 * 必须在首个 open 的 nds_init 之前调用（调用方持 g_reg_lock）。
 * 符号解析顺序：先 dlh（libnds_aiv.so 或其 DT_NEEDED 传递导出），
 * 再 RTLD_DEFAULT（覆盖符号在 libnvm_host.so、经 UMM_NDS_PRELOAD 以
 * RTLD_GLOBAL 预载进全局命名空间的场景——libnds_aiv.so 以
 * RTLD_LOCAL dlopen，其句柄命名空间不一定含预载符号）。
 * 成功返回 UMM_OK 且 *out_ctx 非 NULL；失败返回错误码（已打日志）。
 * *out_disconnect 可选接收 nvm_host_rpc_disconnect 符号（同 bind 的
 * 查找顺序：NDS 库句柄 → RTLD_DEFAULT）；找不到记 NULL + debug 日志，
 * 不视为错误（老版本库可能没有该符号，close 时跳过断开）。 */
static int nds_rpc_bootstrap(void *dlh, const char *sock_path,
                             void **out_ctx,
                             fn_rpc_disconnect_t *out_disconnect)
{
    fn_bind_remote_t fn_bind =
        (fn_bind_remote_t)dlsym(dlh, "nvm_host_bind_remote");
    if (!fn_bind)
        fn_bind = (fn_bind_remote_t)dlsym(RTLD_DEFAULT,
                                          "nvm_host_bind_remote");
    fn_set_rpc_ctx_t fn_set =
        (fn_set_rpc_ctx_t)dlsym(dlh, "nvm_host_set_rpc_context");
    if (!fn_set)
        fn_set = (fn_set_rpc_ctx_t)dlsym(RTLD_DEFAULT,
                                         "nvm_host_set_rpc_context");
    if (!fn_bind || !fn_set) {
        umm_log_error(
                      "nds: UMM_NDS_RPC_SOCKET=%s 已设置但找不到符号 %s%s%s "
                      "(提供方 libnvm_host.so 未加载：把 libnvm_host.so 加入 "
                      "UMM_NDS_PRELOAD，或确认其已被 libnds_aiv.so 的 "
                      "DT_NEEDED 传递加载)", sock_path,
                      fn_bind ? "" : "nvm_host_bind_remote",
                      (!fn_bind && !fn_set) ? "/" : "",
                      fn_set ? "" : "nvm_host_set_rpc_context");
        return UMM_E_NOT_FOUND;
    }

    /* 可选断开符号：与 bind 同查找顺序（NDS 库句柄 → RTLD_DEFAULT），
     * 缺失不算错误——老版本 libnvm_host.so 可能不导出，close 时跳过
     * 断开（保持旧行为） */
    fn_rpc_disconnect_t fn_disconn =
        (fn_rpc_disconnect_t)dlsym(dlh, "nvm_host_rpc_disconnect");
    if (!fn_disconn)
        fn_disconn = (fn_rpc_disconnect_t)dlsym(RTLD_DEFAULT,
                                                "nvm_host_rpc_disconnect");
    if (!fn_disconn)
        umm_log_debug(
                      "nds: 可选符号 nvm_host_rpc_disconnect 未找到，"
                      "close 时将跳过 RPC 断开（老版本库？）");

    /* ---- bind 重试（env UMM_NDS_RPC_WAIT_MS，缺省 3000；0 = 不重试，
     * 保持原单次行为）。真机踩坑：RPC server（Process A）启动窗口期内
     * 客户端 bind 直接失败即死，三进程手工编排启动顺序极易踩中。
     * 指数退避 50→100→200→400→800ms 封顶；重试只针对 bind 失败
     * （可能是 server 尚未 listen 的暂态），符号缺失等确定性错误
     * 在上面已 fail-fast 返回，不进本循环。
     * 注：提供方库 bind 每次失败会自行打印 "Failed to connect..."，
     * 属可观测的暂态噪声，不抑制。 ---- */
    /* 注意不能用 env_u64（其语义是 0→缺省）：这里 0 是合法值
     *（关闭重试），必须显式解析 */
    uint64_t wait_ms = NDS_DEFAULT_RPC_WAIT_MS;
    const char *wait_env = getenv("UMM_NDS_RPC_WAIT_MS");
    if (wait_env && wait_env[0])
        wait_ms = (uint64_t)strtoull(wait_env, NULL, 10);
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    nvm_host_rpc_ctx_t ctx = NULL;
    nvm_host_error_t rc = 0;
    uint64_t backoff = 50;      /* ms */
    unsigned attempt = 0;
    for (;;) {
        attempt++;
        ctx = NULL;
        rc = fn_bind(sock_path, &ctx);
        if (rc == 0 && ctx)
            break;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t elapsed = nds_elapsed_ms(&t0, &now);
        if (wait_ms == 0 || elapsed >= wait_ms) {
            umm_log_error(
                          "nds: nvm_host_bind_remote(%s) failed rc=%d ctx=%p "
                          "(已等待 %lu ms/%lu ms 共 %u 次尝试；提供方 "
                          "Process A RPC server 未启动或 socket 路径错误："
                          "先启动持有 NVMe 控制器的 RPC server 进程，或由 "
                          "umms 配置 nds_rpc_server_enable 托管拉起；"
                          "UMM_NDS_RPC_WAIT_MS=0 关闭重试)",
                          sock_path, (int)rc, (void *)ctx,
                          (unsigned long)elapsed, (unsigned long)wait_ms,
                          attempt);
            return UMM_E_IO;
        }
        /* 退避不超过剩余预算 */
        uint64_t remaining = wait_ms - elapsed;
        uint64_t sleep_ms = backoff < remaining ? backoff : remaining;
        umm_log_debug(
                      "nds: nvm_host_bind_remote(%s) attempt %u failed "
                      "rc=%d, retry in %lu ms (remaining budget %lu ms)",
                      sock_path, attempt, (int)rc,
                      (unsigned long)sleep_ms, (unsigned long)remaining);
        struct timespec ts = { (time_t)(sleep_ms / 1000),
                               (long)(sleep_ms % 1000) * 1000000L };
        nanosleep(&ts, NULL);
        if (backoff < 800)
            backoff *= 2;
    }
    if (attempt > 1) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t elapsed = nds_elapsed_ms(&t0, &now);
        umm_log_info(
                     "nds: nvm_host_bind_remote(%s) succeeded after %u "
                     "attempts (%lu ms)", sock_path, attempt,
                     (unsigned long)elapsed);
    }
    fn_set(ctx);
    umm_log_info(
                 "nds: RPC context established via %s (ctx=%p), "
                 "NDS will use remote admin queue", sock_path, (void *)ctx);
    *out_ctx = ctx;
    if (out_disconnect)
        *out_disconnect = fn_disconn;
    return UMM_OK;
}

int ssd_nds_open(const char *spec, SsdNdsBackend **out)
{
    if (!spec || !out)
        return UMM_E_INVALID_ARG;

    /* 先剥离可选的 "+<base_off>" 窗口基址后缀（支持 0x 十六进制），
     * 再解析十进制 device_id（如 "0+0x40000000" → id=0, base=1GB） */
    char spec_buf[64];
    if (strlen(spec) >= sizeof(spec_buf))
        return UMM_E_INVALID_ARG;
    strcpy(spec_buf, spec);
    uint64_t base_off = 0;
    char *plus = strrchr(spec_buf, '+');
    if (plus) {
        *plus = '\0';
        base_off = strtoull(plus + 1, NULL, 0);
    }
    if (spec_buf[0] == '\0')
        return UMM_E_INVALID_ARG;
    uint32_t device_id = (uint32_t)strtoul(spec_buf, NULL, 10);

    /* 运行参数：env 可调，缺省见 NDS_DEFAULT_* */
    uint64_t queue_depth  = env_u64("UMM_NDS_QUEUE_DEPTH",
                                    NDS_DEFAULT_QUEUE_DEPTH);
    uint64_t core_num     = env_u64("UMM_NDS_CORE_NUM",
                                    NDS_DEFAULT_CORE_NUM);
    uint64_t page_size    = env_u64("UMM_NDS_PAGE_SIZE",
                                    NDS_DEFAULT_PAGE_SIZE);
    uint64_t max_page_num = env_u64("UMM_NDS_MAX_PAGE_NUM",
                                    NDS_DEFAULT_MAX_PAGE_NUM);
    /* 单次 IO 上限：与 init 的 max_page_num 解耦（后者是 NDS 内部
     * IO 资源池规模，见 NDS_DEFAULT_* 注释） */
    uint64_t max_io       = env_u64("UMM_NDS_MAX_IO",
                                    NDS_DEFAULT_MAX_IO);

    /* 预载显式声明的提供方库（解决 NDS 库未声明依赖，如
     * libnds_aiv.so 引用 libread-write_kernel.so 的 readwrite_demo
     * 但 DT_NEEDED 未声明它——LD_LIBRARY_PATH 无法解决此类问题） */
    int prc = nds_preload_from_env();
    if (prc != UMM_OK)
        return prc;

    /* dlopen：先环境变量，再系统库路径（与 libnvm 一致） */
    const char *env = getenv("UMM_NDS_PATH");
    const char *soname = env && env[0] ? env : NDS_DEFAULT_SONAME;
    void *dlh = dlopen(soname, RTLD_NOW | RTLD_LOCAL);
    if (!dlh) {
        const char *err = dlerror();
        if (err && strstr(err, "undefined symbol")) {
            /* 未定义符号（未声明依赖）：回退 RTLD_LAZY 尝试继续。
             * 根治方法是用 UMM_NDS_PRELOAD 预载符号提供方库。 */
            umm_log_warn(
                         "nds: dlopen(%s) RTLD_NOW 失败，未定义符号: %s "
                         "(建议用 UMM_NDS_PRELOAD 预载符号提供方库根治; "
                         "定位方法: ldd -r %s 查全部未定义符号, 再在提供方"
                         "库目录用 nm -D --defined-only 定位符号后预载)",
                         soname, err, soname);
            dlh = dlopen(soname, RTLD_LAZY | RTLD_LOCAL);
            if (dlh)
                umm_log_warn(
                             "nds: %s 已降级 RTLD_LAZY 懒加载成功；若运行期"
                             "真实调用到未定义符号将在调用点崩溃，请尽快用 "
                             "UMM_NDS_PRELOAD 根治", soname);
            else
                umm_log_error(
                              "nds: dlopen(%s) RTLD_LAZY 回退仍失败: %s",
                              soname, dlerror());
        } else {
            /* 非未定义符号类失败（库文件/传递依赖缺失等）不回退 */
            umm_log_error(
                          "nds: dlopen(%s) failed: %s "
                          "(若报错为其他 .so cannot open shared object file, "
                          "属传递依赖缺失: ldd 检查 libnds_aiv.so 并把缺失库"
                          "所在目录加入 LD_LIBRARY_PATH; 库本身路径用 "
                          "UMM_NDS_PATH 指定)", soname, err);
        }
        if (!dlh)
            return UMM_E_NOT_FOUND;
    }

    fn_instance_t  fn_instance =
        (fn_instance_t)load_symbol(dlh, "_ZN3NDS8InstanceEv");
    fn_init_t      fn_init =
        (fn_init_t)load_symbol(dlh, "_ZN3NDS8nds_initEjmmmm");
    fn_uninit_t    fn_uninit =
        (fn_uninit_t)load_symbol(dlh, "_ZN3NDS10nds_uninitEv");
    fn_register_t  fn_register =
        (fn_register_t)load_symbol(dlh, "_ZN3NDS12nds_registerEPvm");
    fn_single_io_t fn_single_write =
        (fn_single_io_t)load_symbol(dlh, "_ZN3NDS16nds_single_writeEPvmm");
    fn_single_io_t fn_single_read =
        (fn_single_io_t)load_symbol(dlh, "_ZN3NDS15nds_single_readEPvmm");
    fn_batch_io_t  fn_batch_write =
        (fn_batch_io_t)load_symbol(dlh, "_ZN3NDS15nds_batch_writeEP5IOVecm");
    fn_batch_io_t  fn_batch_read =
        (fn_batch_io_t)load_symbol(dlh, "_ZN3NDS14nds_batch_readEP5IOVecm");
    if (!fn_instance || !fn_init || !fn_uninit || !fn_register ||
        !fn_single_write || !fn_single_read ||
        !fn_batch_write || !fn_batch_read) {
        dlclose(dlh);
        return UMM_E_NOT_FOUND;
    }

    void *nds = fn_instance();
    if (!nds) {
        umm_log_error(
                      "nds: NDS::Instance() returned NULL");
        dlclose(dlh);
        return UMM_E_IO;
    }

    SsdNdsBackend *b = calloc(1, sizeof(*b));
    if (!b) {
        dlclose(dlh);
        return UMM_E_NO_MEMORY;
    }

    /* ---- device 注册表：同 device_id 复用单例，仅首个 open 调 nds_init。
     * nds_init 无返回值（void），无法感知失败——参数合法性由调用前
     * 校验保证。init 在注册表锁内执行，全局串行。 ---- */
    pthread_mutex_lock(&g_reg_lock);
    NdsDeviceEntry *entry = NULL;
    for (int i = 0; i < NDS_MAX_DEVICES; i++) {
        if (g_devs[i].used && g_devs[i].device_id == device_id) {
            entry = &g_devs[i];
            break;
        }
    }
    if (entry) {
        entry->refcount++;
        /* init 参数以首个 open 为准 */
        page_size    = entry->page_size;
        max_page_num = entry->max_page_num;
        umm_log_info(
                     "nds: device %u already initialized, refcount=%u",
                     device_id, entry->refcount);
    } else {
        for (int i = 0; i < NDS_MAX_DEVICES; i++) {
            if (!g_devs[i].used) {
                entry = &g_devs[i];
                break;
            }
        }
        if (!entry) {
            pthread_mutex_unlock(&g_reg_lock);
            umm_log_error(
                          "nds: device registry full (%d devices)",
                          NDS_MAX_DEVICES);
            dlclose(dlh);
            free(b);
            return UMM_E_NO_MEMORY;
        }
        /* 双进程架构 RPC 引导（env UMM_NDS_RPC_SOCKET 设置时）：
         * 必须在本进程首个 nds_init 之前 bind + set_rpc_context，
         * 否则 nds_init 内 nvm_host_admin_cq_create 报
         * "no RPC context"。同 device 后续 open（refcount++）
         * 复用 entry->rpc_ctx，不重复 bind */
        const char *rpc_sock = getenv("UMM_NDS_RPC_SOCKET");
        void *rpc_ctx = NULL;
        fn_rpc_disconnect_t rpc_disconn = NULL;
        if (rpc_sock && rpc_sock[0]) {
            int rrc = nds_rpc_bootstrap(dlh, rpc_sock, &rpc_ctx,
                                        &rpc_disconn);
            if (rrc != UMM_OK) {
                pthread_mutex_unlock(&g_reg_lock);
                dlclose(dlh);
                free(b);
                return rrc;
            }
        }
        fn_init(nds, device_id, (size_t)queue_depth, (size_t)core_num,
                page_size, max_page_num);
        entry->used         = 1;
        entry->device_id    = device_id;
        entry->refcount     = 1;
        entry->n_regions    = 0;
        memset(entry->regions, 0, sizeof(entry->regions));
        memset(entry->region_sizes, 0, sizeof(entry->region_sizes));
        entry->page_size    = page_size;
        entry->max_page_num = max_page_num;
        entry->rpc_ctx      = rpc_ctx;
        entry->rpc_disconnect = rpc_disconn;
        /* per-device I/O 锁随 entry 生命周期（见头部锁序注释） */
        pthread_mutex_init(&entry->io_lock, NULL);
    }
    pthread_mutex_unlock(&g_reg_lock);

    b->dlh             = dlh;
    b->nds             = nds;
    b->dev             = entry;
    b->device_id       = device_id;
    b->page_size       = page_size;
    b->max_io          = max_io;
    b->base_off        = base_off;
    b->fn_uninit       = fn_uninit;
    b->fn_register     = fn_register;
    b->fn_single_read  = fn_single_read;
    b->fn_single_write = fn_single_write;
    b->fn_batch_read   = fn_batch_read;
    b->fn_batch_write  = fn_batch_write;

    /* batch 单发模拟兜底（过渡措施，库 batch 修复后移除，见头注释）。
     * open 时读 env：已打开的 backend 不受后续 setenv/unsetenv 影响 */
    b->batch_write_emulate =
        env_u64("UMM_NDS_BATCH_WRITE_EMULATE", 0) != 0;
    b->batch_read_emulate  =
        env_u64("UMM_NDS_BATCH_READ_EMULATE", 0) != 0;
    if (b->batch_write_emulate)
        umm_log_info(
                     "nds: device %u UMM_NDS_BATCH_WRITE_EMULATE=1："
                     "batch_write 以单发循环模拟（库 batch_write 未实现/"
                     "异常时的过渡降级，性能低于真 batch）", device_id);
    if (b->batch_read_emulate)
        umm_log_info(
                     "nds: device %u UMM_NDS_BATCH_READ_EMULATE=1："
                     "batch_read 以单发循环模拟（库 batch_read 未实现/"
                     "异常时的过渡降级，性能低于真 batch）", device_id);

    /* 与 libnvm 的差异：open 不做 I/O 探针——探针需要已注册的
     * device 内存作为缓冲，open 阶段不存在合法缓冲，无法构造探针。 */
    umm_log_info(
                 "nds: opened device=%u, page=%lu, max_io=%lu KB, "
                 "base_off=0x%lx",
                 device_id, (unsigned long)page_size,
                 (unsigned long)(b->max_io / 1024),
                 (unsigned long)b->base_off);
    *out = b;
    return UMM_OK;
}

uint64_t ssd_nds_max_io(SsdNdsBackend *b)
{
    return b ? b->max_io : 0;
}

uint32_t ssd_nds_block_size(SsdNdsBackend *b)
{
    return b ? (uint32_t)b->page_size : 0;
}

int ssd_nds_register_mem(SsdNdsBackend *b, void *dev_mem,
                         uint64_t aligned_size)
{
    if (!b || !dev_mem || aligned_size == 0)
        return UMM_E_INVALID_ARG;
    if (aligned_size % b->page_size != 0) {
        umm_log_error(
                      "nds: register size %lu not aligned to page %lu",
                      (unsigned long)aligned_size,
                      (unsigned long)b->page_size);
        return UMM_E_INVALID_ARG;
    }

    /* nds_register 作用于进程级单例：登记到 device 注册表，
     * 同 device 的所有 backend 共享注册状态。
     * 多段注册：NDS 提供方真实测试代码连续 nds_register 多个独立
     * 内存段（样例为 4 段），追加段是 API 预期用法。
     * 锁序：g_reg_lock → entry->io_lock（全局唯一方向，无死锁） */
    pthread_mutex_lock(&g_reg_lock);
    /* 完全相同（base 与 size 均相同）的重复注册：幂等直接成功，
     * 不重复下发 fn_register（同 device 多 backend 各注册同一
     * arena 是池级 API 的自然调用形态） */
    for (uint32_t i = 0; i < b->dev->n_regions; i++) {
        if (b->dev->regions[i] == dev_mem &&
            b->dev->region_sizes[i] == aligned_size) {
            umm_log_warn(
                         "nds: device %u region (dev_mem=%p, size=%lu) "
                         "already registered, idempotent no-op",
                         b->device_id, dev_mem,
                         (unsigned long)aligned_size);
            pthread_mutex_unlock(&g_reg_lock);
            return UMM_OK;
        }
    }
    if (b->dev->n_regions >= NDS_MAX_REGIONS) {
        umm_log_error(
                      "nds: device %u region table full (%u regions)",
                      b->device_id, NDS_MAX_REGIONS);
        pthread_mutex_unlock(&g_reg_lock);
        return UMM_E_NO_MEMORY;
    }
    pthread_mutex_lock(&b->dev->io_lock);
    b->fn_register(b->nds, dev_mem, aligned_size);   /* void 返回 */
    pthread_mutex_unlock(&b->dev->io_lock);
    b->dev->regions[b->dev->n_regions]      = dev_mem;
    b->dev->region_sizes[b->dev->n_regions] = aligned_size;
    b->dev->n_regions++;
    pthread_mutex_unlock(&g_reg_lock);

    umm_log_info(
                 "nds: device %u registered segment #%u dev_mem=%p, "
                 "size=%lu MB",
                 b->device_id, b->dev->n_regions, dev_mem,
                 (unsigned long)(aligned_size / 1024 / 1024));
    return UMM_OK;
}

/* 读 dev->n_regions（无锁读：仅为调用前校验，良性竞争） */
static int nds_is_registered(SsdNdsBackend *b)
{
    return b->dev->n_regions > 0;
}

/* vaddr 区间校验（多段）：[vaddr, vaddr+len) 必须完整落在某一个
 * 已注册段 [regions[i], regions[i]+region_sizes[i]) 内——单条 IO
 * 不允许横跨两个注册段（即使地址相邻），否则 DMA 跨段语义未定义。
 * 真机传错 device 地址 = DMA 越界读写 HBM，且 NDS void 返回无感知，
 * 只能在调用前拦截。
 * 段表无锁读取（良性竞争，同 nds_is_registered）：调用方已先过
 * registered 校验，register 在程序序上先于 I/O。
 * 防溢出写法：全部转为"区间内偏移"比较，不做 vaddr+len 加法。 */
static int nds_vaddr_in_region(SsdNdsBackend *b, const void *vaddr,
                               uint64_t len)
{
    const uint8_t *p = (const uint8_t *)vaddr;
    for (uint32_t i = 0; i < b->dev->n_regions; i++) {
        const uint8_t *base = (const uint8_t *)b->dev->regions[i];
        uint64_t reg_size   = b->dev->region_sizes[i];
        if (p < base)
            continue;
        uint64_t off_in_region = (uint64_t)(p - base);
        if (off_in_region > reg_size)
            continue;
        if (len <= reg_size - off_in_region)
            return 1;   /* 完整落在该段内 */
    }
    return 0;
}

static int ssd_nds_io(SsdNdsBackend *b, uint64_t offset,
                      uint64_t len, void *dev_vaddr, int is_write)
{
    if (!b || (!dev_vaddr && len > 0))
        return UMM_E_INVALID_ARG;
    if (len == 0)
        return UMM_OK;

    /* NDS 全 void 返回：只能在调用前校验。offset/len 必须 page 对齐 */
    if ((offset % b->page_size) != 0 || (len % b->page_size) != 0) {
        umm_log_error(
                      "nds: %s offset=%lu len=%lu not aligned to page %lu",
                      is_write ? "write" : "read",
                      (unsigned long)offset, (unsigned long)len,
                      (unsigned long)b->page_size);
        return UMM_E_INVALID_ARG;
    }
    if (!nds_is_registered(b)) {
        umm_log_error(
                      "nds: device %u not registered "
                      "(ssd_backend_register_dev_mem required before I/O)",
                      b->device_id);
        return UMM_E_INVALID_ARG;
    }
    /* vaddr 必须落在已注册区间内（真机越界 = DMA 误读写 HBM，
     * void 返回无感知，只能调用前拦截） */
    if (!nds_vaddr_in_region(b, dev_vaddr, len)) {
        umm_log_error(
                      "nds: %s vaddr=%p len=%lu 未完整落在任一已注册段内"
                      "（device %u 共 %u 段）",
                      is_write ? "write" : "read", dev_vaddr,
                      (unsigned long)len, b->device_id,
                      b->dev->n_regions);
        return UMM_E_INVALID_ARG;
    }

    pthread_mutex_lock(&b->dev->io_lock);

    uint8_t *p = (uint8_t *)dev_vaddr;
    uint64_t done = 0;
    while (done < len) {
        uint64_t chunk = (len - done) > b->max_io ? b->max_io
                                                  : (len - done);
        uint64_t real_off = b->base_off + offset + done; /* 窗口内相对 → 物理 */
        if (is_write)
            b->fn_single_write(b->nds, p + done, chunk, real_off);
        else
            b->fn_single_read (b->nds, p + done, chunk, real_off);
        /* void 返回：无法感知错误（NDS API 固有限制，见头注释） */
        done += chunk;
    }

    pthread_mutex_unlock(&b->dev->io_lock);
    return UMM_OK;
}

int ssd_nds_read(SsdNdsBackend *b, uint64_t offset,
                 uint64_t len, void *dev_vaddr)
{
    return ssd_nds_io(b, offset, len, dev_vaddr, 0);
}

int ssd_nds_write(SsdNdsBackend *b, uint64_t offset,
                  uint64_t len, const void *dev_vaddr)
{
    /* nds_single_write 的 vaddr 非常量，语义上不会修改调用方数据 */
    return ssd_nds_io(b, offset, len, (void *)dev_vaddr, 1);
}

static int ssd_nds_batch(SsdNdsBackend *b, const UmmNdsIOVec *iovs,
                         size_t n_iov, int is_write)
{
    if (!b || (!iovs && n_iov > 0))
        return UMM_E_INVALID_ARG;
    if (n_iov == 0)
        return UMM_OK;          /* no-op */

    if (!nds_is_registered(b)) {
        umm_log_error(
                      "nds: device %u not registered "
                      "(ssd_backend_register_dev_mem required before I/O)",
                      b->device_id);
        return UMM_E_INVALID_ARG;
    }

    /* 逐 iov 调用前校验（void 返回，只能事前把关） */
    for (size_t i = 0; i < n_iov; i++) {
        if (!iovs[i].vaddr || iovs[i].length == 0) {
            umm_log_error(
                          "nds: batch iov[%zu] invalid vaddr/length", i);
            return UMM_E_INVALID_ARG;
        }
        if ((iovs[i].length % b->page_size) != 0 ||
            (iovs[i].offset % b->page_size) != 0) {
            umm_log_error(
                          "nds: batch iov[%zu] off=%lu len=%lu not "
                          "aligned to page %lu", i,
                          (unsigned long)iovs[i].offset,
                          (unsigned long)iovs[i].length,
                          (unsigned long)b->page_size);
            return UMM_E_INVALID_ARG;
        }
        if (iovs[i].length > b->max_io) {
            umm_log_error(
                          "nds: batch iov[%zu] len=%lu exceeds max_io %lu",
                          i, (unsigned long)iovs[i].length,
                          (unsigned long)b->max_io);
            return UMM_E_INVALID_ARG;
        }
        /* vaddr 必须完整落在某一个已注册段内（同单条路径，跨段拒绝） */
        if (!nds_vaddr_in_region(b, iovs[i].vaddr, iovs[i].length)) {
            umm_log_error(
                          "nds: batch iov[%zu] vaddr=%p len=%lu 未完整"
                          "落在任一已注册段内（device %u 共 %u 段）",
                          i, iovs[i].vaddr,
                          (unsigned long)iovs[i].length,
                          b->device_id, b->dev->n_regions);
            return UMM_E_INVALID_ARG;
        }
    }

    /* 拷贝临时数组加 base_off，不改调用方数组 */
    UmmNdsIOVec *tmp = malloc(n_iov * sizeof(*tmp));
    if (!tmp)
        return UMM_E_NO_MEMORY;
    memcpy(tmp, iovs, n_iov * sizeof(*tmp));
    for (size_t i = 0; i < n_iov; i++)
        tmp[i].offset += b->base_off;

    pthread_mutex_lock(&b->dev->io_lock);
    /* 兜底开关（过渡措施，见头注释）：不走库 batch 入口，逐 iov 循环
     * 单发。语义差异：非原子（中断时前部 iov 已落盘）、无批量提交
     * （性能低于真 batch）；iov.length <= max_io 上面已校验，单条
     * 单发无需再分段；tmp[i].offset 已加 base_off（同真 batch 路径） */
    if ((is_write && b->batch_write_emulate) ||
        (!is_write && b->batch_read_emulate)) {
        for (size_t i = 0; i < n_iov; i++) {
            if (is_write)
                b->fn_single_write(b->nds, tmp[i].vaddr, tmp[i].length,
                                   tmp[i].offset);
            else
                b->fn_single_read (b->nds, tmp[i].vaddr, tmp[i].length,
                                   tmp[i].offset);
            /* void 返回：无法感知错误（NDS API 固有限制） */
        }
    } else if (is_write) {
        b->fn_batch_write(b->nds, tmp, n_iov);
    } else {
        b->fn_batch_read (b->nds, tmp, n_iov);
    }
    pthread_mutex_unlock(&b->dev->io_lock);

    free(tmp);
    return UMM_OK;
}

int ssd_nds_batch_read(SsdNdsBackend *b, UmmNdsIOVec *iovs, size_t n_iov)
{
    return ssd_nds_batch(b, iovs, n_iov, 0);
}

int ssd_nds_batch_write(SsdNdsBackend *b, const UmmNdsIOVec *iovs,
                        size_t n_iov)
{
    return ssd_nds_batch(b, iovs, n_iov, 1);
}

void ssd_nds_close(SsdNdsBackend *b)
{
    if (!b)
        return;

    /* 锁内减引用计数，归零调 nds_uninit（最后一个 close）。
     * per-device io_lock 随 entry 生命周期：uninit 之后、entry
     * 复位之前 destroy——此后该 device 不再有任何 NDS 调用 */
    pthread_mutex_lock(&g_reg_lock);
    NdsDeviceEntry *entry = b->dev;
    if (entry && entry->used) {
        entry->refcount--;
        if (entry->refcount == 0) {
            b->fn_uninit(b->nds);           /* void 返回 */
            /* RPC 连接断开（防真实 RPC server 侧连接泄漏）。顺序关键：
             *   1) 先 fn_uninit 停 NDS 队列（此后不再用 admin queue）；
             *   2) 再 rpc_disconnect 释放 server 侧连接；
             *   3) 本函数末尾才 dlclose 主库——rpc_disconnect 符号若
             *      解析自 NDS 主库句柄，dlclose 之后调用即悬野指针；
             *      若解析自 PRELOAD 的 libnvm_host.so（常驻不 dlclose）
             *      则天然安全。两种来源都要求在此点调用。
             * rpc_disconnect 为可选符号：NULL（老库未导出或未曾 bind）
             * 时跳过。注意 uninit/disconnect 后若再 open 同 device 且
             * UMM_NDS_RPC_SOCKET 仍设置，将重新 bind 得到新 ctx */
            if (entry->rpc_ctx && entry->rpc_disconnect) {
                entry->rpc_disconnect(entry->rpc_ctx);
                umm_log_info(
                             "nds: RPC disconnected (ctx=%p, device %u "
                             "last close)", entry->rpc_ctx,
                             entry->device_id);
            }
            entry->rpc_ctx        = NULL;
            entry->rpc_disconnect = NULL;
            pthread_mutex_destroy(&entry->io_lock);
            entry->used       = 0;
            entry->n_regions  = 0;
            memset(entry->regions, 0, sizeof(entry->regions));
            memset(entry->region_sizes, 0, sizeof(entry->region_sizes));
            umm_log_info(
                         "nds: device %u uninitialized (last close)",
                         entry->device_id);
        }
    }
    pthread_mutex_unlock(&g_reg_lock);

    if (b->dlh)
        dlclose(b->dlh);
    free(b);
}
