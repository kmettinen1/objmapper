/**
 * @file example_server.c
 * @brief Simple example of objmapper protocol server
 */

#include "protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static int create_server_socket(const char *socket_path) {
    /* Remove existing socket */
    unlink(socket_path);
    
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
    
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    
    if (listen(fd, 5) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    
    return fd;
}

static void handle_request(objm_connection_t *conn, objm_request_t *req) {
    printf("  Request: id=%u, mode=%c (%s), uri=%s\n",
           req->id, req->mode, objm_mode_name(req->mode), req->uri);

    objm_segment_t segments[3];
    memset(segments, 0, sizeof(segments));

    const char *behavior = NULL;
    const char *path_hint = NULL;
    char *path_storage = NULL;

    path_hint = strstr(req->uri, "::");
    if (path_hint) {
        size_t path_len = (size_t)(path_hint - req->uri);
        path_storage = malloc(path_len + 1);
        if (!path_storage) {
            objm_server_send_error(conn, req->id, OBJM_STATUS_OUT_OF_MEMORY,
                                  "Out of memory");
            fprintf(stderr, "  Response: OUT_OF_MEMORY\n");
            return;
        }
        memcpy(path_storage, req->uri, path_len);
        path_storage[path_len] = '\0';
        behavior = path_hint + 2;
    }

    const char *open_path = path_storage ? path_storage : req->uri;

    /* Simple file lookup - just try to open the URI as a file path */
    int file_fd = open(open_path, O_RDONLY);
    if (file_fd < 0) {
        /* File not found */
        objm_server_send_error(conn, req->id, OBJM_STATUS_NOT_FOUND,
                              strerror(errno));
        printf("  Response: NOT_FOUND (%s)\n", strerror(errno));
        free(path_storage);
        return;
    }
    
    /* Get file size */
    struct stat st;
    if (fstat(file_fd, &st) < 0) {
        close(file_fd);
        objm_server_send_error(conn, req->id, OBJM_STATUS_STORAGE_ERROR,
                              "fstat failed");
        printf("  Response: STORAGE_ERROR\n");
        free(path_storage);
        return;
    }
    
    /* Build metadata */
    uint8_t *metadata = objm_metadata_create(100);
    size_t meta_len = 0;
    
    meta_len = objm_metadata_add_size(metadata, meta_len, st.st_size);
    meta_len = objm_metadata_add_mtime(metadata, meta_len, st.st_mtime);
    meta_len = objm_metadata_add_backend(metadata, meta_len, 1);  /* Backend 1 = local disk */
    
    /* Build response */
    objm_response_t resp = {
        .request_id = req->id,
        .status = OBJM_STATUS_OK,
        .metadata = metadata,
        .metadata_len = meta_len,
    };
    
    if (req->mode == OBJM_MODE_FDPASS) {
        /* FD pass mode - send file descriptor */
        resp.fd = file_fd;
        resp.content_len = 0;
    } else if (req->mode == OBJM_MODE_SEGMENTED) {
        static const char inline_prefix[] = "inline-prelude:\n";

        segments[0] = (objm_segment_t){
            .type = OBJM_SEG_TYPE_INLINE,
            .flags = 0,
            .copy_length = sizeof(inline_prefix) - 1,
            .logical_length = sizeof(inline_prefix) - 1,
            .storage_offset = 0,
            .storage_length = sizeof(inline_prefix) - 1,
            .inline_data = (uint8_t *)inline_prefix,
            .fd = -1,
        };

        segments[1] = (objm_segment_t){
            .type = OBJM_SEG_TYPE_FD,
            .flags = OBJM_SEG_FLAG_FIN,
            .copy_length = 0,
            .logical_length = st.st_size,
            .storage_offset = 0,
            .storage_length = st.st_size,
            .inline_data = NULL,
            .fd = file_fd,
        };

        int reuse_segments = 0;
        int optional_inline = 0;

        if (behavior && *behavior) {
            if (strstr(behavior, "reuse")) {
                reuse_segments = (st.st_size > 1);
            }
            if (strstr(behavior, "optional")) {
                optional_inline = 1;
            }
        }

        if (optional_inline) {
            segments[0].flags |= OBJM_SEG_FLAG_OPTIONAL;
        }

        resp.fd = -1;

        if (reuse_segments) {
            off_t first_len = st.st_size / 2;
            if (first_len <= 0 || first_len >= st.st_size) {
                reuse_segments = 0;
            }
        }

        if (reuse_segments) {
            off_t first_len = st.st_size / 2;
            off_t second_len = st.st_size - first_len;

            segments[1].flags = 0;
            segments[1].logical_length = first_len;
            segments[1].storage_length = first_len;
            segments[1].storage_offset = 0;

            segments[2] = (objm_segment_t){
                .type = OBJM_SEG_TYPE_FD,
                .flags = OBJM_SEG_FLAG_FIN | OBJM_SEG_FLAG_REUSE_FD,
                .copy_length = 0,
                .logical_length = second_len,
                .storage_offset = first_len,
                .storage_length = second_len,
                .inline_data = NULL,
                .fd = file_fd,
            };

            resp.segment_count = 3;
            resp.segments = segments;
            resp.content_len = segments[0].logical_length +
                                segments[1].logical_length +
                                segments[2].logical_length;
        } else {
            resp.segment_count = 2;
            resp.segments = segments;
            resp.content_len = segments[0].logical_length + segments[1].logical_length;
        }

    } else {
        /* Copy/splice mode - send content length (actual data transfer not implemented) */
        resp.fd = -1;
        resp.content_len = st.st_size;
        close(file_fd);
    }
    
    if (req->mode == OBJM_MODE_SEGMENTED) {
        printf("  Response: OK, segments=%u, total=%zu bytes\n",
               resp.segment_count, resp.content_len);
    } else {
        printf("  Response: OK, size=%lu bytes, mode=%s\n",
               st.st_size, objm_mode_name(req->mode));
    }
    
    if (objm_server_send_response(conn, &resp) < 0) {
        const char *err = objm_last_error(conn);
        fprintf(stderr, "  Failed to send response%s%s\n",
                err ? ": " : "",
                err ? err : "");
    }
    
    free(metadata);
    free(path_storage);
    
    /* Note: resp.fd is closed by objm_response_free, but we manage it ourselves */
    if (req->mode == OBJM_MODE_FDPASS || req->mode == OBJM_MODE_SEGMENTED) {
        close(file_fd);
    }
}

