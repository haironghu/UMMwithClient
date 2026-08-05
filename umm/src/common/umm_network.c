#include "umm_network.h"
#include "error_codes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

/* ========================================================================
 * Platform helpers
 * ======================================================================== */

#ifdef __APPLE__
    /* macOS: use SO_NOSIGPIPE socket option */
    #define UMM_USE_SO_NOSIGPIPE 1
#else
    /* Linux: use MSG_NOSIGNAL flag */
    #define UMM_USE_SO_NOSIGPIPE 0
#endif

/**
 * Apply common socket options: TCP_NODELAY, SO_NOSIGPIPE (macOS).
 */
static int apply_socket_opts(int sock)
{
    int yes = 1;

    /* Disable Nagle's algorithm for low latency */
    if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) < 0)
        return UMM_E_TRANSPORT_ERROR;

#if UMM_USE_SO_NOSIGPIPE
    /* Prevent SIGPIPE on macOS */
    if (setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)) < 0)
        return UMM_E_TRANSPORT_ERROR;
#endif

    return UMM_OK;
}

/**
 * Wrapper around send() that handles EINTR.
 */
static ssize_t safe_send(int sock, const void *buf, size_t len)
{
    int flags = 0;
#if !UMM_USE_SO_NOSIGPIPE
    flags |= MSG_NOSIGNAL;
#endif

    ssize_t rc;
    do {
        rc = send(sock, buf, len, flags);
    } while (rc < 0 && errno == EINTR);

    return rc;
}

/**
 * Wrapper around recv() that handles EINTR.
 */
static ssize_t safe_recv(int sock, void *buf, size_t len)
{
    ssize_t rc;
    do {
        rc = recv(sock, buf, len, 0);
    } while (rc < 0 && errno == EINTR);

    return rc;
}

/* ========================================================================
 * Public API
 * ======================================================================== */

int umm_tcp_listen(const char *bind_addr, int port, int *out_sock)
{
    if (!bind_addr || !out_sock || port <= 0 || port > 65535)
        return UMM_E_INVALID_ARG;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return UMM_E_TRANSPORT_ERROR;

    /* Allow address reuse */
    int yes = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        close(sock);
        return UMM_E_TRANSPORT_ERROR;
    }

    if (apply_socket_opts(sock) != UMM_OK) {
        close(sock);
        return UMM_E_TRANSPORT_ERROR;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);

    if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
        close(sock);
        return UMM_E_INVALID_ARG;
    }

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return UMM_E_TRANSPORT_ERROR;
    }

    if (listen(sock, SOMAXCONN) < 0) {
        close(sock);
        return UMM_E_TRANSPORT_ERROR;
    }

    *out_sock = sock;
    return UMM_OK;
}

int umm_tcp_accept(int listen_sock, int *out_client_sock,
                   char *client_addr, size_t addr_len)
{
    if (listen_sock < 0 || !out_client_sock)
        return UMM_E_INVALID_ARG;

    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);

    int csock;
    do {
        csock = accept(listen_sock, (struct sockaddr *)&addr, &addrlen);
    } while (csock < 0 && errno == EINTR);

    if (csock < 0)
        return UMM_E_TRANSPORT_ERROR;

    if (apply_socket_opts(csock) != UMM_OK) {
        close(csock);
        return UMM_E_TRANSPORT_ERROR;
    }

    if (client_addr && addr_len > 0) {
        char *ip = inet_ntoa(addr.sin_addr);
        strncpy(client_addr, ip, addr_len - 1);
        client_addr[addr_len - 1] = '\0';
    }

    *out_client_sock = csock;
    return UMM_OK;
}

int umm_tcp_connect(const char *host, int port, int *out_sock)
{
    if (!host || !out_sock || port <= 0 || port > 65535)
        return UMM_E_INVALID_ARG;

    /* Attempt numeric IP first */
    struct in_addr numeric_addr;
    int is_numeric = (inet_pton(AF_INET, host, &numeric_addr) == 1);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);

    if (is_numeric) {
        addr.sin_addr = numeric_addr;
    } else {
        /* DNS resolution */
        struct hostent *he = gethostbyname(host);
        if (!he || !he->h_addr_list[0])
            return UMM_E_TRANSPORT_ERROR;

        memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(struct in_addr));
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return UMM_E_TRANSPORT_ERROR;

    if (apply_socket_opts(sock) != UMM_OK) {
        close(sock);
        return UMM_E_TRANSPORT_ERROR;
    }

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return UMM_E_TRANSPORT_ERROR;
    }

    *out_sock = sock;
    return UMM_OK;
}

