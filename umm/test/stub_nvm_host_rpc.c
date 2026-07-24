/*
 * stub_nvm_host_rpc.c — libnvm_host.so 的 RPC 双侧面测试桩
 *
 * 同一库同时导出 server 侧与 client 侧符号，用于在没有真实
 * libnvm/硬件的环境中测试 NDS 双进程 RPC 架构：
 *   server 侧（umm_nds_rpc_server 使用）：
 *     nvm_host_init              返回哑 ctx（不碰硬件）
 *     nvm_host_enable_rpc_server 真 AF_UNIX socket bind+listen +
 *                                acceptor 线程（accept 即 close，
 *                                支撑 umms wait_ready 的 connect 探测
 *                                与客户端 bind_remote 连接）
 *     nvm_host_free              关监听、join acceptor、释放 ctx
 *   client 侧（ssd_backend_nds RPC 引导使用）：
 *     nvm_host_bind_remote       connect() 到 socket，成功给假 ctx
 *     nvm_host_set_rpc_context   记录 ctx（日志可观测）
 *     nvm_host_rpc_disconnect    关连接 fd、记录调用次数/ctx 匹配
 *                                （-DSTUB_NO_DISCONNECT 编译变体不导出
 *                                该符号，模拟老版本库）
 *   测试断言查询（始终导出）：
 *     nvm_host_stub_disconnect_count()    disconnect 被调次数
 *     nvm_host_stub_disconnect_matched()  末次 disconnect 的 ctx 是否
 *                                         为本进程 bind 发出过的活 ctx
 *
 * 故障注入（测试 bind 重试）：
 *   STUB_BIND_FAIL_FIRST_N=N   本进程前 N 次 bind_remote 返回 -6
 *                              （模拟 server 启动窗口期的暂态失败），
 *                              之后走真实 connect
 *
 * 构建: gcc -shared -fPIC -o libnvm_host_rpc_stub.so stub_nvm_host_rpc.c
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

typedef struct nvm_host_ctx {
    int             listen_fd;
    pthread_t       acceptor;
    int             acceptor_started;
    char            sock_path[108];
} nvm_host_ctx_t;

typedef void *nvm_host_rpc_ctx_t;

/* ---- disconnect 测试断言状态：bind 发出的活 ctx 小表 + 计数 ---- */
#define STUB_MAX_LIVE_CTX 16
static void *g_live_ctx[STUB_MAX_LIVE_CTX];
static int   g_disconn_count;
static int   g_disconn_matched;     /* 末次 disconnect ctx 是否为活 ctx */

static void register_live_ctx(void *ctx)
{
    for (int i = 0; i < STUB_MAX_LIVE_CTX; i++)
        if (!g_live_ctx[i]) { g_live_ctx[i] = ctx; return; }
}

static int unregister_live_ctx(void *ctx)
{
    for (int i = 0; i < STUB_MAX_LIVE_CTX; i++)
        if (g_live_ctx[i] == ctx) { g_live_ctx[i] = NULL; return 1; }
    return 0;
}

/* ---- server 侧 ---- */

static volatile sig_atomic_t g_srv_stop = 0;

static void *acceptor_main(void *arg)
{
    nvm_host_ctx_t *ctx = arg;
    /* poll 轮询（100ms 超时检查 stop 标志）：Linux 上另一线程 close
     * listen_fd 不会唤醒阻塞中的 accept，不能直接裸 accept 否则
     * nvm_host_free 的 pthread_join 永久挂起 */
    while (!g_srv_stop) {
        struct pollfd pfd = { .fd = ctx->listen_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 100);
        if (pr <= 0)
            continue;
        int fd = accept(ctx->listen_fd, NULL, NULL);
        if (fd >= 0)
            close(fd);          /* 探测/客户端连接：accept 即完成使命 */
    }
    return NULL;
}

