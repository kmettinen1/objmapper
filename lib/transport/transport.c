/**
 * @file transport.c
 * @brief Network transport abstraction implementation
 */

#define _GNU_SOURCE
#include "transport.h"
#include "../fdpass/fdpass.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#ifdef DEBUG
#define dprintf(...) fprintf(stderr, __VA_ARGS__)
#else
#define dprintf(...) ((void)0)
#endif

/* Transport structure */
struct transport {
    transport_type_t type;
    int fd;
    int is_server;
    
    /* For UDP: peer address */
    struct sockaddr_storage peer_addr;
    socklen_t peer_addr_len;
    size_t max_packet_size;
};

/* Helper: create Unix domain socket server */
static int create_unix_server(const char *path, int backlog)
{
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    
    /* Remove existing socket file */
    unlink(path);
    
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sock);
        return -1;
    }
    
    if (listen(sock, backlog) < 0) {
        perror("listen");
        close(sock);
        return -1;
    }
    
    dprintf("Unix server listening on %s\n", path);
    return sock;
}

/* Helper: create TCP server */
static int create_tcp_server(const char *host, uint16_t port, int backlog)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }
    
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (host && strcmp(host, "*") != 0) {
        if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
            /* Try to resolve hostname */
            struct hostent *he = gethostbyname(host);
            if (!he) {
                fprintf(stderr, "Invalid host: %s\n", host);
                close(sock);
                return -1;
            }
            memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
        }
    } else {
        addr.sin_addr.s_addr = INADDR_ANY;
    }
    
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sock);
        return -1;
    }
    
    if (listen(sock, backlog) < 0) {
        perror("listen");
        close(sock);
        return -1;
    }
    
    dprintf("TCP server listening on %s:%u\n", 
            host ? host : "*", port);
    return sock;
}

/* Helper: create UDP server */
static int create_udp_server(const char *host, uint16_t port)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }
    
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (host && strcmp(host, "*") != 0) {
        if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
            struct hostent *he = gethostbyname(host);
            if (!he) {
                fprintf(stderr, "Invalid host: %s\n", host);
                close(sock);
                return -1;
            }
            memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
        }
    } else {
        addr.sin_addr.s_addr = INADDR_ANY;
    }
    
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sock);
        return -1;
    }
    
    dprintf("UDP server bound to %s:%u\n", 
            host ? host : "*", port);
    return sock;
}

transport_t *transport_server_create(const transport_config_t *config,
                                     int max_connections)
{
    if (!config) {
        errno = EINVAL;
        return NULL;
    }
    
    transport_t *transport = calloc(1, sizeof(transport_t));
    if (!transport) return NULL;
    
    transport->type = config->type;
    transport->is_server = 1;
    
    switch (config->type) {
        case TRANSPORT_UNIX:
            transport->fd = create_unix_server(config->unix_cfg.path,
                                              max_connections);
            break;
            
        case TRANSPORT_TCP:
            transport->fd = create_tcp_server(config->tcp_cfg.host,
                                             config->tcp_cfg.port,
                                             max_connections);
            break;
            
        case TRANSPORT_UDP:
            transport->fd = create_udp_server(config->udp_cfg.host,
                                             config->udp_cfg.port);
            transport->max_packet_size = config->udp_cfg.max_packet_size > 0 ?
                                        config->udp_cfg.max_packet_size : 8192;
            break;
            
        default:
            free(transport);
            errno = EINVAL;
            return NULL;
    }
    
    if (transport->fd < 0) {
        free(transport);
        return NULL;
    }
    
    return transport;
}

transport_t *transport_accept(transport_t *server)
{
    if (!server || !server->is_server) {
        errno = EINVAL;
        return NULL;
    }
    
    /* UDP doesn't have accept */
    if (server->type == TRANSPORT_UDP) {
        errno = EOPNOTSUPP;
        return NULL;
    }
    
    int client_fd = accept(server->fd, NULL, NULL);
    if (client_fd < 0) {
        return NULL;
    }
    
    transport_t *client = calloc(1, sizeof(transport_t));
    if (!client) {
        close(client_fd);
        return NULL;
    }
    
    client->type = server->type;
    client->fd = client_fd;
    client->is_server = 0;
    
    dprintf("Accepted connection: fd=%d, type=%d\n", client_fd, client->type);
    
    return client;
}

