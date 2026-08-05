/* ========================================================================
 * transport_remote.c — 远端数据面 transport（Phase 1 实现）
 *
 * 形态：同步 stop-and-wait。每次 I/O = 1 个 RTT + 属主机落盘时间。
 * 进程内一把大锁串行化所有远端 I/O（与同步语义一致；流水化属 Phase 2）。
 *
 * 断线策略：传输错误 → 断开重连并重试一次；业务错误（status != OK）
 * 立即透传，不重试。
 * ======================================================================== */

#include "transport_remote.h"
#include "../cis/cis_router.h"
#include "../protocol/mem_protocol.h"
#include "../protocol/protocol_common.h"
#include "../common/umm_network.h"
#include "../common/error_codes.h"
#include "../common/log.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* ------------------------------------------------------------------------ */
/* Constants                                                                 */
/* ------------------------------------------------------------------------ */

#define REMOTE_MAX_CONNS        64
#define REMOTE_MAX_IO_DEFAULT   (1u * 1024 * 1024)
#define REMOTE_MAX_IO_MIN       (4u * 1024)
#define REMOTE_MAX_IO_MAX       (16u * 1024 * 1024)

/* ------------------------------------------------------------------------ */
/* Context                                                                   */
/* ------------------------------------------------------------------------ */

typedef struct {
    int        in_use;
    node_id_t  node;
    char       host[256];
    int        port;
    int        sock;
} RemoteConn;

typedef struct {
    node_id_t       my_node;
    uint32_t        max_io;

    int             token_on;
    uint8_t         token[6];

    /* umms 回退地址（owner == fb_node 时免 cis 查表） */
    int             fb_valid;
    node_id_t       fb_node;
    char            fb_addr[256];

    RemoteConn      conns[REMOTE_MAX_CONNS];
    pthread_mutex_t lock;

    MemoryTransportVtbl *my_vtbl;   /* owned, for destroy */
} RemoteCtx;

/* ------------------------------------------------------------------------ */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------ */

static int parse_addr(const char *addr, char *out_host, size_t host_len,
                      int *out_port)
{
    if (!addr || !out_host || !out_port)
        return UMM_E_INVALID_ARG;
    const char *colon = strrchr(addr, ':');
    if (!colon)
        return UMM_E_INVALID_ARG;
    size_t hlen = (size_t)(colon - addr);
    if (hlen == 0 || hlen >= host_len)
        return UMM_E_INVALID_ARG;
    memcpy(out_host, addr, hlen);
    out_host[hlen] = '\0';
    *out_port = atoi(colon + 1);
    if (*out_port <= 0 || *out_port > 65535)
        return UMM_E_INVALID_ARG;
    return UMM_OK;
}

/* Caller holds lock. 解析 owner 节点地址到 host:port。 */
static int remote_resolve(RemoteCtx *rctx, node_id_t owner,
                          char *out_host, size_t host_len, int *out_port)
{
    /* 1. umms 回退（owner 即分配权威所在节点） */
    if (rctx->fb_valid && owner == rctx->fb_node)
        return parse_addr(rctx->fb_addr, out_host, host_len, out_port);

    /* 2. cis 静态节点表 */
    char addr[256];
    if (cis_router_get_node_addr(owner, addr, sizeof(addr)) == UMM_OK)
        return parse_addr(addr, out_host, host_len, out_port);

    /* 3. 均 miss */
    umm_log_error("transport_remote: no route to owner node %u "
                  "(peer_nodes 未配置且非 umms 回退节点)", (unsigned)owner);
    return UMM_E_NOT_FOUND;
}

/* Caller holds lock. 找到（或新建）owner 对应的连接槽位。 */
static RemoteConn* remote_conn_for(RemoteCtx *rctx, node_id_t owner,
                                   const char *host, int port)
{
    RemoteConn *free_slot = NULL;
    for (int i = 0; i < REMOTE_MAX_CONNS; i++) {
        RemoteConn *c = &rctx->conns[i];
        if (!c->in_use) {
            if (!free_slot)
                free_slot = c;
            continue;
        }
        if (c->node == owner)
            return c;
    }
    if (!free_slot) {
        umm_log_error("transport_remote: conn table full (%d)",
                      REMOTE_MAX_CONNS);
        return NULL;
    }
    free_slot->in_use = 1;
    free_slot->node   = owner;
    free_slot->sock   = -1;
    strncpy(free_slot->host, host, sizeof(free_slot->host) - 1);
    free_slot->host[sizeof(free_slot->host) - 1] = '\0';
    free_slot->port   = port;
    return free_slot;
}

static void remote_disconnect(RemoteConn *c)
{
    if (c->sock >= 0) {
        umm_tcp_close(c->sock);
        c->sock = -1;
    }
}

