/**
 * @file client.c
 * @brief Objmapper client implementation
 */

#define _GNU_SOURCE
#include "objmapper.h"
#include "../../lib/fdpass/fdpass.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

#ifdef DEBUG
#define dprintf(...) fprintf(stderr, __VA_ARGS__)
#else
#define dprintf(...) ((void)0)
#endif

int objmapper_client_connect(const client_config_t *config)
{
    if (!config || !config->socket_path) {
        errno = EINVAL;
        return -1;
    }
    
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, config->socket_path, sizeof(addr.sun_path) - 1);
    
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return -1;
    }
    
    /* Send operation mode */
    char mode = config->operation_mode;
    if (write(sock, &mode, 1) != 1) {
        perror("write mode");
        close(sock);
        return -1;
    }
    
    /* Wait for acknowledgment */
    char response[4];
    if (read(sock, response, 3) != 3) {
        perror("read ack");
        close(sock);
        return -1;
    }
    
    dprintf("objmapper_client_connect: connected sock=%d, mode=%c\n", sock, mode);
    
    return sock;
}

int objmapper_client_request(int sock, const char *uri, char mode)
{
    if (sock < 0 || !uri) {
        errno = EINVAL;
        return -1;
    }
    
    /* Send URI request */
    size_t uri_len = strlen(uri);
    if (write(sock, uri, uri_len) != (ssize_t)uri_len) {
        perror("write uri");
        return -1;
    }
    
    dprintf("objmapper_client_request: uri='%s', mode=%c\n", uri, mode);
    
    /* Handle response based on mode */
    switch (mode) {
        case OP_FDPASS: {
            /* Receive file descriptor */
            char op_type;
            int fd = fdpass_recv(sock, &op_type);
            if (fd < 0) {
                dprintf("objmapper_client_request: fdpass_recv failed\n");
                return -1;
            }
            dprintf("objmapper_client_request: received fd=%d\n", fd);
            return fd;
        }
        
        case OP_COPY:
        case OP_SPLICE: {
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
                
                write(fd, buffer, n);
                total_read += n;
            }
            
            lseek(fd, 0, SEEK_SET);
            dprintf("objmapper_client_request: received %zd bytes\n", total_read);
            
            return fd;
        }
        
        default:
            errno = EINVAL;
            return -1;
    }
}

void objmapper_client_close(int sock)
{
    if (sock >= 0) {
        close(sock);
        dprintf("objmapper_client_close: sock=%d\n", sock);
    }
}
