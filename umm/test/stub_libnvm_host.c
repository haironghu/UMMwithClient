/*
 * stub_libnvm_host.c — libnvm_host.so 的测试桩
 *
 * 用常规文件模拟 NVMe 控制器，实现与 nvm_host.h 相同的 6 个符号，
 * 用于在没有真实 libnvm/硬件的环境中测试 ssd_backend_libnvm。
 *
 * 构建: gcc -shared -fPIC -o libnvm_host.so stub_libnvm_host.c
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>

#define STUB_MAX_IO   (64 * 1024)      /* 单次 I/O 上限 64KB */
#define STUB_DISK     "/tmp/libnvm_stub_disk.raw"

typedef struct nvm_host_ctx {
    int      fd;
    uint32_t ns_id;
    uint64_t id;        /* 进程内 init 序号（故障注入用） */
} nvm_host_ctx_t;

static uint64_t g_ctx_seq = 0;

/* 故障注入（测试用）：
 *   STUB_SEQ_RESET=1        下次 init 时序号归零（之后自动失效）
 *   STUB_READ_FAIL_ON_IDS   逗号分隔的 id 列表，命中则 read 失败
 *                           （模拟真库同进程多 init 后 ctx 静默损坏） */
static int id_in_fail_list(uint64_t id)
{
    const char *env = getenv("STUB_READ_FAIL_ON_IDS");
    if (!env || !env[0])
        return 0;
    char buf[128];
    strncpy(buf, env, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *save = NULL;
    for (char *t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        if (strtoull(t, NULL, 10) == id)
            return 1;
    }
    return 0;
}

typedef struct {
    uint32_t ns_id;
    size_t   page_size;
    size_t   max_data_size;
    size_t   block_size;
} nvm_host_disk_info_t;

int nvm_host_init(const char *ctrl_path, uint32_t ns_id,
                  size_t queue_depth, nvm_host_ctx_t **ctx)
{
    (void)ctrl_path; (void)queue_depth;
    if (getenv("STUB_SEQ_RESET")) {
        g_ctx_seq = 0;
        unsetenv("STUB_SEQ_RESET");   /* 只重置一次 */
    }
    int fd = open(STUB_DISK, O_RDWR | O_CREAT, 0644);
    if (fd < 0)
        return -1;
    /* 模拟设备语义：盘任何 LBA 恒可读。稀疏扩展到 1GB，
     * 否则空文件 pread 返回 0（短读），开机只读探针会误判 */
    ftruncate(fd, 1LL << 30);
    nvm_host_ctx_t *c = calloc(1, sizeof(*c));
    c->fd = fd;
    c->ns_id = ns_id;
    c->id = g_ctx_seq++;
    *ctx = c;
    return 0;
}

void nvm_host_free(nvm_host_ctx_t *ctx)
{
    if (!ctx) return;
    close(ctx->fd);
    free(ctx);
}

int nvm_host_get_disk_info(nvm_host_ctx_t *ctx, nvm_host_disk_info_t *info)
{
    if (!ctx || !info) return -1;
    info->ns_id         = ctx->ns_id;
    info->page_size     = 4096;
    info->max_data_size = STUB_MAX_IO;
    info->block_size    = 4096;
    return 0;
}

int nvm_host_write(nvm_host_ctx_t *ctx, void *data, size_t size,
                   uint64_t ssd_offset, size_t *written)
{
    if (!ctx || !data) return -1;
    if (size > STUB_MAX_IO) return -5;
    ssize_t n = pwrite(ctx->fd, data, size, (off_t)ssd_offset);
    if (n < 0) return -5;
    *written = (size_t)n;
    return 0;
}

int nvm_host_read(nvm_host_ctx_t *ctx, void *data, size_t size,
                  uint64_t ssd_offset, size_t *read)
{
    if (!ctx || !data) return -1;
    if (id_in_fail_list(ctx->id)) return -6;   /* 故障注入：静默损坏的 ctx */
    if (size > STUB_MAX_IO) return -5;
    ssize_t n = pread(ctx->fd, data, size, (off_t)ssd_offset);
    if (n < 0) return -5;
    *read = (size_t)n;
    return 0;
}

bool nvm_host_is_initialized(nvm_host_ctx_t *ctx)
{
    return ctx != NULL;
}