static int remote_ensure_connected(RemoteConn *c)
{
    if (c->sock >= 0)
        return UMM_OK;
    int s = -1;
    if (umm_tcp_connect(c->host, c->port, &s) != UMM_OK)
        return UMM_E_TRANSPORT_ERROR;
    c->sock = s;
    return UMM_OK;
}

/* ------------------------------------------------------------------------ */
/* Wire helpers（header.reserved 承载 token 摘要）                            */
/* ------------------------------------------------------------------------ */

static void build_header(RemoteCtx *rctx, UmmProtoHeader *hdr, uint8_t op,
                         uint16_t body_len)
{
    memset(hdr, 0, sizeof(*hdr));
    memcpy(hdr->magic, UMM_PROTO_MAGIC, 4);
    hdr->version  = UMM_PROTO_VERSION;
    hdr->opcode   = op;
    hdr->flags    = UMM_FLAG_REQUEST;
    hdr->body_len = body_len;
    if (rctx->token_on)
        memcpy(hdr->reserved, rctx->token, sizeof(hdr->reserved));
}

static int recv_resp_header(RemoteConn *c, UmmProtoHeader *hdr,
                            uint8_t expect_op)
{
    int rc = umm_tcp_recv_exact(c->sock, hdr, sizeof(*hdr));
    if (rc != UMM_OK)
        return rc;
    if (memcmp(hdr->magic, UMM_PROTO_MAGIC, 4) != 0 ||
        hdr->version != UMM_PROTO_VERSION ||
        hdr->opcode  != expect_op ||
        hdr->body_len > UMM_PROTO_MAX_BODY)
        return UMM_E_RPC_ERROR;
    return UMM_OK;
}

/* ------------------------------------------------------------------------ */
/* Single-chunk RPC（len ≤ max_io，调用方已分块）                             */
/* 返回：UMM_OK / 服务端业务 status / UMM_E_TRANSPORT_ERROR（可重试）         */
/* ------------------------------------------------------------------------ */

static int rpc_data_write(RemoteCtx *rctx, RemoteConn *c, gpa_t gpa,
                          const uint8_t *buf, uint64_t len)
{
    UmmProtoBody req;
    mem_pack_data_req(gpa, len, &req);

    UmmProtoHeader hdr;
    build_header(rctx, &hdr, MEM_OP_DATA_WRITE, req.len);

    if (umm_tcp_send(c->sock, &hdr, sizeof(hdr)) != UMM_OK ||
        umm_tcp_send(c->sock, req.data, req.len) != UMM_OK ||
        umm_tcp_send(c->sock, buf, (size_t)len) != UMM_OK)
        return UMM_E_TRANSPORT_ERROR;

    UmmProtoHeader resp_hdr;
    int rc = recv_resp_header(c, &resp_hdr, MEM_OP_DATA_WRITE);
    if (rc != UMM_OK)
        return (rc == UMM_E_RPC_ERROR) ? rc : UMM_E_TRANSPORT_ERROR;

    UmmProtoBody resp;
    memset(&resp, 0, sizeof(resp));
    if (resp_hdr.body_len > 0) {
        if (umm_tcp_recv_exact(c->sock, resp.data, resp_hdr.body_len) != UMM_OK)
            return UMM_E_TRANSPORT_ERROR;
        resp.len = resp_hdr.body_len;
    }
    if (resp.len < 4)
        return UMM_E_RPC_ERROR;

    size_t p = 0;
    return proto_read_i32(resp.data, &p);
}

static int rpc_data_read(RemoteCtx *rctx, RemoteConn *c, gpa_t gpa,
                         uint8_t *out, uint64_t len)
{
    UmmProtoBody req;
    mem_pack_data_req(gpa, len, &req);

    UmmProtoHeader hdr;
    build_header(rctx, &hdr, MEM_OP_DATA_READ, req.len);

    if (umm_tcp_send(c->sock, &hdr, sizeof(hdr)) != UMM_OK ||
        umm_tcp_send(c->sock, req.data, req.len) != UMM_OK)
        return UMM_E_TRANSPORT_ERROR;

    UmmProtoHeader resp_hdr;
    int rc = recv_resp_header(c, &resp_hdr, MEM_OP_DATA_READ);
    if (rc != UMM_OK)
        return (rc == UMM_E_RPC_ERROR) ? rc : UMM_E_TRANSPORT_ERROR;

    UmmProtoBody resp;
    memset(&resp, 0, sizeof(resp));
    if (resp_hdr.body_len > 0) {
        if (umm_tcp_recv_exact(c->sock, resp.data, resp_hdr.body_len) != UMM_OK)
            return UMM_E_TRANSPORT_ERROR;
        resp.len = resp_hdr.body_len;
    }

    int32_t  status = UMM_E_RPC_ERROR;
    uint64_t rlen   = 0;
    rc = mem_unpack_data_read_resp(&resp, &status, &rlen);
    if (rc != UMM_OK)
        return rc;
    if (status != UMM_OK)
        return status;
    if (rlen != len) {
        umm_log_error("transport_remote: DATA_READ len mismatch "
                      "(want %lu, got %lu)",
                      (unsigned long)len, (unsigned long)rlen);
        return UMM_E_RPC_ERROR;
    }

    if (umm_tcp_recv_exact(c->sock, out, (size_t)len) != UMM_OK)
        return UMM_E_TRANSPORT_ERROR;
    return UMM_OK;
}