transport_t *transport_client_connect(const transport_config_t *config)
{
    if (!config) {
        errno = EINVAL;
        return NULL;
    }
    
    transport_t *transport = calloc(1, sizeof(transport_t));
    if (!transport) return NULL;
    
    transport->type = config->type;
    transport->is_server = 0;
    
    switch (config->type) {
        case TRANSPORT_UNIX: {
            transport->fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (transport->fd < 0) {
                free(transport);
                return NULL;
            }
            
            struct sockaddr_un addr;
            memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, config->unix_cfg.path,
                   sizeof(addr.sun_path) - 1);
            
            if (connect(transport->fd, (struct sockaddr *)&addr,
                       sizeof(addr)) < 0) {
                close(transport->fd);
                free(transport);
                return NULL;
            }
            
            dprintf("Connected to Unix socket: %s\n", config->unix_cfg.path);
            break;
        }
        
        case TRANSPORT_TCP: {
            transport->fd = socket(AF_INET, SOCK_STREAM, 0);
            if (transport->fd < 0) {
                free(transport);
                return NULL;
            }
            
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(config->tcp_cfg.port);
            
            if (inet_pton(AF_INET, config->tcp_cfg.host, &addr.sin_addr) <= 0) {
                struct hostent *he = gethostbyname(config->tcp_cfg.host);
                if (!he) {
                    close(transport->fd);
                    free(transport);
                    return NULL;
                }
                memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
            }
            
            if (connect(transport->fd, (struct sockaddr *)&addr,
                       sizeof(addr)) < 0) {
                close(transport->fd);
                free(transport);
                return NULL;
            }
            
            dprintf("Connected to TCP: %s:%u\n", 
                   config->tcp_cfg.host, config->tcp_cfg.port);
            break;
        }
        
        case TRANSPORT_UDP: {
            transport->fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (transport->fd < 0) {
                free(transport);
                return NULL;
            }
            
            /* Set up peer address for sendto/recvfrom */
            struct sockaddr_in *peer = (struct sockaddr_in *)&transport->peer_addr;
            memset(peer, 0, sizeof(*peer));
            peer->sin_family = AF_INET;
            peer->sin_port = htons(config->udp_cfg.port);
            
            if (inet_pton(AF_INET, config->udp_cfg.host, &peer->sin_addr) <= 0) {
                struct hostent *he = gethostbyname(config->udp_cfg.host);
                if (!he) {
                    close(transport->fd);
                    free(transport);
                    return NULL;
                }
                memcpy(&peer->sin_addr, he->h_addr_list[0], he->h_length);
            }
            
            transport->peer_addr_len = sizeof(*peer);
            transport->max_packet_size = config->udp_cfg.max_packet_size > 0 ?
                                        config->udp_cfg.max_packet_size : 8192;
            
            dprintf("UDP client setup for %s:%u\n",
                   config->udp_cfg.host, config->udp_cfg.port);
            break;
        }
        
        default:
            free(transport);
            errno = EINVAL;
            return NULL;
    }
    
    return transport;
}

ssize_t transport_send(transport_t *transport, const void *data, size_t len)
{
    if (!transport || !data) {
        errno = EINVAL;
        return -1;
    }
    
    if (transport->type == TRANSPORT_UDP) {
        /* UDP: use sendto */
        if (transport->peer_addr_len > 0) {
            return sendto(transport->fd, data, len, 0,
                         (struct sockaddr *)&transport->peer_addr,
                         transport->peer_addr_len);
        } else {
            /* Server without peer - can't send */
            errno = EDESTADDRREQ;
            return -1;
        }
    } else {
        /* Stream socket: use send */
        return send(transport->fd, data, len, 0);
    }
}

ssize_t transport_recv(transport_t *transport, void *buffer, size_t len)
{
    if (!transport || !buffer) {
        errno = EINVAL;
        return -1;
    }
    
    if (transport->type == TRANSPORT_UDP) {
        /* UDP: use recvfrom and update peer address */
        transport->peer_addr_len = sizeof(transport->peer_addr);
        return recvfrom(transport->fd, buffer, len, 0,
                       (struct sockaddr *)&transport->peer_addr,
                       &transport->peer_addr_len);
    } else {
        /* Stream socket: use recv */
        return recv(transport->fd, buffer, len, 0);
    }
}

int transport_send_fd(transport_t *transport, int fd, char metadata)
{
    if (!transport) {
        errno = EINVAL;
        return -1;
    }
    
    /* Only Unix sockets support FD passing */
    if (transport->type != TRANSPORT_UNIX) {
        errno = EOPNOTSUPP;
        return -1;
    }
    
    return fdpass_send(transport->fd, NULL, fd, metadata);
}

int transport_recv_fd(transport_t *transport, char *metadata)
{
    if (!transport) {
        errno = EINVAL;
        return -1;
    }
    
    /* Only Unix sockets support FD passing */
    if (transport->type != TRANSPORT_UNIX) {
        errno = EOPNOTSUPP;
        return -1;
    }
    
    return fdpass_recv(transport->fd, metadata);
}

int transport_get_fd(transport_t *transport)
{
    return transport ? transport->fd : -1;
}

int transport_get_caps(transport_t *transport, transport_caps_t *caps)
{
    if (!transport || !caps) {
        errno = EINVAL;
        return -1;
    }
    
    memset(caps, 0, sizeof(*caps));
    
    switch (transport->type) {
        case TRANSPORT_UNIX:
            caps->supports_fdpass = 1;
            caps->is_stream = 1;
            caps->is_connection_oriented = 1;
            break;
            
        case TRANSPORT_TCP:
            caps->supports_fdpass = 0;
            caps->is_stream = 1;
            caps->is_connection_oriented = 1;
            break;
            
        case TRANSPORT_UDP:
            caps->supports_fdpass = 0;
            caps->is_stream = 0;
            caps->is_connection_oriented = 0;
            break;
    }
    
    return 0;
}

transport_type_t transport_get_type(transport_t *transport)
{
    return transport ? transport->type : -1;
}

void transport_close(transport_t *transport)
{
    if (!transport) return;
    
    if (transport->fd >= 0) {
        close(transport->fd);
    }
    
    dprintf("Closed transport: fd=%d, type=%d\n", 
           transport->fd, transport->type);
    
    free(transport);
}
