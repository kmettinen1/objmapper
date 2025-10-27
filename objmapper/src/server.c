/**
 * @file server.c
 * @brief Objmapper server implementation
 */

#define _GNU_SOURCE
#include "objmapper.h"
#include "../../lib/storage/storage.h"
#include "../../lib/fdpass/fdpass.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

#ifdef DEBUG
#define dprintf(...) fprintf(stderr, __VA_ARGS__)
#else
#define dprintf(...) ((void)0)
#endif

typedef struct {
    int client_sock;
    object_storage_t *storage;
    char operation_mode;
} session_t;

static void *handle_client(void *arg)
{
    session_t *session = (session_t *)arg;
    char buffer[1024];
    ssize_t bytes_read;
    
    dprintf("handle_client: started for sock=%d\n", session->client_sock);
    
    /* Send mode acknowledgment */
    char mode_response[4] = "200";
    write(session->client_sock, mode_response, 3);
    
    while ((bytes_read = read(session->client_sock, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytes_read] = '\0';
        
        dprintf("handle_client: request uri='%s'\n", buffer);
        
        object_info_t info;
        int fd = storage_get_fd(session->storage, buffer, &info);
        
        if (fd < 0) {
            dprintf("handle_client: object not found uri='%s'\n", buffer);
            ssize_t size = 0;
            write(session->client_sock, &size, sizeof(size));
            continue;
        }
        
        /* Handle based on operation mode */
        switch (session->operation_mode) {
            case OP_FDPASS: {
                /* Send file descriptor directly */
                if (fdpass_send(session->client_sock, NULL, fd, OP_FDPASS) < 0) {
                    dprintf("handle_client: fdpass_send failed\n");
                }
                close(fd);
                break;
            }
            
            case OP_COPY: {
                /* Send size then data */
                ssize_t size = info.size;
                write(session->client_sock, &size, sizeof(size));
                
                char data_buf[8192];
                ssize_t total_sent = 0;
                ssize_t n;
                
                lseek(fd, 0, SEEK_SET);
                while ((n = read(fd, data_buf, sizeof(data_buf))) > 0) {
                    ssize_t sent = write(session->client_sock, data_buf, n);
                    if (sent < 0) break;
                    total_sent += sent;
                }
                close(fd);
                
                dprintf("handle_client: copied %zd bytes\n", total_sent);
                break;
            }
            
            case OP_SPLICE: {
                /* Send size then splice data */
                ssize_t size = info.size;
                write(session->client_sock, &size, sizeof(size));
                
                off_t offset = 0;
                ssize_t total_spliced = 0;
                
                while (offset < (off_t)info.size) {
                    ssize_t spliced = splice(fd, &offset, session->client_sock, NULL,
                                            info.size - offset, SPLICE_F_MOVE);
                    if (spliced <= 0) break;
                    total_spliced += spliced;
                }
                close(fd);
                
                dprintf("handle_client: spliced %zd bytes\n", total_spliced);
                break;
            }
            
            default:
                close(fd);
                break;
        }
    }
    
    close(session->client_sock);
    free(session);
    
    dprintf("handle_client: finished\n");
    return NULL;
}

int objmapper_server_start(const server_config_t *config)
{
    if (!config || !config->socket_path || !config->backing_dir) {
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
    
    /* Create Unix domain socket */
    int server_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("socket");
        storage_cleanup(storage);
        return -1;
    }
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, config->socket_path, sizeof(addr.sun_path) - 1);
    
    /* Remove existing socket file */
    unlink(config->socket_path);
    
    if (bind(server_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_sock);
        storage_cleanup(storage);
        return -1;
    }
    
    if (listen(server_sock, config->max_connections > 0 ? 
               config->max_connections : 10) < 0) {
        perror("listen");
        close(server_sock);
        storage_cleanup(storage);
        return -1;
    }
    
    printf("Objmapper server listening on %s\n", config->socket_path);
    printf("Backing dir: %s\n", config->backing_dir);
    printf("Cache dir: %s\n", config->cache_dir ? config->cache_dir : "none");
    printf("Cache limit: %zu bytes\n", config->cache_limit);
    
    /* Accept connections */
    while (1) {
        int client_sock = accept(server_sock, NULL, NULL);
        if (client_sock < 0) {
            perror("accept");
            continue;
        }
        
        /* Read operation mode from client */
        char mode = OP_FDPASS;
        read(client_sock, &mode, 1);
        
        dprintf("New client connection: sock=%d, mode=%c\n", client_sock, mode);
        
        /* Create session */
        session_t *session = malloc(sizeof(session_t));
        if (!session) {
            close(client_sock);
            continue;
        }
        
        session->client_sock = client_sock;
        session->storage = storage;
        session->operation_mode = mode;
        
        /* Handle in new thread */
        pthread_t thread;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        
        if (pthread_create(&thread, &attr, handle_client, session) != 0) {
            perror("pthread_create");
            close(client_sock);
            free(session);
        }
        
        pthread_attr_destroy(&attr);
    }
    
    close(server_sock);
    storage_cleanup(storage);
    unlink(config->socket_path);
    
    return 0;
}
