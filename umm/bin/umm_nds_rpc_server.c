/* ========================================================================
 * umm_nds_rpc_server -- NDS 双进程架构的 Process A（配套守护进程）
 *
 * 角色：本地持有 NVMe 控制器（nvm_host_init），并对外提供 RPC admin
 * queue 服务（nvm_host_enable_rpc_server）。UMM 侧 NDS 客户端进程
 * （ummd/umms，env UMM_NDS_RPC_SOCKET 引导）经 bind_remote +
 * set_rpc_context 连接本进程后方可 nds_init。
 *
 * 部署形态（三进程）：
 *   umm_nds_rpc_server  →  ummD/umms（NDS 客户端）  →  UMM 客户端
 *
 * 依赖：仅 -ldl -pthread（dlopen 软依赖 libnvm_host.so，同
 * src/transport/ssd_backend_libnvm.c 的加载策略）。
 *
 * 退出码约定：
 *   0  正常退出（SIGINT/SIGTERM）
 *   1  参数错误
 *   2  dlopen/dlsym libnvm_host.so 失败（缺符号）
 *   3  nvm_host_init 失败
 *   4  nvm_host_enable_rpc_server 失败
 * ======================================================================== */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LOG_PREFIX "[umm-nds-rpc-server]"
#define LOGI(fmt, ...) fprintf(stderr, LOG_PREFIX " [info] " fmt "\n", ##__VA_ARGS__)
#define LOGW(fmt, ...) fprintf(stderr, LOG_PREFIX " [warn] " fmt "\n", ##__VA_ARGS__)
#define LOGE(fmt, ...) fprintf(stderr, LOG_PREFIX " [error] " fmt "\n", ##__VA_ARGS__)

#define LIBNVM_DEFAULT_SONAME  "libnvm_host.so"
#define DEFAULT_CTRL           "/dev/libnvm_helper0"
#define DEFAULT_NS             1
#define DEFAULT_QD             64
#define DEFAULT_SOCKET         "/tmp/nvm_host_rpc.sock"

/* ---- libnvm_host 公开 API 的本地声明（与 nvm_host.h 一致）---- */
typedef struct nvm_host_ctx nvm_host_ctx_t;
typedef void *nvm_host_rpc_ctx_t;
typedef int   nvm_host_error_t;   /* NVM_HOST_OK = 0 */

typedef int  (*fn_init_t)(const char *, uint32_t, size_t, nvm_host_ctx_t **);
typedef void (*fn_free_t)(nvm_host_ctx_t *);
typedef nvm_host_error_t (*fn_enable_rpc_t)(void *, const char *);
typedef void (*fn_rpc_disconnect_t)(nvm_host_rpc_ctx_t);

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "用法: %s [--ctrl PATH] [--ns N] [--qd N] [--socket PATH] [--help]\n"
        "\n"
        "NDS 双进程架构 Process A：持有 NVMe 控制器并提供 RPC admin queue 服务。\n"
        "\n"
        "选项（env 等价物 / 缺省）：\n"
        "  --ctrl PATH    NVMe 控制器设备路径\n"
        "                 (env UMM_NVM_CTRL, 缺省 %s)\n"
        "  --ns N         namespace id\n"
        "                 (env UMM_NVM_NS, 缺省 %d)\n"
        "  --qd N         admin queue 深度\n"
        "                 (env UMM_NVM_QD, 缺省 %d)\n"
        "  --socket PATH  RPC 监听 unix socket 路径\n"
        "                 (env UMM_NVM_RPC_SOCKET, 缺省 %s)\n"
        "  --help         打印本说明\n"
        "\n"
        "库加载：先 UMM_LIBNVM_PATH 完整路径，再系统库路径 %s\n"
        "（与 UMM libnvm 后端一致，RTLD_NOW|RTLD_LOCAL）。\n"
        "\n"
        "UMM 客户端侧（Process B，ummd/umms）对照 env：\n"
        "  UMM_NDS_RPC_SOCKET  须等于本进程的 --socket\n"
        "  UMM_NDS_PATH        libnds_aiv.so 路径\n"
        "  UMM_NDS_PRELOAD     预载符号提供方库（如客户端版 libnvm_host.so）\n"
        "\n"
        "示例：\n"
        "  %s --ctrl /dev/libnvm_helper0 --ns 1 --socket /tmp/nvm_host_rpc.sock\n"
        "  UMM_NDS_RPC_SOCKET=/tmp/nvm_host_rpc.sock ./umms -c config/umms_ssd_nds.yaml\n",
        prog, DEFAULT_CTRL, DEFAULT_NS, DEFAULT_QD, DEFAULT_SOCKET,
        LIBNVM_DEFAULT_SONAME, prog);
}

