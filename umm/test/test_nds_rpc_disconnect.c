/*
 * test_nds_rpc_disconnect.c — NDS RPC 连接断开（close 时 rpc_disconnect）
 *
 * 配合 test/stub_nvm_host_rpc.c 编出的桩库，验证 ssd_backend_nds 的
 * RPC 连接生命周期（真机实证故障：close 从不断开 → server 侧连接泄漏）：
 *   single    open(bind 成功) → close → disconnect 恰好 1 次且 ctx 匹配
 *   shared    双 backend 同 device（refcount）：open A、open B、
 *             close A（不断开）→ close B（断开 1 次）——仅 last close 断开
 *   nodisconn 桩库不导出 nvm_host_rpc_disconnect（-DSTUB_NO_DISCONNECT
 *             变体，模拟老版本库）→ close 正常完成、无崩溃、无调用
 *   nosock    不设 UMM_NDS_RPC_SOCKET → 无 disconnect 调用（零回归）
 *
 * 每个模式独立进程运行（preload 的桩库符号在进程内不可卸载，
 * nodisconn 变体必须与完整桩库隔离）。
 *
 * 用法: test_nds_rpc_disconnect <mode> <libnds_aiv.so> <stub.so> <sock>
 * 退出码 0=全部断言通过，1=失败
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../src/transport/ssd_backend_nds.h"
#include "../include/umm.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do {                                   \
    if (cond) { g_pass++; printf("  [PASS] %s\n", msg); }       \
    else      { g_fail++; printf("  [FAIL] %s\n", msg); }       \
} while (0)

/* 测试自建 AF_UNIX listener：桩库 bind_remote 走真实 connect()，
 * listen backlog 即可让 connect 成功，无需 accept 线程 */
static int make_listener(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    unlink(path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(fd, 8) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

typedef int (*fn_stub_count_t)(void);

/* 桩库经 UMM_NDS_PRELOAD 以 RTLD_GLOBAL 进入全局命名空间，
 * 查询函数从 RTLD_DEFAULT 解析 */
static fn_stub_count_t stub_query(const char *name)
{
    return (fn_stub_count_t)dlsym(RTLD_DEFAULT, name);
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s <single|shared|nodisconn|nosock> "
                "<libnds_aiv.so> <stub.so> <sock>\n", argv[0]);
        return 1;
    }
    const char *mode  = argv[1];
    int use_sock = strcmp(mode, "nosock") != 0;

    int lfd = -1;
    if (use_sock) {
        lfd = make_listener(argv[4]);
        if (lfd < 0) {
            fprintf(stderr, "listener(%s) failed\n", argv[4]);
            return 1;
        }
    }

    setenv("UMM_NDS_PATH", argv[2], 1);
    setenv("UMM_NDS_PRELOAD", argv[3], 1);
    unsetenv("UMM_NDS_RPC_WAIT_MS");
    if (use_sock)
        setenv("UMM_NDS_RPC_SOCKET", argv[4], 1);
    else
        unsetenv("UMM_NDS_RPC_SOCKET");

    printf("[test_nds_rpc_disconnect] mode=%s\n", mode);

    SsdNdsBackend *a = NULL, *b = NULL;
    int rc;

    if (strcmp(mode, "single") == 0) {
        rc = ssd_nds_open("0", &a);
        CHECK(rc == UMM_OK && a, "single: open ok (RPC bind)");
        ssd_nds_close(a);
        fn_stub_count_t q_count = stub_query("nvm_host_stub_disconnect_count");
        fn_stub_count_t q_match =
            stub_query("nvm_host_stub_disconnect_matched");
        CHECK(q_count && q_match, "single: stub query syms resolved");
        if (q_count && q_match) {
            CHECK(q_count() == 1, "single: disconnect called exactly once");
            CHECK(q_match() == 1, "single: disconnect ctx matches bind ctx");
        }
    } else if (strcmp(mode, "shared") == 0) {
        rc = ssd_nds_open("0", &a);
        CHECK(rc == UMM_OK && a, "shared: open A ok");
        rc = ssd_nds_open("0", &b);
        CHECK(rc == UMM_OK && b, "shared: open B ok (refcount share)");
        ssd_nds_close(a);
        fn_stub_count_t q_count = stub_query("nvm_host_stub_disconnect_count");
        fn_stub_count_t q_match =
            stub_query("nvm_host_stub_disconnect_matched");
        CHECK(q_count && q_match, "shared: stub query syms resolved");
        if (q_count && q_match) {
            CHECK(q_count() == 0,
                  "shared: close A (not last) does NOT disconnect");
            ssd_nds_close(b);
            CHECK(q_count() == 1,
                  "shared: close B (last) disconnects exactly once");
            CHECK(q_match() == 1,
                  "shared: disconnect ctx matches bind ctx");
        } else {
            ssd_nds_close(b);
        }
    } else if (strcmp(mode, "nodisconn") == 0) {
        /* 桩库变体不导出 nvm_host_rpc_disconnect：close 必须正常完成 */
        rc = ssd_nds_open("0", &a);
        CHECK(rc == UMM_OK && a, "nodisconn: open ok (symbol optional)");
        ssd_nds_close(a);
        CHECK(1, "nodisconn: close completed without crash");
        fn_stub_count_t q_count = stub_query("nvm_host_stub_disconnect_count");
        CHECK(q_count && q_count() == 0,
              "nodisconn: no disconnect call (symbol absent)");
    } else if (strcmp(mode, "nosock") == 0) {
        rc = ssd_nds_open("0", &a);
        CHECK(rc == UMM_OK && a, "nosock: open ok (no RPC bootstrap)");
        ssd_nds_close(a);
        fn_stub_count_t q_count = stub_query("nvm_host_stub_disconnect_count");
        CHECK(q_count && q_count() == 0,
              "nosock: no disconnect call (zero regression)");
    } else {
        fprintf(stderr, "unknown mode %s\n", mode);
        if (lfd >= 0) {
            close(lfd);
            unlink(argv[4]);
        }
        return 1;
    }

    if (lfd >= 0) {
        close(lfd);
        unlink(argv[4]);
    }
    printf("[test_nds_rpc_disconnect] mode=%s pass=%d fail=%d\n",
           mode, g_pass, g_fail);
    return g_fail ? 1 : 0;
}
