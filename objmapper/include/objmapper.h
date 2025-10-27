/**
 * @file objmapper.h
 * @brief Main objmapper server and client API
 */

#ifndef OBJMAPPER_H
#define OBJMAPPER_H

#include <sys/types.h>
#include <stdint.h>

#define OBJMAPPER_SOCK_PATH "/tmp/objmapper.sock"
#define OBJMAPPER_TCP_PORT 9999
#define OBJMAPPER_UDP_PORT 9998

/* Transport types */
typedef enum {
    OBJMAPPER_TRANSPORT_UNIX = 0,  /* Unix socket (primary, supports FD passing) */
    OBJMAPPER_TRANSPORT_TCP = 1,   /* TCP socket */
    OBJMAPPER_TRANSPORT_UDP = 2    /* UDP socket */
} objmapper_transport_t;

/* Operation types */
#define OP_FDPASS    '1'  /* File descriptor passing (Unix only) */
#define OP_COPY      '2'  /* Data copy */
#define OP_SPLICE    '3'  /* Splice transfer (Unix/TCP only) */

/* Client configuration */
typedef struct {
    objmapper_transport_t transport;
    union {
        const char *socket_path;    /* For Unix sockets */
        struct {
            const char *host;       /* For TCP/UDP */
            uint16_t port;
        } net;
    };
    char operation_mode;            /* OP_FDPASS, OP_COPY, or OP_SPLICE */
} client_config_t;

/* Server configuration */
typedef struct {
    objmapper_transport_t transport;
    union {
        const char *socket_path;    /* For Unix sockets */
        struct {
            const char *host;       /* For TCP/UDP (NULL or "*" for any) */
            uint16_t port;
        } net;
    };
    const char *backing_dir;
    const char *cache_dir;
    size_t cache_limit;
    int max_connections;
} server_config_t;

/**
 * Start objmapper server
 * 
 * @param config Server configuration
 * @return 0 on success, -1 on failure
 */
int objmapper_server_start(const server_config_t *config);

/**
 * Connect to objmapper server
 * 
 * @param config Client configuration
 * @return Socket file descriptor on success, -1 on failure
 */
int objmapper_client_connect(const client_config_t *config);

/**
 * Request object from server
 * 
 * @param sock Client socket
 * @param uri Object URI
 * @param mode Operation mode (OP_FDPASS, OP_COPY, etc.)
 * @return File descriptor or data on success, -1 on failure
 */
int objmapper_client_request(int sock, const char *uri, char mode);

/**
 * Close client connection
 * 
 * @param sock Client socket
 */
void objmapper_client_close(int sock);

#endif /* OBJMAPPER_H */