/* ------------------------------------------------------------------------ */
/* Chunked I/O（重试一次：仅传输错误）                                        */
/* ------------------------------------------------------------------------ */

static int remote_io(RemoteCtx *rctx, gpa_t gpa, uint64_t len,
                     uint8_t *buf, int is_write)
{
    node_id_t owner = gpa_to_node(gpa);
    if (len == 0)
        return UMM_OK;

    pthread_mutex_lock(&rctx->lock);

    char host[256];
    int  port = 0;
    int rc = remote_resolve(rctx, owner, host, sizeof(host), &port);
    if (rc != UMM_OK) {
        pthread_mutex_unlock(&rctx->lock);
        return rc;
    }

    RemoteConn *c = remote_conn_for(rctx, owner, host, port);
    if (!c) {
        pthread_mutex_unlock(&rctx->lock);
        return UMM_E_NO_MEMORY;
    }

    for (int attempt = 0; attempt < 2; attempt++) {
        rc = remote_ensure_connected(c);
        if (rc != UMM_OK) {
            if (attempt == 0)
                continue;   /* 首连失败也重试一次（对端可能正在重启） */
            break;
        }

        uint64_t done = 0;
        while (done < len) {
            uint64_t chunk = len - done;
            if (chunk > rctx->max_io)
                chunk = rctx->max_io;

            gpa_t gpa_i = gpa + done;   /* 只推进 offset 位（offset < 2^56） */
            rc = is_write
               ? rpc_data_write(rctx, c, gpa_i, buf + done, chunk)
               : rpc_data_read (rctx, c, gpa_i, buf + done, chunk);

            if (rc == UMM_E_TRANSPORT_ERROR)
                break;   /* 断开重连后整个 I/O 重来 */
            if (rc != UMM_OK) {
                pthread_mutex_unlock(&rctx->lock);
                return rc;   /* 业务错误：立即透传 */
            }
            done += chunk;
        }

        if (rc == UMM_OK) {
            pthread_mutex_unlock(&rctx->lock);
            return UMM_OK;
        }

        /* 传输错误：断开，下一轮重连重试 */
        remote_disconnect(c);
        umm_log_warn("transport_remote: %s to node %u failed, %s",
                     is_write ? "write" : "read", (unsigned)owner,
                     attempt == 0 ? "reconnecting" : "giving up");
    }

    pthread_mutex_unlock(&rctx->lock);
    return UMM_E_TRANSPORT_ERROR;
}

/* ------------------------------------------------------------------------ */
/* MemoryTransportVtbl implementations                                       */
/* ------------------------------------------------------------------------ */

static int remote_get(void *ctx, gpa_t gpa, uint64_t len, void *out_buf)
{
    if (!ctx || (!out_buf && len > 0))
        return UMM_E_INVALID_ARG;
    return remote_io((RemoteCtx *)ctx, gpa, len, (uint8_t *)out_buf, 0);
}

static int remote_put(void *ctx, gpa_t gpa, uint64_t len, const void *buf)
{
    if (!ctx || (!buf && len > 0))
        return UMM_E_INVALID_ARG;
    return remote_io((RemoteCtx *)ctx, gpa, len, (uint8_t *)buf, 1);
}

static int remote_atomic_unsupported(void *ctx, gpa_t gpa)
{
    (void)ctx;
    umm_log_warn("transport_remote: atomic op on remote GPA 0x%lx is "
                 "UNSUPPORTED in Phase 1 (owner node %u)",
                 (unsigned long)gpa, (unsigned)gpa_to_node(gpa));
    return UMM_E_UNSUPPORTED;
}

static int remote_atomic_cas(void *ctx, gpa_t gpa, uint64_t expected,
                             uint64_t desired, uint64_t *old)
{
    (void)expected; (void)desired; (void)old;
    return remote_atomic_unsupported(ctx, gpa);
}

static int remote_atomic_fetch_add(void *ctx, gpa_t gpa, uint64_t value,
                                   uint64_t *result)
{
    (void)value; (void)result;
    return remote_atomic_unsupported(ctx, gpa);
}

