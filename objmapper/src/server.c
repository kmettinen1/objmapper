/**
 * @file server.c
 * @brief Objmapper server implementation
 */

#define _GNU_SOURCE
#include "objmapper.h"
#include "../../lib/storage/storage.h"
#include "../../lib/transport/transport.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>

#ifdef DEBUG
#define dprintf(...) fprintf(stderr, __VA_ARGS__)
#else
#define dprintf(...) ((void)0)
#endif

typedef struct {
    transport_t *transport;
    object_storage_t *storage;
    char operation_mode;
    transport_caps_t caps;
} session_t;

static void *handle_client(void *arg)
{
    session_t *session = (session_t *)arg;
    char buffer[1024];
    ssize_t bytes_read;
    
    dprintf("handle_client: started, supports_fdpass=%d\n", 
            session->caps.supports_fdpass);
    
    /* Send mode acknowledgment */
    char mode_response[4] = "200";
    transport_send(session->transport, mode_response, 3);
    
    while ((bytes_read = transport_recv(session->transport, buffer, 
                                       sizeof(buffer) - 1)) > 0) {
        buffer[bytes_read] = '\0';
        
        dprintf("handle_client: request uri='%s'\n", buffer);
        
        object_info_t info;
        int fd = storage_get_fd(session->storage, buffer, &info);
        
        if (fd < 0) {
            dprintf("handle_client: object not found uri='%s'\n", buffer);
            ssize_t size = 0;
            transport_send(session->transport, &size, sizeof(size));
            continue;
        }
        
        /* Handle based on operation mode and transport capabilities */
        if (session->operation_mode == OP_FDPASS && session->caps.supports_fdpass) {
            /* FD passing mode (Unix sockets only) */
            if (transport_send_fd(session->transport, fd, OP_FDPASS) < 0) {
                dprintf("handle_client: transport_send_fd failed\n");
            }
            close(fd);
        } else if (session->operation_mode == OP_SPLICE && session->caps.is_stream) {
            /* Splice mode (stream transports) */
            ssize_t size = info.size;
            transport_send(session->transport, &size, sizeof(size));
            
            off_t offset = 0;
            ssize_t total_spliced = 0;
            int trans_fd = transport_get_fd(session->transport);
            
            while (offset < (off_t)info.size) {
                ssize_t spliced = splice(fd, &offset, trans_fd, NULL,
                                        info.size - offset, SPLICE_F_MOVE);
                if (spliced <= 0) break;
                total_spliced += spliced;
            }
            close(fd);
            
            dprintf("handle_client: spliced %zd bytes\n", total_spliced);
        } else {
            /* Copy mode (all transports) */
            ssize_t size = info.size;
            transport_send(session->transport, &size, sizeof(size));
            
            char data_buf[8192];
            ssize_t total_sent = 0;
            ssize_t n;
            
            lseek(fd, 0, SEEK_SET);
            while ((n = read(fd, data_buf, sizeof(data_buf))) > 0) {
                ssize_t sent = transport_send(session->transport, data_buf, n);
                if (sent < 0) break;
                total_sent += sent;
            }
            close(fd);
            
            dprintf("handle_client: copied %zd bytes\n", total_sent);
        }
    }
    
    transport_close(session->transport);
    free(session);
    
    dprintf("handle_client: finished\n");
    return NULL;
}