static void handle_client(int client_fd) {
    printf("New client connected (fd=%d)\n", client_fd);
    
    /* Create server connection */
    objm_connection_t *conn = objm_server_create(client_fd);
    if (!conn) {
        fprintf(stderr, "Failed to create server connection\n");
        return;
    }
    
    /* Perform handshake and detect protocol version */
    objm_hello_t hello = {
        .capabilities = OBJM_CAP_OOO_REPLIES | OBJM_CAP_PIPELINING |
                        OBJM_CAP_SEGMENTED_DELIVERY,
        .max_pipeline = 100,
        .backend_parallelism = 3  /* SSD, NFS, S3 */
    };
    
    objm_params_t params;
    if (objm_server_handshake(conn, &hello, &params) < 0) {
        fprintf(stderr, "Handshake failed\n");
        objm_server_destroy(conn);
        return;
    }
    
    char cap_str[256];
    objm_capability_names(params.capabilities, cap_str, sizeof(cap_str));
    printf("Negotiated: version=%d, caps=0x%04x (%s), pipeline=%d\n",
        params.version, params.capabilities, cap_str, params.max_pipeline);
    if (params.capabilities & OBJM_CAP_SEGMENTED_DELIVERY) {
        printf("  Segmented delivery enabled\n");
    }
    
    /* Process requests in a loop */
    while (1) {
        objm_request_t *req = NULL;
        int ret = objm_server_recv_request(conn, &req);
        
        if (ret < 0) {
            fprintf(stderr, "Failed to receive request\n");
            break;
        }
        
        if (ret == 1) {
            /* Client requested close */
            printf("Client requested close\n");
            objm_server_send_close_ack(conn, 0);
            break;
        }
        
        /* Handle request */
        handle_request(conn, req);
        
        objm_request_free(req);
    }
    
    objm_server_destroy(conn);
    printf("Client disconnected\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <socket_path>\n", argv[0]);
        return 1;
    }
    
    const char *socket_path = argv[1];
    
    /* Create server socket */
    int server_fd = create_server_socket(socket_path);
    if (server_fd < 0) {
        return 1;
    }
    
    printf("Server listening on %s\n", socket_path);
    
    /* Accept clients in a loop */
    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        
        /* Handle client (in this simple example, one at a time) */
        handle_client(client_fd);
        close(client_fd);
    }
    
    close(server_fd);
    unlink(socket_path);
    
    return 0;
}
