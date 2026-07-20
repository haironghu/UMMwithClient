/* ========================================================================
 * storage-tierd -- UMM SSD Tier Daemon
 *
 * Manages the SSD tier for a node. Handles chunk creation, persistence,
 * deletion, sync, and stats requests via TCP RPC.
 *
 * Usage: ./storage-tierd [-p port] [-b bind_addr] [-d base_dir]
 *                        [-s max_size_gb] [-n node_id] [-l log_level]
 *   -p port      Listening port      (default: 20004)
 *   -b addr      Bind address        (default: 0.0.0.0)
 *   -d dir       SSD base directory  (default: /tmp/umm_ssd)
 *   -s size_gb   Max size in GB      (default: 10)
 *   -n node_id   Node ID             (default: 0)
 *   -l level     Log level 0-4       (default: 1 = INFO)
 * ======================================================================== */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <pthread.h>
#include <errno.h>
#include <stdarg.h>

#include "../include/umm.h"
#include "../src/common/log.h"
#include "../src/common/error_codes.h"
#include "../src/common/types.h"
#include "../src/common/umm_network.h"
#include "../src/protocol/protocol_common.h"
#include "../src/tier/ssd_tier.h"

/* ------------------------------------------------------------------------ */
/* Missing error-code constants (not yet in include/umm.h)                 */
/* ------------------------------------------------------------------------ */

#ifndef UMM_E_RPC_ERROR
#define UMM_E_RPC_ERROR     (-6)
#endif
#ifndef UMM_E_TRANSPORT_ERROR
#define UMM_E_TRANSPORT_ERROR (-8)
#endif
#ifndef UMM_E_UNKNOWN
#define UMM_E_UNKNOWN       (-99)
#endif

/* ------------------------------------------------------------------------ */
/* SSD service opcodes                                                     */
/* ------------------------------------------------------------------------ */

enum {
    SSD_OP_CREATE_CHUNK = 1,
    SSD_OP_DELETE_CHUNK = 2,
    SSD_OP_SYNC_CHUNK   = 3,
    SSD_OP_GET_STATS    = 4,
    SSD_OP_HEARTBEAT    = 5,
};

/* ------------------------------------------------------------------------ */
/* Local variadic logging wrappers                                         */
/* ------------------------------------------------------------------------ */

static void daemon_log(int level, const char *file, int line,
                       const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    umm_log(level, file, line, fmt, ap);
    va_end(ap);
}

