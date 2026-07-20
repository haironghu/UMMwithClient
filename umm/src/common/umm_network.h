#ifndef UMM_NETWORK_H
#define UMM_NETWORK_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create a TCP listening socket.
 *
 * @param bind_addr     Address to bind to (e.g. "0.0.0.0" or "::").
 * @param port          Port number to listen on.
 * @param[out] out_sock File descriptor of the listening socket.
 * @return              UMM_OK on success, negative error code otherwise.
 */
int umm_tcp_listen(const char *bind_addr, int port, int *out_sock);

/**
 * Accept a new TCP connection.
 *
 * @param listen_sock   Listening socket file descriptor.
 * @param[out] out_client_sock  File descriptor of the accepted connection.
 * @param[out] client_addr      Buffer to store client address string.
 * @param addr_len      Size of the client_addr buffer.
 * @return              UMM_OK on success, negative error code otherwise.
 */
int umm_tcp_accept(int listen_sock, int *out_client_sock,
                   char *client_addr, size_t addr_len);

/**
 * Connect to a remote TCP endpoint.
 *
 * @param host          Hostname or IP address.
 * @param port          Port number.
 * @param[out] out_sock File descriptor of the connected socket.
 * @return              UMM_OK on success, negative error code otherwise.
 */
int umm_tcp_connect(const char *host, int port, int *out_sock);

/**
 * Send data over a TCP socket (handles partial sends and EINTR).
 *
 * @param sock  Socket file descriptor.
 * @param data  Pointer to data to send.
 * @param len   Number of bytes to send.
 * @return      UMM_OK on success, negative error code otherwise.
 */
int umm_tcp_send(int sock, const void *data, size_t len);

/**
 * Receive data from a TCP socket (handles EINTR).
 *
 * @param sock  Socket file descriptor.
 * @param buf   Buffer to store received data.
 * @param len   Maximum number of bytes to receive.
 * @return      Number of bytes actually received, or negative error code.
 *              Returns 0 if the peer closed the connection.
 */
int umm_tcp_recv(int sock, void *buf, size_t len);

/**
 * Receive exactly `len` bytes from a TCP socket, looping as needed.
 *
 * @param sock  Socket file descriptor.
 * @param buf   Buffer to store received data.
 * @param len   Exact number of bytes to receive.
 * @return      UMM_OK if exactly len bytes were received,
 *              negative error code otherwise (including if peer closed
 *              the connection before len bytes arrived).
 */
int umm_tcp_recv_exact(int sock, void *buf, size_t len);

/**
 * Close a TCP socket.
 */
void umm_tcp_close(int sock);

#ifdef __cplusplus
}
#endif

#endif /* UMM_NETWORK_H */