int nvm_host_init(const char *ctrl_path, uint32_t ns_id,
                  size_t queue_depth, nvm_host_ctx_t **ctx)
{
    (void)queue_depth;
    if (getenv("STUB_INIT_FAIL"))
        return -3;
    nvm_host_ctx_t *c = calloc(1, sizeof(*c));
    if (!c)
        return -1;
    c->listen_fd = -1;
    *ctx = c;
    fprintf(stderr, "[stub_nvm_host_rpc] nvm_host_init(ctrl=%s, ns=%u) ok\n",
            ctrl_path, ns_id);
    return 0;
}

int nvm_host_enable_rpc_server(void *vctx, const char *sock_path)
{
    nvm_host_ctx_t *ctx = vctx;
    if (!ctx || !sock_path)
        return -1;
    if (strlen(sock_path) >= sizeof(ctx->sock_path))
        return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -4;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, sock_path);
    unlink(sock_path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(fd, 16) != 0) {
        fprintf(stderr, "[stub_nvm_host_rpc] bind/listen(%s) failed: %s\n",
                sock_path, strerror(errno));
        close(fd);
        return -4;
    }
    ctx->listen_fd = fd;
    strcpy(ctx->sock_path, sock_path);
    if (pthread_create(&ctx->acceptor, NULL, acceptor_main, ctx) == 0)
        ctx->acceptor_started = 1;
    fprintf(stderr, "[stub_nvm_host_rpc] rpc server listening on %s\n",
            sock_path);
    /* 可选：模拟 server 启动窗口期（listen 前人为延迟由调用方控制，
     * 这里支持 listen 后延迟生效无意义，略） */
    return 0;
}

void nvm_host_free(nvm_host_ctx_t *ctx)
{
    if (!ctx)
        return;
    if (ctx->listen_fd >= 0) {
        g_srv_stop = 1;
        if (ctx->acceptor_started)
            pthread_join(ctx->acceptor, NULL);
        close(ctx->listen_fd);
    }
    free(ctx);
    fprintf(stderr, "[stub_nvm_host_rpc] nvm_host_free done\n");
}

/* ---- client 侧 ---- */

int nvm_host_bind_remote(const char *sock_path, nvm_host_rpc_ctx_t *out_ctx)
{
    static _Atomic int g_bind_calls = 0;
    int n = ++g_bind_calls;
    const char *e = getenv("STUB_BIND_FAIL_FIRST_N");
    int fail_first = e ? atoi(e) : 0;
    if (n <= fail_first) {
        fprintf(stderr, "[stub_nvm_host_rpc] bind_remote attempt %d "
                "injected failure (-6)\n", n);
        return -6;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -6;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "Failed to connect to %s: %s\n",
                sock_path, strerror(errno));
        close(fd);
        return -6;
    }
    /* 假 ctx：用连接 fd 装箱，有效至 nvm_host_rpc_disconnect 关闭 */
    *out_ctx = (void *)(intptr_t)(fd + 1);
    register_live_ctx(*out_ctx);
    fprintf(stderr, "[stub_nvm_host_rpc] bind_remote(%s) ok ctx=%p\n",
            sock_path, *out_ctx);
    return 0;
}

void nvm_host_set_rpc_context(nvm_host_rpc_ctx_t ctx)
{
    fprintf(stderr, "[stub_nvm_host_rpc] set_rpc_context(ctx=%p)\n", ctx);
}

#ifndef STUB_NO_DISCONNECT
void nvm_host_rpc_disconnect(nvm_host_rpc_ctx_t ctx)
{
    g_disconn_count++;
    g_disconn_matched = unregister_live_ctx(ctx);
    int fd = (int)(intptr_t)ctx - 1;    /* bind 的 fd+1 装箱逆运算 */
    if (fd >= 0)
        close(fd);
    fprintf(stderr, "[stub_nvm_host_rpc] rpc_disconnect(ctx=%p) "
            "count=%d matched=%d\n", ctx, g_disconn_count,
            g_disconn_matched);
}
#endif /* STUB_NO_DISCONNECT */

/* ---- 测试断言查询（STUB_NO_DISCONNECT 变体同样导出） ---- */
int nvm_host_stub_disconnect_count(void)
{
    return g_disconn_count;
}

int nvm_host_stub_disconnect_matched(void)
{
    return g_disconn_matched;
}
