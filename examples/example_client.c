/**
 * @file example_client.c
 * @brief Simple example of objmapper protocol client
 */

#include "protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int connect_to_server(const char *socket_path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
    
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }
    
    return fd;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <socket_path> <uri> [mode]\n", argv[0]);
        fprintf(stderr, "  mode: 1=fdpass (default), 2=copy, 3=splice\n");
        return 1;
    }
    
    const char *socket_path = argv[1];
    const char *uri = argv[2];
    char mode = (argc > 3) ? argv[3][0] : OBJM_MODE_FDPASS;
    
    /* Connect to server */
    int sock = connect_to_server(socket_path);
    if (sock < 0) {
        return 1;
    }
    
    printf("Connected to server at %s\n", socket_path);
    
    /* Create client connection (V2 with OOO support) */
    objm_connection_t *conn = objm_client_create(sock, OBJM_PROTO_V2);
    if (!conn) {
        fprintf(stderr, "Failed to create client connection\n");
        close(sock);
        return 1;
    }
    
    /* Send HELLO and negotiate capabilities */
    objm_hello_t hello = {
        .capabilities = OBJM_CAP_OOO_REPLIES | OBJM_CAP_PIPELINING,
        .max_pipeline = 100,
        .backend_parallelism = 0  /* Client field, unused */
    };
    
    objm_params_t params;
    if (objm_client_hello(conn, &hello, &params) < 0) {
        fprintf(stderr, "Handshake failed\n");
        objm_client_destroy(conn);
        close(sock);
        return 1;
    }
    
    char cap_str[256];
    objm_capability_names(params.capabilities, cap_str, sizeof(cap_str));
    printf("Negotiated: version=%d, caps=%s, pipeline=%d, backends=%d\n",
           params.version, cap_str, params.max_pipeline, params.backend_parallelism);
    
    /* Send request */
    objm_request_t req = {
        .id = 1,
        .flags = 0,  /* No special flags */
        .mode = mode,
        .uri = (char *)uri,
        .uri_len = strlen(uri)
    };
    
    printf("Sending request: id=%u, mode=%c (%s), uri=%s\n",
           req.id, req.mode, objm_mode_name(req.mode), req.uri);
    
    if (objm_client_send_request(conn, &req) < 0) {
        fprintf(stderr, "Failed to send request\n");
        objm_client_destroy(conn);
        close(sock);
        return 1;
    }
    
    /* Receive response */
    objm_response_t *resp = NULL;
    if (objm_client_recv_response(conn, &resp) < 0) {
        fprintf(stderr, "Failed to receive response\n");
        objm_client_destroy(conn);
        close(sock);
        return 1;
    }
    
    printf("Received response: id=%u, status=%s\n",
           resp->request_id, objm_status_name(resp->status));
    
    if (resp->status == OBJM_STATUS_OK) {
        if (resp->fd >= 0) {
            /* FD pass mode - got a file descriptor */
            printf("  Received FD: %d\n", resp->fd);
            
            /* Read and display file size */
            off_t size = lseek(resp->fd, 0, SEEK_END);
            if (size >= 0) {
                printf("  File size: %ld bytes\n", size);
            }
        } else if (resp->content_len > 0) {
            /* Copy/splice mode - got content length */
            printf("  Content length: %zu bytes\n", resp->content_len);
        }
        
        /* Parse metadata if present */
        if (resp->metadata_len > 0) {
            objm_metadata_entry_t *entries = NULL;
            size_t num_entries = 0;
            
            if (objm_metadata_parse(resp->metadata, resp->metadata_len,
                                   &entries, &num_entries) == 0) {
                printf("  Metadata (%zu entries):\n", num_entries);
                
                for (size_t i = 0; i < num_entries; i++) {
                    printf("    Type 0x%02x: %u bytes\n",
                           entries[i].type, entries[i].length);
                    
                    if (entries[i].type == OBJM_META_SIZE && entries[i].length == 8) {
                        uint64_t size = be64toh(*(uint64_t *)entries[i].data);
                        printf("      Size: %lu bytes\n", size);
                    } else if (entries[i].type == OBJM_META_BACKEND && entries[i].length == 1) {
                        printf("      Backend: %u\n", entries[i].data[0]);
                    }
                }
                
                objm_metadata_free_entries(entries, num_entries);
            }
        }
    } else {
        fprintf(stderr, "  Error: %s\n", objm_status_name(resp->status));
    }
    
    objm_response_free(resp);
    
    /* Close gracefully */
    objm_client_close(conn, OBJM_CLOSE_NORMAL);
    objm_client_destroy(conn);
    close(sock);
    
    printf("Done\n");
    return 0;
}