int objmapper_server_start(const server_config_t *config)
{
    if (!config || !config->backing_dir) {
        errno = EINVAL;
        return -1;
    }
    
    /* Initialize storage */
    storage_config_t storage_cfg = {
        .backing_dir = config->backing_dir,
        .cache_dir = config->cache_dir,
        .cache_limit = config->cache_limit,
        .hash_size = 0
    };
    
    object_storage_t *storage = storage_init(&storage_cfg);
    if (!storage) {
        fprintf(stderr, "Failed to initialize storage\n");
        return -1;
    }
    
    /* Create transport server */
    transport_config_t trans_cfg;
    memset(&trans_cfg, 0, sizeof(trans_cfg));
    
    switch (config->transport) {
        case OBJMAPPER_TRANSPORT_UNIX:
            trans_cfg.type = TRANSPORT_UNIX;
            trans_cfg.unix_cfg.path = config->socket_path ? 
                                     config->socket_path : OBJMAPPER_SOCK_PATH;
            printf("Starting Unix socket server on %s\n", 
                   trans_cfg.unix_cfg.path);
            break;
            
        case OBJMAPPER_TRANSPORT_TCP:
            trans_cfg.type = TRANSPORT_TCP;
            trans_cfg.tcp_cfg.host = config->net.host;
            trans_cfg.tcp_cfg.port = config->net.port > 0 ? 
                                    config->net.port : OBJMAPPER_TCP_PORT;
            printf("Starting TCP server on %s:%u\n",
                   trans_cfg.tcp_cfg.host ? trans_cfg.tcp_cfg.host : "*",
                   trans_cfg.tcp_cfg.port);
            break;
            
        case OBJMAPPER_TRANSPORT_UDP:
            trans_cfg.type = TRANSPORT_UDP;
            trans_cfg.udp_cfg.host = config->net.host;
            trans_cfg.udp_cfg.port = config->net.port > 0 ?
                                    config->net.port : OBJMAPPER_UDP_PORT;
            trans_cfg.udp_cfg.max_packet_size = 8192;
            printf("Starting UDP server on %s:%u\n",
                   trans_cfg.udp_cfg.host ? trans_cfg.udp_cfg.host : "*",
                   trans_cfg.udp_cfg.port);
            break;
            
        default:
            fprintf(stderr, "Invalid transport type\n");
            storage_cleanup(storage);
            return -1;
    }
    
    transport_t *server_transport = transport_server_create(&trans_cfg,
                                     config->max_connections > 0 ? 
                                     config->max_connections : 10);
    if (!server_transport) {
        fprintf(stderr, "Failed to create transport server\n");
        storage_cleanup(storage);
        return -1;
    }
    
    printf("Backing dir: %s\n", config->backing_dir);
    printf("Cache dir: %s\n", config->cache_dir ? config->cache_dir : "none");
    printf("Cache limit: %zu bytes\n", config->cache_limit);
    
    transport_caps_t server_caps;
    transport_get_caps(server_transport, &server_caps);
    printf("Transport capabilities: fdpass=%d, stream=%d\n",
           server_caps.supports_fdpass, server_caps.is_stream);
    
    /* Accept connections (for stream transports) or handle datagrams */
    if (server_caps.is_connection_oriented) {
        /* Stream-based: accept connections */
        while (1) {
            transport_t *client_transport = transport_accept(server_transport);
            if (!client_transport) {
                perror("accept");
                continue;
            }
            
            /* Read operation mode from client */
            char mode = OP_COPY;
            transport_recv(client_transport, &mode, 1);
            
            /* Validate mode based on transport capabilities */
            transport_caps_t client_caps;
            transport_get_caps(client_transport, &client_caps);
            
            if (mode == OP_FDPASS && !client_caps.supports_fdpass) {
                dprintf("Client requested FD passing on non-Unix transport, using copy\n");
                mode = OP_COPY;
            }
            
            dprintf("New client connection: mode=%c\n", mode);
            
            /* Create session */
            session_t *session = malloc(sizeof(session_t));
            if (!session) {
                transport_close(client_transport);
                continue;
            }
            
            session->transport = client_transport;
            session->storage = storage;
            session->operation_mode = mode;
            session->caps = client_caps;
            
            /* Handle in new thread */
            pthread_t thread;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            
            if (pthread_create(&thread, &attr, handle_client, session) != 0) {
                perror("pthread_create");
                transport_close(client_transport);
                free(session);
            }
            
            pthread_attr_destroy(&attr);
        }
    } else {
        /* Datagram-based (UDP): handle requests directly */
        fprintf(stderr, "UDP server mode not yet implemented\n");
        /* TODO: Implement UDP request handling */
    }
    
    transport_close(server_transport);
    storage_cleanup(storage);
    
    return 0;
}