int main(int argc, char **argv)
{
    const char *ctrl   = getenv("UMM_NVM_CTRL");
    const char *sock   = getenv("UMM_NVM_RPC_SOCKET");
    const char *env_ns = getenv("UMM_NVM_NS");
    const char *env_qd = getenv("UMM_NVM_QD");
    uint32_t ns = env_ns ? (uint32_t)strtoul(env_ns, NULL, 10) : DEFAULT_NS;
    size_t   qd = env_qd ? (size_t)strtoul(env_qd, NULL, 10) : DEFAULT_QD;
    if (!ctrl || !ctrl[0]) ctrl = DEFAULT_CTRL;
    if (!sock || !sock[0]) sock = DEFAULT_SOCKET;

    static const struct option long_opts[] = {
        {"ctrl",   required_argument, NULL, 'c'},
        {"ns",     required_argument, NULL, 'n'},
        {"qd",     required_argument, NULL, 'q'},
        {"socket", required_argument, NULL, 's'},
        {"help",   no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "c:n:q:s:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'c': ctrl = optarg; break;
        case 'n': ns = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'q': qd = (size_t)strtoul(optarg, NULL, 10); break;
        case 's': sock = optarg; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }
    if (ns == 0 || qd == 0) {
        LOGE("ns/qd 必须为正整数 (ns=%u qd=%zu)", ns, qd);
        return 1;
    }

    LOGI("config: ctrl=%s ns=%u qd=%zu socket=%s", ctrl, ns, qd, sock);

    /* ---- dlopen libnvm_host.so：先 UMM_LIBNVM_PATH，再系统库路径 ---- */
    const char *env_lib = getenv("UMM_LIBNVM_PATH");
    const char *lib = (env_lib && env_lib[0]) ? env_lib : LIBNVM_DEFAULT_SONAME;
    void *dlh = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
    if (!dlh) {
        LOGE("dlopen(%s) failed: %s "
             "(传递依赖缺失时 ldd 检查并设置 LD_LIBRARY_PATH; "
             "库本身路径用 UMM_LIBNVM_PATH 指定)", lib, dlerror());
        return 2;
    }
    fn_init_t fn_init = (fn_init_t)dlsym(dlh, "nvm_host_init");
    fn_free_t fn_free = (fn_free_t)dlsym(dlh, "nvm_host_free");
    fn_enable_rpc_t fn_enable =
        (fn_enable_rpc_t)dlsym(dlh, "nvm_host_enable_rpc_server");
    /* rpc_disconnect 可选：server 侧不持有 rpc_ctx（见退出清理注释），
     * 仅记录可用性 */
    fn_rpc_disconnect_t fn_rpc_disconnect =
        (fn_rpc_disconnect_t)dlsym(dlh, "nvm_host_rpc_disconnect");
    if (!fn_init || !fn_free || !fn_enable) {
        LOGE("libnvm_host.so 缺必需符号: init=%p free=%p enable_rpc_server=%p "
             "(需要 nvm_host_init/nvm_host_free/nvm_host_enable_rpc_server)",
             (void *)fn_init, (void *)fn_free, (void *)fn_enable);
        dlclose(dlh);
        return 2;
    }
    LOGI("loaded %s (rpc_disconnect symbol: %s)", lib,
         fn_rpc_disconnect ? "present" : "absent");

    /* ---- stale socket 处理（真机实证会有残留）---- */
    if (access(sock, F_OK) == 0) {
        LOGW("stale socket %s exists, unlinking", sock);
        if (unlink(sock) != 0) {
            LOGE("unlink(%s) failed: %s", sock, strerror(errno));
            dlclose(dlh);
            return 4;
        }
    }

    /* ---- 启动序列 ---- */
    nvm_host_ctx_t *ctx = NULL;
    int rc = fn_init(ctrl, ns, qd, &ctx);
    if (rc != 0 || !ctx) {
        LOGE("nvm_host_init(%s, ns=%u, qd=%zu) failed rc=%d ctx=%p "
             "(检查控制器路径/访问权限/libnvm_helper 内核模块是否加载)",
             ctrl, ns, qd, rc, (void *)ctx);
        dlclose(dlh);
        return 3;
    }
    LOGI("nvm_host_init ok: ctrl=%s ns=%u qd=%zu ctx=%p", ctrl, ns, qd, (void *)ctx);

    nvm_host_error_t err = fn_enable(ctx, sock);
    if (err != 0) {
        LOGE("nvm_host_enable_rpc_server(ctx, %s) failed rc=%d", sock, (int)err);
        fn_free(ctx);
        dlclose(dlh);
        return 4;
    }
    LOGI("nvm_host_enable_rpc_server ok: socket=%s", sock);

    printf("RPC server ready on %s (ctrl=%s, ns=%u)\n", sock, ctrl, ns);
    fflush(stdout);

    /* ---- 常驻：SIGINT/SIGTERM 置标志后退出 pause 循环 ---- */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    while (!g_stop)
        pause();

    /* ---- 退出清理 ----
     * nvm_host_rpc_disconnect 仅适用于客户端侧 bind_remote 产出的
     * rpc_ctx；server 侧（本进程）只有 host ctx，无 rpc_ctx 可断开，
     * 故即使符号存在也不在 server 侧调用（真机语义待确认）。 */
    LOGI("signal received, shutting down");
    fn_free(ctx);
    LOGI("nvm_host_free done");
    if (unlink(sock) != 0 && errno != ENOENT)
        LOGW("unlink(%s) failed: %s", sock, strerror(errno));
    dlclose(dlh);
    LOGI("exit ok");
    return 0;
}