#define DLOG_DEBUG(fmt, ...) \
    daemon_log(UMM_LOG_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define DLOG_INFO(fmt, ...) \
    daemon_log(UMM_LOG_INFO,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define DLOG_WARN(fmt, ...) \
    daemon_log(UMM_LOG_WARN,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define DLOG_ERROR(fmt, ...) \
    daemon_log(UMM_LOG_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define DLOG_FATAL(fmt, ...) \
    daemon_log(UMM_LOG_FATAL, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

/* ------------------------------------------------------------------------ */
/* Globals                                                                  */
/* ------------------------------------------------------------------------ */

static volatile sig_atomic_t g_running = 1;

/* ------------------------------------------------------------------------ */
/* Signal handler                                                           */
/* ------------------------------------------------------------------------ */

static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ------------------------------------------------------------------------ */
/* Print usage                                                              */
/* ------------------------------------------------------------------------ */

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [-p port] [-b bind_addr] [-d base_dir] "
            "[-s max_size_gb] [-n node_id] [-l log_level]\n"
            "  -p port      Listening port      (default: 20004)\n"
            "  -b addr      Bind address        (default: 0.0.0.0)\n"
            "  -d dir       SSD base directory  (default: /tmp/umm_ssd)\n"
            "  -s size_gb   Max size in GB      (default: 10)\n"
            "  -n node_id   Node ID             (default: 0)\n"
            "  -l level     Log level 0-4       (default: 1 = INFO)\n"
            "                 0=DEBUG 1=INFO 2=WARN 3=ERROR 4=FATAL\n",
            prog);
}

/* ------------------------------------------------------------------------ */
/* Helper: build and send a complete response (header + body)              */
/* ------------------------------------------------------------------------ */

static int send_response(int client_sock, uint8_t opcode, uint8_t flags,
                         const uint8_t *body_data, uint16_t body_len)
{
    UmmProtoHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, UMM_PROTO_MAGIC, 4);
    hdr.version  = UMM_PROTO_VERSION;
    hdr.opcode   = opcode;
    hdr.flags    = flags;
    hdr.body_len = body_len;

    int rc = umm_tcp_send(client_sock, &hdr, sizeof(hdr));
    if (rc != UMM_OK)
        return rc;

    if (body_data && body_len > 0) {
        rc = umm_tcp_send(client_sock, body_data, body_len);
        if (rc != UMM_OK)
            return rc;
    }

    return UMM_OK;
}

/* ------------------------------------------------------------------------ */
/* Helper: read a complete request (header + body) from the client         */
/* ------------------------------------------------------------------------ */

static int recv_request(int client_sock, UmmProtoHeader *hdr,
                        UmmProtoBody *body)
{
    /* --- read header --- */
    int rc = umm_tcp_recv_exact(client_sock, hdr, sizeof(*hdr));
    if (rc != UMM_OK)
        return rc;

    /* --- validate header --- */
    if (memcmp(hdr->magic, UMM_PROTO_MAGIC, 4) != 0)
        return UMM_E_RPC_ERROR;
    if (hdr->version != UMM_PROTO_VERSION)
        return UMM_E_RPC_ERROR;
    if (hdr->body_len > UMM_PROTO_MAX_BODY)
        return UMM_E_RPC_ERROR;

    /* --- read body if present --- */
    memset(body, 0, sizeof(*body));
    if (hdr->body_len > 0) {
        rc = umm_tcp_recv_exact(client_sock, body->data, hdr->body_len);
        if (rc != UMM_OK)
            return rc;
    }
    body->len = hdr->body_len;

    return UMM_OK;
}

/* ------------------------------------------------------------------------ */
/* SSD RPC request handler                                                  */
/* ------------------------------------------------------------------------ */

static int ssd_rpc_handle(int client_sock, SsdTier *st)
{
    UmmProtoHeader hdr;
    UmmProtoBody   body;
    uint8_t        resp_data[UMM_PROTO_MAX_BODY];
    uint16_t       resp_len = 0;

    int rc = recv_request(client_sock, &hdr, &body);
    if (rc != UMM_OK)
        return rc;

    uint8_t opcode = hdr.opcode;
    int32_t status = UMM_OK;
    size_t  p = 0;

    memset(resp_data, 0, sizeof(resp_data));

    switch (opcode) {

    case SSD_OP_CREATE_CHUNK: {
        /* Request: node_id(u32) + offset(u64) + size(u64) = 20 bytes */
        if (body.len < 20) {
            status = UMM_E_INVALID_ARG;
            proto_write_i32(resp_data, &p, status);
            resp_len = (uint16_t)p;
            break;
        }
        p = 0;
        node_id_t node  = (node_id_t)proto_read_u32(body.data, &p);
        uint64_t offset = proto_read_u64(body.data, &p);
        uint64_t size   = proto_read_u64(body.data, &p);

        DLOG_DEBUG("SSD_OP_CREATE_CHUNK: node=%u offset=%lu size=%lu",
                   (unsigned)node, (unsigned long)offset,
                   (unsigned long)size);

        status = ssd_tier_create_chunk(st, node, offset, size);

        p = 0;
        proto_write_i32(resp_data, &p, status);
        resp_len = (uint16_t)p;
        break;
    }

    case SSD_OP_DELETE_CHUNK: {
        /* Request: node_id(u32) + offset(u64) = 12 bytes */
        if (body.len < 12) {
            status = UMM_E_INVALID_ARG;
            proto_write_i32(resp_data, &p, status);
            resp_len = (uint16_t)p;
            break;
        }
        p = 0;
        node_id_t node  = (node_id_t)proto_read_u32(body.data, &p);
        uint64_t offset = proto_read_u64(body.data, &p);

        DLOG_DEBUG("SSD_OP_DELETE_CHUNK: node=%u offset=%lu",
                   (unsigned)node, (unsigned long)offset);

        status = ssd_tier_delete_chunk(st, node, offset);

        p = 0;
        proto_write_i32(resp_data, &p, status);
        resp_len = (uint16_t)p;
        break;
    }

    case SSD_OP_SYNC_CHUNK: {
        /* Request: node_id(u32) + offset(u64) + size(u64) = 20 bytes */
        if (body.len < 20) {
            status = UMM_E_INVALID_ARG;
            proto_write_i32(resp_data, &p, status);
            resp_len = (uint16_t)p;
            break;
        }
        p = 0;
        node_id_t node  = (node_id_t)proto_read_u32(body.data, &p);
        uint64_t offset = proto_read_u64(body.data, &p);
        uint64_t size   = proto_read_u64(body.data, &p);

        DLOG_DEBUG("SSD_OP_SYNC_CHUNK: node=%u offset=%lu size=%lu",
                   (unsigned)node, (unsigned long)offset,
                   (unsigned long)size);

        status = ssd_tier_sync(st, node, offset, size);

        p = 0;
        proto_write_i32(resp_data, &p, status);
        resp_len = (uint16_t)p;
        break;
    }

    case SSD_OP_GET_STATS: {
        /* No request body needed */
        DLOG_DEBUG("SSD_OP_GET_STATS");

        /* For stub tier, return zeros; real tier would expose stats */
        uint64_t total = 0;
        uint64_t used  = 0;
        uint64_t free  = 0;

        p = 0;
        proto_write_i32(resp_data, &p, status);
        proto_write_u64(resp_data, &p, total);
        proto_write_u64(resp_data, &p, used);
        proto_write_u64(resp_data, &p, free);
        resp_len = (uint16_t)p;
        break;
    }

    case SSD_OP_HEARTBEAT: {
        DLOG_DEBUG("SSD_OP_HEARTBEAT");
        status = UMM_OK;
        /* Empty response body */
        resp_len = 0;
        break;
    }

    default:
        DLOG_WARN("Unknown opcode: %u", (unsigned)opcode);
        status = UMM_E_UNKNOWN;
        resp_len = 0;
        break;
    }

    /* Send response with RESPONSE flag */
    rc = send_response(client_sock, opcode, UMM_FLAG_RESPONSE,
                       resp_data, resp_len);
    if (rc != UMM_OK)
        return rc;

    return UMM_OK;
}

/* ------------------------------------------------------------------------ */
/* Client handler thread (one per connection)                              */
/* ------------------------------------------------------------------------ */

typedef struct {
    int       client_sock;
    char      client_addr[64];
    SsdTier  *ssd_tier;
} ClientThreadArg;

static void* client_handler_thread(void *arg)
{
    ClientThreadArg *cta = (ClientThreadArg *)arg;
    int csock = cta->client_sock;
    SsdTier *st = cta->ssd_tier;

    DLOG_INFO("Client connected from %s", cta->client_addr);
    free(cta);
    cta = NULL;

    while (g_running) {
        int rc = ssd_rpc_handle(csock, st);
        if (rc != UMM_OK) {
            if (rc != UMM_E_TRANSPORT_ERROR)
                DLOG_WARN("RPC handler error: %d (%s)", rc, umm_error_string(rc));
            break;
        }
    }

    umm_tcp_close(csock);
    DLOG_INFO("Client handler exiting");
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* main                                                                     */
/* ------------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    int         port        = 20004;
    const char *bind_addr   = "0.0.0.0";
    const char *base_dir    = "/tmp/umm_ssd";
    uint64_t    max_size_gb = 10;
    node_id_t   node_id     = 0;
    int         log_level   = UMM_LOG_INFO;

    int opt;
    while ((opt = getopt(argc, argv, "p:b:d:s:n:l:h")) != -1) {
        switch (opt) {
        case 'p':
            port = atoi(optarg);
            if (port <= 0 || port > 65535) {
                fprintf(stderr, "Invalid port: %s\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'b':
            bind_addr = optarg;
            break;
        case 'd':
            base_dir = optarg;
            break;
        case 's':
            max_size_gb = strtoull(optarg, NULL, 0);
            if (max_size_gb == 0) {
                fprintf(stderr, "Invalid max size: %s\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'n':
            node_id = (node_id_t)atoi(optarg);
            break;
        case 'l':
            log_level = atoi(optarg);
            if (log_level < UMM_LOG_DEBUG || log_level > UMM_LOG_FATAL) {
                fprintf(stderr, "Invalid log level: %s\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'h':
        default:
            print_usage(argv[0]);
            return (opt == 'h') ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

    /* ---- Logging ---- */
    umm_log_set_level(log_level);

    uint64_t max_bytes = max_size_gb * 1024ULL * 1024ULL * 1024ULL;

    DLOG_INFO("storage-tierd starting: bind=%s port=%d node=%u "
              "base_dir=%s max_size=%luGB (%lu bytes)",
              bind_addr, port, (unsigned)node_id,
              base_dir, (unsigned long)max_size_gb, (unsigned long)max_bytes);

    /* ---- Signal handlers ---- */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    if (sigaction(SIGINT, &sa, NULL) < 0) {
        DLOG_FATAL("sigaction(SIGINT) failed: %s", strerror(errno));
        return EXIT_FAILURE;
    }
    if (sigaction(SIGTERM, &sa, NULL) < 0) {
        DLOG_FATAL("sigaction(SIGTERM) failed: %s", strerror(errno));
        return EXIT_FAILURE;
    }

    /* Ignore SIGPIPE */
    signal(SIGPIPE, SIG_IGN);

    /* ---- Create SSD tier ---- */
    SsdTier *st = ssd_tier_create(base_dir, max_bytes);
    if (!st) {
        DLOG_WARN("ssd_tier_create returned NULL (stub tier), continuing...");
    } else {
        DLOG_INFO("SSD tier created at %s", base_dir);
    }

    /* ---- Create listening socket ---- */
    int listen_sock = -1;
    int rc = umm_tcp_listen(bind_addr, port, &listen_sock);
    if (rc != UMM_OK) {
        DLOG_FATAL("Failed to listen on %s:%d (%s)",
                   bind_addr, port, umm_error_string(rc));
        ssd_tier_destroy(st);
        return EXIT_FAILURE;
    }

    DLOG_INFO("storage-tierd listening on %s:%d", bind_addr, port);

    /* ---- Main accept loop ---- */
    while (g_running) {
        int csock;
        char client_addr[64] = {0};

        rc = umm_tcp_accept(listen_sock, &csock, client_addr, sizeof(client_addr));
        if (rc != UMM_OK) {
            if (!g_running)
                break;
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            DLOG_ERROR("Accept failed: %s", strerror(errno));
            continue;
        }

        /* Spawn a detached thread to handle this client */
        ClientThreadArg *cta = malloc(sizeof(ClientThreadArg));
        if (!cta) {
            DLOG_ERROR("Out of memory for client thread arg");
            umm_tcp_close(csock);
            continue;
        }

        cta->client_sock = csock;
        strncpy(cta->client_addr, client_addr, sizeof(cta->client_addr) - 1);
        cta->client_addr[sizeof(cta->client_addr) - 1] = '\0';
        cta->ssd_tier = st;

        pthread_t tid;
        rc = pthread_create(&tid, NULL, client_handler_thread, cta);
        if (rc != 0) {
            DLOG_ERROR("pthread_create failed: %s", strerror(rc));
            free(cta);
            umm_tcp_close(csock);
            continue;
        }

        /* Detach the thread so it cleans up on exit */
        pthread_detach(tid);
    }

    DLOG_INFO("storage-tierd shutting down...");

    /* ---- Cleanup ---- */
    if (listen_sock >= 0) {
        umm_tcp_close(listen_sock);
    }

    ssd_tier_destroy(st);

    DLOG_INFO("storage-tierd exited cleanly");
    return EXIT_SUCCESS;
}