static int remote_atomic_set(void *ctx, gpa_t gpa, uint64_t value)
{
    (void)value;
    return remote_atomic_unsupported(ctx, gpa);
}

static void remote_fence(void *ctx)       { (void)ctx; /* 同步 I/O 无乱序 */ }
static void remote_barrier_all(void *ctx) { (void)ctx; }
static void remote_quiet(void *ctx)       { (void)ctx; }

static int remote_register_node(void *ctx, node_id_t node, uint64_t base,
                                uint64_t size, const char *device)
{
    (void)ctx; (void)node; (void)base; (void)size; (void)device;
    /* 存储注册仍走本地 tier transport / RPC 控制面，远端 transport 无操作 */
    return UMM_OK;
}

static int remote_init(void *ctx, const UMMConfig *cfg)
{
    (void)ctx; (void)cfg;
    return UMM_OK;
}

static void remote_deinit(void *ctx)
{
    RemoteCtx *rctx = ctx;
    if (!rctx)
        return;
    pthread_mutex_lock(&rctx->lock);
    for (int i = 0; i < REMOTE_MAX_CONNS; i++)
        if (rctx->conns[i].in_use)
            remote_disconnect(&rctx->conns[i]);
    pthread_mutex_unlock(&rctx->lock);
}

/* ------------------------------------------------------------------------ */
/* Public API                                                                */
/* ------------------------------------------------------------------------ */

MemoryTransportVtbl* remote_transport_create(node_id_t my_node,
                                             uint32_t data_max_io,
                                             const char *rpc_token,
                                             void **out_ctx)
{
    if (!out_ctx)
        return NULL;

    MemoryTransportVtbl *vtbl = calloc(1, sizeof(MemoryTransportVtbl));
    RemoteCtx *rctx = calloc(1, sizeof(RemoteCtx));
    if (!vtbl || !rctx) {
        free(vtbl);
        free(rctx);
        return NULL;
    }

    rctx->my_node = my_node;
    rctx->fb_valid = 0;
    rctx->fb_node  = UMM_NODE_UNKNOWN;
    pthread_mutex_init(&rctx->lock, NULL);
    rctx->my_vtbl = vtbl;

    if (data_max_io == 0)
        rctx->max_io = REMOTE_MAX_IO_DEFAULT;
    else if (data_max_io < REMOTE_MAX_IO_MIN)
        rctx->max_io = REMOTE_MAX_IO_MIN;
    else if (data_max_io > REMOTE_MAX_IO_MAX)
        rctx->max_io = REMOTE_MAX_IO_MAX;
    else
        rctx->max_io = data_max_io;

    if (rpc_token && rpc_token[0] != '\0') {
        umm_token_digest(rpc_token, rctx->token);
        rctx->token_on = 1;
    }

    vtbl->get              = remote_get;
    vtbl->put              = remote_put;
    vtbl->atomic_cas       = remote_atomic_cas;
    vtbl->atomic_fetch_add = remote_atomic_fetch_add;
    vtbl->atomic_set       = remote_atomic_set;
    vtbl->fence            = remote_fence;
    vtbl->barrier_all      = remote_barrier_all;
    vtbl->quiet            = remote_quiet;
    vtbl->register_node    = remote_register_node;
    vtbl->init             = remote_init;
    vtbl->deinit           = remote_deinit;

    umm_log_info("transport_remote: created (my_node=%u, max_io=%u, token=%s)",
                 (unsigned)my_node, rctx->max_io,
                 rctx->token_on ? "ON" : "off");

    *out_ctx = rctx;
    return vtbl;
}

void remote_transport_destroy(void *ctx)
{
    RemoteCtx *rctx = ctx;
    if (!rctx)
        return;

    remote_deinit(ctx);
    pthread_mutex_destroy(&rctx->lock);

    if (rctx->my_vtbl)
        free(rctx->my_vtbl);
    free(rctx);
}

int remote_transport_set_fallback(void *ctx, node_id_t node,
                                  const char *addr)
{
    RemoteCtx *rctx = ctx;
    if (!rctx || !addr || addr[0] == '\0' || node == UMM_NODE_UNKNOWN)
        return UMM_E_INVALID_ARG;

    char host[256];
    int  port;
    if (parse_addr(addr, host, sizeof(host), &port) != UMM_OK)
        return UMM_E_INVALID_ARG;

    pthread_mutex_lock(&rctx->lock);
    rctx->fb_node = node;
    strncpy(rctx->fb_addr, addr, sizeof(rctx->fb_addr) - 1);
    rctx->fb_addr[sizeof(rctx->fb_addr) - 1] = '\0';
    rctx->fb_valid = 1;
    pthread_mutex_unlock(&rctx->lock);

    umm_log_info("transport_remote: fallback owner node %u -> %s",
                 (unsigned)node, addr);
    return UMM_OK;
}