int umm_tcp_send(int sock, const void *data, size_t len)
{
    if (sock < 0 || (!data && len > 0))
        return UMM_E_INVALID_ARG;

    const uint8_t *ptr = (const uint8_t *)data;
    size_t sent = 0;

    while (sent < len) {
        ssize_t rc = safe_send(sock, ptr + sent, len - sent);
        if (rc < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return UMM_E_TRANSPORT_ERROR;
        }
        if (rc == 0)
            return UMM_E_TRANSPORT_ERROR; /* connection closed unexpectedly */

        sent += (size_t)rc;
    }

    return UMM_OK;
}

int umm_tcp_recv(int sock, void *buf, size_t len)
{
    if (sock < 0 || (!buf && len > 0))
        return UMM_E_INVALID_ARG;

    ssize_t rc = safe_recv(sock, buf, len);
    if (rc < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0; /* no data available */
        return UMM_E_TRANSPORT_ERROR;
    }

    return (int)rc; /* 0 means peer closed connection */
}

int umm_tcp_recv_exact(int sock, void *buf, size_t len)
{
    if (sock < 0 || (!buf && len > 0))
        return UMM_E_INVALID_ARG;

    uint8_t *ptr = (uint8_t *)buf;
    size_t received = 0;

    while (received < len) {
        ssize_t rc = safe_recv(sock, ptr + received, len - received);
        if (rc < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return UMM_E_TRANSPORT_ERROR;
        }
        if (rc == 0)
            return UMM_E_TRANSPORT_ERROR; /* peer closed before we got all data */

        received += (size_t)rc;
    }

    return UMM_OK;
}

void umm_tcp_close(int sock)
{
    if (sock >= 0)
        close(sock);
}

/* ========================================================================
 * Phase 1: CIDR 白名单（accept 级拦截，IPv4）
 * ======================================================================== */

/* 解析单个 "a.b.c.d[/n]" 条目；返回 UMM_OK 时输出网络序地址与前缀长 */
static int parse_cidr_entry(const char *entry, size_t len,
                            uint32_t *out_net, int *out_prefix)
{
    char buf[64];
    if (len == 0 || len >= sizeof(buf))
        return UMM_E_INVALID_ARG;
    memcpy(buf, entry, len);
    buf[len] = '\0';

    char *slash = strchr(buf, '/');
    int prefix = 32;
    if (slash) {
        *slash = '\0';
        prefix = atoi(slash + 1);
        if (prefix < 0 || prefix > 32)
            return UMM_E_INVALID_ARG;
    }

    struct in_addr a;
    if (inet_pton(AF_INET, buf, &a) != 1)
        return UMM_E_INVALID_ARG;

    *out_net    = ntohl(a.s_addr);   /* 转主机序便于位移比较 */
    *out_prefix = prefix;
    return UMM_OK;
}

int umm_net_acl_match(const char *cidr_list, const char *ip)
{
    if (!cidr_list || cidr_list[0] == '\0')
        return 1;   /* 无白名单 = 全放行（旧行为） */
    if (!ip)
        return 0;

    struct in_addr ia;
    if (inet_pton(AF_INET, ip, &ia) != 1)
        return 0;
    uint32_t addr = ntohl(ia.s_addr);

    const char *p = cidr_list;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);

        /* 跳过前导空白 */
        while (len > 0 && (*p == ' ' || *p == '\t')) { p++; len--; }

        uint32_t net; int prefix;
        if (parse_cidr_entry(p, len, &net, &prefix) == UMM_OK) {
            uint32_t mask = (prefix == 0) ? 0u
                          : (0xFFFFFFFFu << (32 - prefix));
            if ((addr & mask) == (net & mask))
                return 1;
        }
        /* 非法条目：不命中，继续看后续条目 */

        if (!comma)
            break;
        p = comma + 1;
    }
    return 0;
}
