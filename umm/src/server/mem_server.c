/* ====================================================================
 * MemServer – TCP server framework for umms (memory server)
 *
 * Thread-per-connection model (max 64 concurrent client threads).
 * The server internally creates a MemoryServiceVtbl + context via
 * mem_service_direct_create() and destroys them on cleanup.
 * ==================================================================== */

#include "mem_server.h"

#include "../common/log.h"
#include "../common/error_codes.h"
#include "../common/types.h"
#include "../common/umm_network.h"
#include "../memory_service/mem_service.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <stdarg.h>
#include <pthread.h>

#define MEM_SERVER_MAX_THREADS 64

/* -------------------------------------------------------------------- */
/* Local variadic logging wrappers (umm_log takes va_list, not ...)     */
/* -------------------------------------------------------------------- */

static void mem_srv_log(int level, const char *file, int line,
                        const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    umm_log(level, file, line, fmt, ap);
    va_end(ap);
}

#define MEM_LOG_DEBUG(fmt, ...) \
    mem_srv_log(UMM_LOG_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define MEM_LOG_INFO(fmt, ...) \
    mem_srv_log(UMM_LOG_INFO,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define MEM_LOG_WARN(fmt, ...) \
    mem_srv_log(UMM_LOG_WARN,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define MEM_LOG_ERROR(fmt, ...) \
    mem_srv_log(UMM_LOG_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define MEM_LOG_FATAL(fmt, ...) \
    mem_srv_log(UMM_LOG_FATAL, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

/* -------------------------------------------------------------------- */
/* Forward declarations for RPC handler and service factory             */
/* (defined in separate .c files that are included by the binary)       */
/* -------------------------------------------------------------------- */

extern int mem_service_rpc_handle(int client_sock, void *ctx,
                                   MemoryServiceVtbl *vtbl);
extern void mem_rpc_server_configure(node_id_t node_id,
                                     const char *rpc_token,
                                     uint32_t data_max_io);
extern MemoryServiceVtbl* mem_service_direct_create(node_id_t node_id,
                                                     uint64_t memory_size,
                                                     uint64_t base_gpa,
                                                     void **out_ctx);
/* Phase 2 混合池：内存层 tier 可选（CXL 默认 / DRAM 无 CXL 硬件）；
 * Phase 2.5：mem_device 可选内存层后备设备（共享内存窗口） */
extern MemoryServiceVtbl* mem_service_direct_create_tiered(node_id_t node_id,
                                                     uint64_t memory_size,
                                                     uint64_t base_gpa,
                                                     tier_id_t tier,
                                                     const char *mem_device,
                                                     void **out_ctx);
extern void mem_service_direct_destroy(void *ctx);

/* -------------------------------------------------------------------- */
/* Per-client thread argument                                           */
/* -------------------------------------------------------------------- */

typedef struct {
    MemServer          *server;
    int                 client_sock;
    char                client_addr[64];
} MemClientArg;

/* -------------------------------------------------------------------- */
/* Server state                                                         */
/* -------------------------------------------------------------------- */

struct MemServer {
    char                bind_addr[64];
    int                 port;
    int                 listen_sock;
    node_id_t           node_id;

    /* Phase 1 安全：accept 白名单（空串 = 全放行） */
    char                allow_cidrs[512];

    /* Service implementation */
    MemoryServiceVtbl  *vtbl;
    void               *service_ctx;

    /* Lifecycle */
    volatile int        running;
    pthread_t           accept_thread;

    /* Client thread tracking */
    pthread_t           client_threads[MEM_SERVER_MAX_THREADS];
    int                 client_thread_active[MEM_SERVER_MAX_THREADS];
    pthread_mutex_t     client_list_lock;
    int                 thread_count;
};

/* ==================================================================== */
/* Client handler thread                                                */
/* ==================================================================== */

static void* mem_client_thread(void *arg)
{
    MemClientArg *ca = (MemClientArg *)arg;
    MemServer    *srv = ca->server;
    int           csock = ca->client_sock;

    MEM_LOG_INFO("mem_server: client connected from %s", ca->client_addr);

    free(ca);  /* arg was heap-allocated by accept thread */
    ca = NULL;

    while (srv->running) {
        int rc = mem_service_rpc_handle(csock, srv->service_ctx, srv->vtbl);
        if (rc != UMM_OK) {
            /* Client disconnected or error – exit handler loop */
            if (rc != UMM_E_TRANSPORT_ERROR) {
                MEM_LOG_WARN("mem_server: rpc_handle error %d (%s)",
                             rc, umm_error_string(rc));
            }
            break;
        }
    }

    umm_tcp_close(csock);
    MEM_LOG_INFO("mem_server: client handler exiting");
    return NULL;
}

/* ==================================================================== */
/* Accept loop thread                                                   */
/* ==================================================================== */

static void* mem_accept_thread(void *arg)
{
    MemServer *srv = (MemServer *)arg;

    MEM_LOG_INFO("mem_server: listening on %s:%d", srv->bind_addr, srv->port);

    while (srv->running) {
        int csock;
        char client_addr[64] = {0};

        int rc = umm_tcp_accept(srv->listen_sock, &csock,
                                client_addr, sizeof(client_addr));
        if (rc != UMM_OK) {
            if (!srv->running)
                break;  /* shutting down */
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            MEM_LOG_ERROR("mem_server: accept failed: %s", strerror(errno));
            continue;
        }

        /* Phase 1：CIDR 白名单——未命中即断开（数据面 op 上线后，
         * 端口能力 = 整盘读写，必须在最外层拦截） */
        if (srv->allow_cidrs[0] != '\0' &&
            !umm_net_acl_match(srv->allow_cidrs, client_addr)) {
            MEM_LOG_WARN("mem_server: client %s rejected by ACL (%s)",
                         client_addr, srv->allow_cidrs);
            umm_tcp_close(csock);
            continue;
        }

        /* Find a free slot in the client thread array */
        pthread_mutex_lock(&srv->client_list_lock);
        if (srv->thread_count >= MEM_SERVER_MAX_THREADS) {
            pthread_mutex_unlock(&srv->client_list_lock);
            MEM_LOG_WARN("mem_server: max threads reached, rejecting client %s",
                         client_addr);
            umm_tcp_close(csock);
            continue;
        }

        /* Find first free slot */
        int slot = -1;
        for (int i = 0; i < MEM_SERVER_MAX_THREADS; i++) {
            if (!srv->client_thread_active[i]) {
                slot = i;
                break;
            }
        }

        if (slot < 0) {
            /* Should not happen (thread_count < MAX) but be safe */
            pthread_mutex_unlock(&srv->client_list_lock);
            MEM_LOG_WARN("mem_server: no free slot, rejecting client %s",
                         client_addr);
            umm_tcp_close(csock);
            continue;
        }

        MemClientArg *ca = malloc(sizeof(MemClientArg));
        if (!ca) {
            pthread_mutex_unlock(&srv->client_list_lock);
            MEM_LOG_ERROR("mem_server: out of memory for client arg");
            umm_tcp_close(csock);
            continue;
        }

        ca->server = srv;
        ca->client_sock = csock;
        strncpy(ca->client_addr, client_addr, sizeof(ca->client_addr) - 1);
        ca->client_addr[sizeof(ca->client_addr) - 1] = '\0';

        rc = pthread_create(&srv->client_threads[slot], NULL,
                            mem_client_thread, ca);
        if (rc != 0) {
            pthread_mutex_unlock(&srv->client_list_lock);
            MEM_LOG_ERROR("mem_server: pthread_create failed: %s", strerror(rc));
            free(ca);
            umm_tcp_close(csock);
            continue;
        }

        srv->client_thread_active[slot] = 1;
        srv->thread_count++;
        pthread_mutex_unlock(&srv->client_list_lock);
    }

    MEM_LOG_INFO("mem_server: accept thread exiting");
    return NULL;
}

/* ==================================================================== */
/* Public API                                                           */
/* ==================================================================== */

/* create_impl — 全参数内部实现；mem_tier 选择内存层注册为 CXL（默认）
 * 还是 DRAM（Phase 2 混合池，无 CXL 硬件场景）；
 * mem_device 为内存层后备设备（Phase 2.5 共享内存窗口，NULL=旧行为）。 */
static MemServer* create_impl(const char *bind_addr, int port,
                              node_id_t node_id, uint64_t memory_size,
                              uint64_t base_gpa, tier_id_t mem_tier,
                              const char *mem_device,
                              const char *ssd_backend_dir)
{
    if (!bind_addr || port <= 0 || port > 65535 || memory_size == 0)
        return NULL;

    MemServer *srv = calloc(1, sizeof(MemServer));
    if (!srv)
        return NULL;

    strncpy(srv->bind_addr, bind_addr, sizeof(srv->bind_addr) - 1);
    srv->bind_addr[sizeof(srv->bind_addr) - 1] = '\0';
    srv->port        = port;
    srv->listen_sock = -1;
    srv->running     = 0;
    srv->node_id     = node_id;

    if (pthread_mutex_init(&srv->client_list_lock, NULL) != 0) {
        free(srv);
        return NULL;
    }

    /* Create the memory service implementation（按 mem_tier 注册内存层） */
    srv->vtbl = mem_service_direct_create_tiered(node_id, memory_size,
                                                 base_gpa, mem_tier,
                                                 mem_device,
                                                 &srv->service_ctx);
    if (!srv->vtbl || !srv->service_ctx) {
        MEM_LOG_ERROR("mem_server: failed to create memory service");
        pthread_mutex_destroy(&srv->client_list_lock);
        free(srv);
        return NULL;
    }

    /* Register SSD storage backend if directory specified */
    if (ssd_backend_dir && ssd_backend_dir[0] != '\0' &&
        srv->vtbl->register_storage) {
        StorageResource ssd_res = {
            .tier    = UMM_TIER_SSD,
            .capacity = memory_size,  /* same capacity as CXL tier */
            .base_offset = 0,           /* SSD has its own address space starting at 0 */
            .online  = 1,
        };
        strncpy(ssd_res.device_path, ssd_backend_dir,
                sizeof(ssd_res.device_path) - 1);

        int rc = srv->vtbl->register_storage(srv->service_ctx, &ssd_res);
        if (rc == UMM_OK) {
            MEM_LOG_INFO("mem_server: registered SSD tier (dir=%s, "
                         "capacity=%lu, base_offset=0x%lx)",
                         ssd_backend_dir, (unsigned long)memory_size,
                         (unsigned long)ssd_res.base_offset);
        } else {
            MEM_LOG_WARN("mem_server: failed to register SSD tier (rc=%d), "
                         "SSD allocation will not be available", rc);
        }
    }

    /* Create listening socket */
    int rc = umm_tcp_listen(srv->bind_addr, srv->port, &srv->listen_sock);
    if (rc != UMM_OK) {
        MEM_LOG_ERROR("mem_server: failed to listen on %s:%d (%s)",
                      srv->bind_addr, srv->port, umm_error_string(rc));
        mem_service_direct_destroy(srv->service_ctx);
        pthread_mutex_destroy(&srv->client_list_lock);
        free(srv);
        return NULL;
    }

    MEM_LOG_INFO("mem_server: created (node=%u, size=%lu, base_gpa=0x%lx)",
                 (unsigned)node_id, (unsigned long)memory_size,
                 (unsigned long)base_gpa);
    return srv;
}

MemServer* mem_server_create(const char *bind_addr, int port,
                              node_id_t node_id, uint64_t memory_size,
                              uint64_t base_gpa,
                              const char *ssd_backend_dir)
{
    return create_impl(bind_addr, port, node_id, memory_size, base_gpa,
                       UMM_TIER_CXL, NULL, ssd_backend_dir);
}

/* ======================================================================== */
/* mem_server_create_multi — Create with explicit multi-device list         */
/* ======================================================================== */

/* Internal: multi-device create with selectable memory tier */
static MemServer* create_multi_impl(const char *bind_addr, int port,
                                    node_id_t node_id, uint64_t memory_size,
                                    uint64_t base_gpa, tier_id_t mem_tier,
                                    const char *mem_device,
                                    const char *ssd_backend_dir,
                                    const void *ssd_devices_void,
                                    uint32_t num_ssd_devices)
{
    /* First create with basic SSD backend (registers memory tier) */
    MemServer *srv = create_impl(bind_addr, port, node_id,
                                  memory_size, base_gpa, mem_tier,
                                  mem_device, ssd_backend_dir);
    if (!srv)
        return NULL;

    /* If explicit device list provided, register additional devices */
    if (num_ssd_devices > 0 && ssd_devices_void && srv->vtbl->register_storage) {
        /* Note: ssd_devices_void should be cast to const SsdDeviceConfig* */
        /* We use void* to avoid exposing umm.h in mem_server.h */
        typedef struct { char path[256]; uint64_t size; } SsdDevEntry;
        const SsdDevEntry *devices = (const SsdDevEntry *)ssd_devices_void;

        for (uint32_t i = 0; i < num_ssd_devices; i++) {
            StorageResource res = {
                .tier        = UMM_TIER_SSD,
                .capacity    = devices[i].size,
                .base_offset = 0,  /* pool manages virtual layout */
                .online      = 1,
            };
            strncpy(res.device_path, devices[i].path,
                    sizeof(res.device_path) - 1);

            int rc = srv->vtbl->register_storage(srv->service_ctx, &res);
            if (rc != UMM_OK) {
                MEM_LOG_WARN("mem_server: failed to register SSD device[%u] %s "
                             "(rc=%d)", i, devices[i].path, rc);
                /* Device indices must retain configured order. A partial pool
                 * would silently shift all subsequent device_idx values. */
                mem_server_destroy(srv);
                return NULL;
            } else {
                MEM_LOG_INFO("mem_server: registered SSD device[%u] %s, size=%lu MB",
                             i, devices[i].path,
                             (unsigned long)(devices[i].size / (1024*1024)));
            }
        }
    }

    return srv;
}

MemServer* mem_server_create_multi(const char *bind_addr, int port,
                                    node_id_t node_id, uint64_t memory_size,
                                    uint64_t base_gpa,
                                    const char *ssd_backend_dir,
                                    const void *ssd_devices_void,
                                    uint32_t num_ssd_devices)
{
    return create_multi_impl(bind_addr, port, node_id, memory_size, base_gpa,
                             UMM_TIER_CXL, NULL, ssd_backend_dir,
                             ssd_devices_void, num_ssd_devices);
}

/* Phase 2 混合池：内存层 tier 可选（UMM_TIER_DRAM = 无 CXL 硬件场景，
 * 注册即 malloc 后备；UMM_TIER_CXL = 默认旧行为）。
 * Phase 2.5：mem_device = 内存层后备设备（共享内存窗口，如 /dev/pmem0；
 * NULL = 旧行为）。 */
MemServer* mem_server_create_multi_tiered(const char *bind_addr, int port,
                                           node_id_t node_id,
                                           uint64_t memory_size,
                                           uint64_t base_gpa,
                                           tier_id_t mem_tier,
                                           const char *mem_device,
                                           const void *ssd_devices_void,
                                           uint32_t num_ssd_devices)
{
    return create_multi_impl(bind_addr, port, node_id, memory_size, base_gpa,
                             mem_tier, mem_device, NULL,
                             ssd_devices_void, num_ssd_devices);
}

/* ======================================================================== */

void mem_server_destroy(MemServer *srv)
{
    if (!srv)
        return;

    mem_server_stop(srv);

    if (srv->listen_sock >= 0) {
        umm_tcp_close(srv->listen_sock);
        srv->listen_sock = -1;
    }

    /* Destroy the memory service implementation */
    if (srv->service_ctx) {
        mem_service_direct_destroy(srv->service_ctx);
        srv->service_ctx = NULL;
    }
    srv->vtbl = NULL;

    pthread_mutex_destroy(&srv->client_list_lock);
    free(srv);
}

int mem_server_start(MemServer *srv)
{
    if (!srv)
        return UMM_E_INVALID_ARG;

    if (srv->running)
        return UMM_OK;  /* already running */

    srv->running = 1;

    int rc = pthread_create(&srv->accept_thread, NULL, mem_accept_thread, srv);
    if (rc != 0) {
        MEM_LOG_ERROR("mem_server: failed to create accept thread: %s",
                      strerror(rc));
        srv->running = 0;
        return UMM_E_RPC_ERROR;
    }

    MEM_LOG_INFO("mem_server: started");
    return UMM_OK;
}

void mem_server_stop(MemServer *srv)
{
    if (!srv || !srv->running)
        return;

    MEM_LOG_INFO("mem_server: stopping...");

    /* Signal all threads to stop */
    srv->running = 0;

    /* Closing the listen socket wakes up the accept thread.
     * 注意：Linux 上另一线程阻塞在 accept() 时，单纯 close(listen_fd)
     * 不会唤醒它（accept 在内核中已持有 socket 引用），会导致下面的
     * pthread_join 永久挂起；必须先 shutdown() 使 accept 以 EINVAL
     * 出错返回，线程见 running=0 退出后再 close。 */
    if (srv->listen_sock >= 0) {
        int tmp_sock = srv->listen_sock;
        srv->listen_sock = -1;
        shutdown(tmp_sock, SHUT_RDWR);
        umm_tcp_close(tmp_sock);
    }

    /* Wait for accept thread */
    pthread_join(srv->accept_thread, NULL);

    /* Wait for all client handler threads */
    pthread_mutex_lock(&srv->client_list_lock);
    for (int i = 0; i < MEM_SERVER_MAX_THREADS; i++) {
        if (srv->client_thread_active[i]) {
            pthread_mutex_unlock(&srv->client_list_lock);
            pthread_join(srv->client_threads[i], NULL);
            pthread_mutex_lock(&srv->client_list_lock);
            srv->client_thread_active[i] = 0;
            srv->thread_count--;
        }
    }
    pthread_mutex_unlock(&srv->client_list_lock);

    MEM_LOG_INFO("mem_server: stopped");
}

int mem_server_is_running(MemServer *srv)
{
    if (!srv)
        return 0;
    return srv->running;
}

/* ========================================================================
 * Phase 1 安全接口：token 校验 + CIDR 白名单 + 数据面帧上限
 * ======================================================================== */

int mem_server_set_security(MemServer *srv, const char *rpc_token,
                            const char *allow_cidrs, uint32_t data_max_io)
{
    if (!srv)
        return UMM_E_INVALID_ARG;

    if (allow_cidrs) {
        strncpy(srv->allow_cidrs, allow_cidrs,
                sizeof(srv->allow_cidrs) - 1);
        srv->allow_cidrs[sizeof(srv->allow_cidrs) - 1] = '\0';
    }

    /* token / data_max_io 注入协议处理层（进程级单例） */
    mem_rpc_server_configure(srv->node_id, rpc_token, data_max_io);

    MEM_LOG_INFO("mem_server: security configured (acl=%s, token=%s, "
                 "data_max_io=%u)",
                 srv->allow_cidrs[0] ? srv->allow_cidrs : "(allow-all)",
                 (rpc_token && rpc_token[0]) ? "ON" : "off",
                 data_max_io ? data_max_io : 0);
    return UMM_OK;
}
