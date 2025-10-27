/**
 * @file transport.h
 * @brief Network transport abstraction layer
 *
 * Provides a unified interface for different transport types:
 * - Unix domain sockets (supports FD passing)
 * - TCP sockets
 * - UDP sockets
 */

#ifndef TRANSPORT_H
#define TRANSPORT_H

#include <sys/types.h>
#include <netinet/in.h>

/* Transport types */
typedef enum {
    TRANSPORT_UNIX,     /* Unix domain socket - supports FD passing */
    TRANSPORT_TCP,      /* TCP socket - stream-based */
    TRANSPORT_UDP       /* UDP socket - datagram-based */
} transport_type_t;

/* Transport handle */
typedef struct transport transport_t;

/* Transport configuration */
typedef struct {
    transport_type_t type;
    union {
        struct {
            const char *path;           /* Unix socket path */
        } unix_cfg;
        struct {
            const char *host;           /* Hostname or IP */
            uint16_t port;              /* Port number */
        } tcp_cfg;
        struct {
            const char *host;           /* Hostname or IP */
            uint16_t port;              /* Port number */
            size_t max_packet_size;     /* Max UDP packet size */
        } udp_cfg;
    };
} transport_config_t;

/* Transport capabilities */
typedef struct {
    int supports_fdpass;        /* Can pass file descriptors */
    int is_stream;              /* Stream-based (vs datagram) */
    int is_connection_oriented; /* Connection-oriented */
} transport_caps_t;

/**
 * Create a transport server
 * 
 * @param config Transport configuration
 * @param max_connections Max pending connections (for stream transports)
 * @return Transport handle on success, NULL on failure
 */
transport_t *transport_server_create(const transport_config_t *config, 
                                     int max_connections);

/**
 * Accept a client connection (stream transports only)
 * 
 * @param server Server transport handle
 * @return Client transport handle on success, NULL on failure
 */
transport_t *transport_accept(transport_t *server);

/**
 * Create a transport client connection
 * 
 * @param config Transport configuration
 * @return Transport handle on success, NULL on failure
 */
transport_t *transport_client_connect(const transport_config_t *config);

/**
 * Send data over transport
 * 
 * @param transport Transport handle
 * @param data Data to send
 * @param len Data length
 * @return Bytes sent on success, -1 on failure
 */
ssize_t transport_send(transport_t *transport, const void *data, size_t len);

/**
 * Receive data from transport
 * 
 * @param transport Transport handle
 * @param buffer Buffer to receive into
 * @param len Buffer size
 * @return Bytes received on success, -1 on failure
 */
ssize_t transport_recv(transport_t *transport, void *buffer, size_t len);

/**
 * Send file descriptor (Unix sockets only)
 * 
 * @param transport Transport handle
 * @param fd File descriptor to send
 * @param metadata Optional metadata byte
 * @return 0 on success, -1 on failure
 */
int transport_send_fd(transport_t *transport, int fd, char metadata);

/**
 * Receive file descriptor (Unix sockets only)
 * 
 * @param transport Transport handle
 * @param metadata Pointer to receive metadata byte (can be NULL)
 * @return File descriptor on success, -1 on failure
 */
int transport_recv_fd(transport_t *transport, char *metadata);

/**
 * Get transport file descriptor (for polling/select)
 * 
 * @param transport Transport handle
 * @return File descriptor
 */
int transport_get_fd(transport_t *transport);

/**
 * Get transport capabilities
 * 
 * @param transport Transport handle
 * @param caps Pointer to receive capabilities
 * @return 0 on success, -1 on failure
 */
int transport_get_caps(transport_t *transport, transport_caps_t *caps);

/**
 * Get transport type
 * 
 * @param transport Transport handle
 * @return Transport type
 */
transport_type_t transport_get_type(transport_t *transport);

/**
 * Close and free transport
 * 
 * @param transport Transport handle
 */
void transport_close(transport_t *transport);

#endif /* TRANSPORT_H */
