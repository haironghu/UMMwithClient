/* ====================================================================
 * MetaServer – TCP server framework for ummd (metadata daemon)
 *
 * Thread-per-connection model (max 64 concurrent client threads).
 * ==================================================================== */

#include "meta_server.h"

#include "../common/log.h"
#include "../common/error_codes.h"
#include "../common/umm_network.h"
#include "../metadata_service/meta_service.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <pthread.h>

#define META_SERVER_MAX_THREADS 64

/* -------------------------------------------------------------------- */
/* Local variadic logging wrappers (umm_log takes va_list, not ...)     */
/* -------------------------------------------------------------------- */

static void meta_srv_log(int level, const char *file, int line,
                         const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    umm_log(level, file, line, fmt, ap);
    va_end(ap);
}

#define META_LOG_DEBUG(fmt, ...) \
    meta_srv_log(UMM_LOG_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define META_LOG_INFO(fmt, ...) \
    meta_srv_log(UMM_LOG_INFO,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define META_LOG_WARN(fmt, ...) \
    meta_srv_log(UMM_LOG_WARN,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define META_LOG_ERROR(fmt, ...) \
    meta_srv_log(UMM_LOG_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define META_LOG_FATAL(fmt, ...) \
    meta_srv_log(UMM_LOG_FATAL, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

/* -------------------------------------------------------------------- */
/* Forward declaration for RPC handler (defined in separate .c file)    */
/* -------------------------------------------------------------------- */

extern int meta_service_rpc_handle(int client_sock, void *ctx,
                                    MetadataServiceVtbl *vtbl);

/* -------------------------------------------------------------------- */
/* Per-client thread argument                                           */
/* -------------------------------------------------------------------- */

typedef struct {
    MetaServer         *server;
    int                 client_sock;
    char                client_addr[64];
} MetaClientArg;

/* -------------------------------------------------------------------- */
/* Server state                                                         */
/* -------------------------------------------------------------------- */

struct MetaServer {
    char                bind_addr[64];
    int                 port;
    int                 listen_sock;

    MetadataServiceVtbl *vtbl;
    void                *service_ctx;

    /* Lifecycle */
    volatile int        running;
    pthread_t           accept_thread;

    /* Client thread tracking */
    pthread_t           client_threads[META_SERVER_MAX_THREADS];
    int                 client_thread_active[META_SERVER_MAX_THREADS];
    pthread_mutex_t     client_list_lock;
    int                 thread_count;
};

/* ==================================================================== */
/* Client handler thread                                                */
/* ==================================================================== */

static void* meta_client_thread(void *arg)
{
    MetaClientArg *ca = (MetaClientArg *)arg;
    MetaServer    *srv = ca->server;
    int            csock = ca->client_sock;

    META_LOG_INFO("meta_server: client connected from %s", ca->client_addr);

    free(ca);  /* arg was heap-allocated by accept thread */
    ca = NULL;

    while (srv->running) {
        int rc = meta_service_rpc_handle(csock, srv->service_ctx, srv->vtbl);
        if (rc != UMM_OK) {
            /* Client disconnected or error – exit handler loop */
            if (rc != UMM_E_TRANSPORT_ERROR) {
                META_LOG_WARN("meta_server: rpc_handle error %d (%s)",
                              rc, umm_error_string(rc));
            }
            break;
        }
    }

    umm_tcp_close(csock);
    META_LOG_INFO("meta_server: client handler exiting");
    return NULL;
}

/* ==================================================================== */
/* Accept loop thread                                                   */
/* ==================================================================== */

static void* meta_accept_thread(void *arg)
{
    MetaServer *srv = (MetaServer *)arg;

    META_LOG_INFO("meta_server: listening on %s:%d", srv->bind_addr, srv->port);

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
            META_LOG_ERROR("meta_server: accept failed: %s", strerror(errno));
            continue;
        }

        /* Find a free slot in the client thread array */
        pthread_mutex_lock(&srv->client_list_lock);
        if (srv->thread_count >= META_SERVER_MAX_THREADS) {
            pthread_mutex_unlock(&srv->client_list_lock);
            META_LOG_WARN("meta_server: max threads reached, rejecting client %s",
                          client_addr);
            umm_tcp_close(csock);
            continue;
        }

        /* Find first free slot */
        int slot = -1;
        for (int i = 0; i < META_SERVER_MAX_THREADS; i++) {
            if (!srv->client_thread_active[i]) {
                slot = i;
                break;
            }
        }

        if (slot < 0) {
            /* Should not happen (thread_count < MAX) but be safe */
            pthread_mutex_unlock(&srv->client_list_lock);
            META_LOG_WARN("meta_server: no free slot, rejecting client %s",
                          client_addr);
            umm_tcp_close(csock);
            continue;
        }

        MetaClientArg *ca = malloc(sizeof(MetaClientArg));
        if (!ca) {
            pthread_mutex_unlock(&srv->client_list_lock);
            META_LOG_ERROR("meta_server: out of memory for client arg");
            umm_tcp_close(csock);
            continue;
        }

        ca->server = srv;
        ca->client_sock = csock;
        strncpy(ca->client_addr, client_addr, sizeof(ca->client_addr) - 1);
        ca->client_addr[sizeof(ca->client_addr) - 1] = '\0';

        rc = pthread_create(&srv->client_threads[slot], NULL,
                            meta_client_thread, ca);
        if (rc != 0) {
            pthread_mutex_unlock(&srv->client_list_lock);
            META_LOG_ERROR("meta_server: pthread_create failed: %s", strerror(rc));
            free(ca);
            umm_tcp_close(csock);
            continue;
        }

        srv->client_thread_active[slot] = 1;
        srv->thread_count++;
        pthread_mutex_unlock(&srv->client_list_lock);
    }

    META_LOG_INFO("meta_server: accept thread exiting");
    return NULL;
}

/* ==================================================================== */
/* Public API                                                           */
/* ==================================================================== */

MetaServer* meta_server_create(const char *bind_addr, int port,
                                MetadataServiceVtbl *vtbl, void *service_ctx)
{
    if (!bind_addr || port <= 0 || port > 65535 || !vtbl || !service_ctx)
        return NULL;

    MetaServer *srv = calloc(1, sizeof(MetaServer));
    if (!srv)
        return NULL;

    strncpy(srv->bind_addr, bind_addr, sizeof(srv->bind_addr) - 1);
    srv->bind_addr[sizeof(srv->bind_addr) - 1] = '\0';
    srv->port        = port;
    srv->vtbl        = vtbl;
    srv->service_ctx = service_ctx;
    srv->listen_sock = -1;
    srv->running     = 0;

    if (pthread_mutex_init(&srv->client_list_lock, NULL) != 0) {
        free(srv);
        return NULL;
    }

    /* Create listening socket */
    int rc = umm_tcp_listen(srv->bind_addr, srv->port, &srv->listen_sock);
    if (rc != UMM_OK) {
        META_LOG_ERROR("meta_server: failed to listen on %s:%d (%s)",
                       srv->bind_addr, srv->port, umm_error_string(rc));
        pthread_mutex_destroy(&srv->client_list_lock);
        free(srv);
        return NULL;
    }

    META_LOG_INFO("meta_server: created, listening socket fd=%d",
                  srv->listen_sock);
    return srv;
}

void meta_server_destroy(MetaServer *srv)
{
    if (!srv)
        return;

    meta_server_stop(srv);

    if (srv->listen_sock >= 0) {
        umm_tcp_close(srv->listen_sock);
        srv->listen_sock = -1;
    }

    pthread_mutex_destroy(&srv->client_list_lock);
    free(srv);
}

int meta_server_start(MetaServer *srv)
{
    if (!srv)
        return UMM_E_INVALID_ARG;

    if (srv->running)
        return UMM_OK;  /* already running */

    srv->running = 1;

    int rc = pthread_create(&srv->accept_thread, NULL, meta_accept_thread, srv);
    if (rc != 0) {
        META_LOG_ERROR("meta_server: failed to create accept thread: %s",
                       strerror(rc));
        srv->running = 0;
        return UMM_E_RPC_ERROR;
    }

    META_LOG_INFO("meta_server: started");
    return UMM_OK;
}

void meta_server_stop(MetaServer *srv)
{
    if (!srv || !srv->running)
        return;

    META_LOG_INFO("meta_server: stopping...");

    /* Signal all threads to stop */
    srv->running = 0;

    /* Closing the listen socket wakes up the accept thread */
    if (srv->listen_sock >= 0) {
        int tmp_sock = srv->listen_sock;
        srv->listen_sock = -1;
        umm_tcp_close(tmp_sock);
    }

    /* Wait for accept thread */
    pthread_join(srv->accept_thread, NULL);

    /* Wait for all client handler threads */
    pthread_mutex_lock(&srv->client_list_lock);
    for (int i = 0; i < META_SERVER_MAX_THREADS; i++) {
        if (srv->client_thread_active[i]) {
            pthread_mutex_unlock(&srv->client_list_lock);
            pthread_join(srv->client_threads[i], NULL);
            pthread_mutex_lock(&srv->client_list_lock);
            srv->client_thread_active[i] = 0;
            srv->thread_count--;
        }
    }
    pthread_mutex_unlock(&srv->client_list_lock);

    META_LOG_INFO("meta_server: stopped");
}

int meta_server_is_running(MetaServer *srv)
{
    if (!srv)
        return 0;
    return srv->running;
}
