/* ========================================================================
 * cis_router.c  --  Cluster Information Service router（Phase 1 实做）
 *
 * Phase 1 形态：静态节点表。集群成员来自配置 UMMConfig.peer_nodes
 * （"node:host:port,node:host:port,..."），进程内可经
 * cis_router_register_node 增补/更新。无成员服务、无心跳——
 * 多节点（>2）与 fencing 属 Phase 3 范围。
 *
 * 用途：transport_remote 把"非本节点 GPA"的 I/O 路由到属主节点时，
 * 经 cis_router_get_node_addr 解析数据面地址。
 * ======================================================================== */

#include "cis_router.h"
#include "../common/log.h"
#include "../common/error_codes.h"

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>

/* ------------------------------------------------------------------------ */
/* Logging helpers                                                          */
/* ------------------------------------------------------------------------ */

static void log_warn(const char *file, int line, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    umm_log(UMM_LOG_WARN, file, line, fmt, ap);
    va_end(ap);
}

static void log_error(const char *file, int line, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    umm_log(UMM_LOG_ERROR, file, line, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------------ */
/* Internal state                                                            */
/* ------------------------------------------------------------------------ */

#define CIS_MAX_NODES 64

typedef struct {
    node_id_t  node;
    char       addr[256];   /* "host:port"（数据面 = mem 协议端口） */
} CisNodeEntry;

typedef struct {
    int             initialized;
    node_id_t       local_node;
    CisNodeEntry    entries[CIS_MAX_NODES];
    int             count;
    pthread_mutex_t lock;
} CisRouterState;

static CisRouterState g_router = {0};

/* ------------------------------------------------------------------------ */
/* Peer list parsing: "node:host:port,node:host:port,..."                    */
/* 注意：host 是 IPv4 或主机名（不含冒号），故第一个 ':' 分隔 node，        */
/* 剩余整体即 "host:port"。                                                  */
/* ------------------------------------------------------------------------ */

static void cis_parse_peer_nodes(const char *peer_nodes)
{
    if (!peer_nodes || peer_nodes[0] == '\0')
        return;

    char *buf = strdup(peer_nodes);
    if (!buf)
        return;

    char *saveptr = NULL;
    for (char *tok = strtok_r(buf, ",", &saveptr);
         tok;
         tok = strtok_r(NULL, ",", &saveptr)) {

        while (*tok == ' ' || *tok == '\t')
            tok++;
        if (*tok == '\0')
            continue;

        char *colon = strchr(tok, ':');
        if (!colon || colon == tok || colon[1] == '\0') {
            log_warn(__FILE__, __LINE__,
                     "cis: ignore malformed peer entry '%s' "
                     "(expect node:host:port)", tok);
            continue;
        }

        long node = strtol(tok, NULL, 10);
        if (node < 0 || node > 255 || node == UMM_NODE_UNKNOWN) {
            log_warn(__FILE__, __LINE__,
                     "cis: ignore peer entry '%s' (node id out of range)", tok);
            continue;
        }

        const char *addr = colon + 1;
        if (strlen(addr) >= sizeof(((CisNodeEntry *)0)->addr)) {
            log_warn(__FILE__, __LINE__,
                     "cis: ignore peer entry '%s' (addr too long)", tok);
            continue;
        }

        cis_router_register_node((node_id_t)node, addr);
    }

    free(buf);
}

/* ------------------------------------------------------------------------ */
/* Public API                                                                */
/* ------------------------------------------------------------------------ */

int cis_router_init(const UMMConfig *cfg)
{
    if (!cfg)
        return UMM_E_INVALID_ARG;

    if (g_router.initialized)
        return UMM_E_ALREADY_EXISTS;

    memset(&g_router, 0, sizeof(g_router));
    pthread_mutex_init(&g_router.lock, NULL);
    g_router.local_node  = cfg->my_node_id;
    g_router.initialized = 1;

    /* Phase 1：静态节点表来自配置 */
    cis_parse_peer_nodes(cfg->peer_nodes);

    if (g_router.count > 0) {
        umm_log_info("cis_router: initialized with %d peer node(s), local=%u",
                     g_router.count, (unsigned)g_router.local_node);
    }
    return UMM_OK;
}

void cis_router_deinit(void)
{
    if (!g_router.initialized)
        return;

    pthread_mutex_destroy(&g_router.lock);
    memset(&g_router, 0, sizeof(g_router));
}

node_id_t cis_route_alloc_node(uint64_t size)
{
    (void)size;

    if (!g_router.initialized) {
        log_error(__FILE__, __LINE__,
                  "cis_route_alloc_node: router not initialized");
        return 0;
    }

    /* Phase 1：分配仍集中在一个 umms（mem_server_addr），不经过本函数。
     * 多 umms 池的容量/负载感知路由属 Phase 3。 */
    return g_router.local_node;
}

int cis_router_register_node(node_id_t node, const char *addr)
{
    if (!g_router.initialized)
        return UMM_E_NOT_INITIALIZED;
    if (!addr || addr[0] == '\0')
        return UMM_E_INVALID_ARG;

    pthread_mutex_lock(&g_router.lock);

    /* 已存在则更新地址 */
    for (int i = 0; i < g_router.count; i++) {
        if (g_router.entries[i].node == node) {
            strncpy(g_router.entries[i].addr, addr,
                    sizeof(g_router.entries[i].addr) - 1);
            g_router.entries[i].addr
                [sizeof(g_router.entries[i].addr) - 1] = '\0';
            pthread_mutex_unlock(&g_router.lock);
            return UMM_OK;
        }
    }

    if (g_router.count >= CIS_MAX_NODES) {
        pthread_mutex_unlock(&g_router.lock);
        return UMM_E_NO_MEMORY;
    }

    CisNodeEntry *e = &g_router.entries[g_router.count++];
    e->node = node;
    strncpy(e->addr, addr, sizeof(e->addr) - 1);
    e->addr[sizeof(e->addr) - 1] = '\0';

    pthread_mutex_unlock(&g_router.lock);
    return UMM_OK;
}

int cis_router_get_node_addr(node_id_t node, char *out_addr, size_t len)
{
    if (!g_router.initialized)
        return UMM_E_NOT_INITIALIZED;

    if (!out_addr || len == 0)
        return UMM_E_INVALID_ARG;

    out_addr[0] = '\0';

    pthread_mutex_lock(&g_router.lock);
    for (int i = 0; i < g_router.count; i++) {
        if (g_router.entries[i].node == node) {
            strncpy(out_addr, g_router.entries[i].addr, len - 1);
            out_addr[len - 1] = '\0';
            pthread_mutex_unlock(&g_router.lock);
            return UMM_OK;
        }
    }
    pthread_mutex_unlock(&g_router.lock);

    return UMM_E_NOT_FOUND;
}
