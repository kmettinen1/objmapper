/**
 * @file client.c
 * @brief Objmapper client implementation
 */

#define _GNU_SOURCE
#include "objmapper.h"
#include "../../lib/transport/transport.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#ifdef DEBUG
#define dprintf(...) fprintf(stderr, __VA_ARGS__)
#else
#define dprintf(...) ((void)0)
#endif

int objmapper_client_connect(const client_config_t *config)
{
    if (!config) {
        errno = EINVAL;
        return -1;
    }
    
    /* Create transport configuration */
    transport_config_t trans_cfg;
    memset(&trans_cfg, 0, sizeof(trans_cfg));
    
    switch (config->transport) {
        case OBJMAPPER_TRANSPORT_UNIX:
            trans_cfg.type = TRANSPORT_UNIX;
            trans_cfg.unix_cfg.path = config->socket_path ?
                                     config->socket_path : OBJMAPPER_SOCK_PATH;
            break;
            
        case OBJMAPPER_TRANSPORT_TCP:
            trans_cfg.type = TRANSPORT_TCP;
            trans_cfg.tcp_cfg.host = config->net.host;
            trans_cfg.tcp_cfg.port = config->net.port > 0 ?
                                    config->net.port : OBJMAPPER_TCP_PORT;
            break;
            
        case OBJMAPPER_TRANSPORT_UDP:
            trans_cfg.type = TRANSPORT_UDP;
            trans_cfg.udp_cfg.host = config->net.host;
            trans_cfg.udp_cfg.port = config->net.port > 0 ?
                                    config->net.port : OBJMAPPER_UDP_PORT;
            trans_cfg.udp_cfg.max_packet_size = 8192;
            break;
            
        default:
            errno = EINVAL;
            return -1;
    }
    
    transport_t *transport = transport_client_connect(&trans_cfg);
    if (!transport) {
        return -1;
    }
    
    /* Send operation mode */
    char mode = config->operation_mode;
    if (transport_send(transport, &mode, 1) != 1) {
        perror("send mode");
        transport_close(transport);
        return -1;
    }
    
    /* Wait for acknowledgment */
    char response[4];
    if (transport_recv(transport, response, 3) != 3) {
        perror("recv ack");
        transport_close(transport);
        return -1;
    }
    
    dprintf("objmapper_client_connect: connected, mode=%c\n", mode);
    
    /* Return the transport file descriptor */
    return transport_get_fd(transport);
}

int objmapper_client_request(int sock, const char *uri, char mode)
{
    if (sock < 0 || !uri) {
        errno = EINVAL;
        return -1;
    }
    
    /* Create transport wrapper for existing socket */
    /* Note: This is a simplified approach - in production, we'd track transport objects */
    
    /* Send URI request */
    size_t uri_len = strlen(uri);
    if (write(sock, uri, uri_len) != (ssize_t)uri_len) {
        perror("write uri");
        return -1;
    }
    
    dprintf("objmapper_client_request: uri='%s', mode=%c\n", uri, mode);
    
    /* Handle response based on mode */
    if (mode == OP_FDPASS) {
        /* Receive file descriptor (Unix sockets only) */
        #include "../../lib/fdpass/fdpass.h"
        
        char op_type;
        int fd = fdpass_recv(sock, &op_type);
        
        if (fd < 0) {
            dprintf("objmapper_client_request: fdpass_recv failed\n");
            return -1;
        }
        dprintf("objmapper_client_request: received fd=%d\n", fd);
        return fd;
        
    } else {
        /* Read size first */
        ssize_t size;
        if (read(sock, &size, sizeof(size)) != sizeof(size)) {
            perror("read size");
            return -1;
        }
        
        if (size <= 0) {
            dprintf("objmapper_client_request: object not found\n");
            errno = ENOENT;
            return -1;
        }
        
        dprintf("objmapper_client_request: receiving %zd bytes\n", size);
        
        /* Create temporary file to receive data */
        char temp_path[] = "/tmp/objmapper_XXXXXX";
        int fd = mkstemp(temp_path);
        if (fd < 0) {
            perror("mkstemp");
            return -1;
        }
        unlink(temp_path); /* Auto-delete on close */
        
        /* Read data */
        char buffer[8192];
        ssize_t total_read = 0;
        
        while (total_read < size) {
            ssize_t to_read = size - total_read;
            if (to_read > (ssize_t)sizeof(buffer)) {
                to_read = sizeof(buffer);
            }
            
            ssize_t n = read(sock, buffer, to_read);
            if (n <= 0) break;
            
            ssize_t written = 0;
            while (written < n) {
                ssize_t w = write(fd, buffer + written, n - written);
                if (w <= 0) {
                    close(fd);
                    return -1;
                }
                written += w;
            }
            total_read += n;
        }
        
        lseek(fd, 0, SEEK_SET);
        dprintf("objmapper_client_request: received %zd bytes\n", total_read);
        
        return fd;
    }
}

void objmapper_client_close(int sock)
{
    if (sock >= 0) {
        close(sock);
        dprintf("objmapper_client_close: sock=%d\n", sock);
    }
}
