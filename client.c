/**
 * @file client.c
 * @brief objmapper client with Unix socket and FD passing
 * 
 * Command-line client for objmapper server:
 * - Connects via Unix socket
 * - Uses FD passing for zero-copy GET/PUT
 * - Simple command interface
 */

#include "lib/protocol/protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

#define DEFAULT_SOCKET_PATH "/tmp/objmapper.sock"
#define BUFFER_SIZE (64 * 1024)

/* ============================================================================
 * Client Commands
 * ============================================================================ */

static int cmd_put(objm_connection_t *conn, const char *uri, const char *source_path) {
    /* Send PUT request */
    objm_request_t req = {
        .id = 0,
        .flags = 0,
        .mode = OBJM_MODE_FDPASS,
        .uri = (char *)uri,
        .uri_len = strlen(uri)
    };
    
    printf("PUT %s <- %s\n", uri, source_path);
    
    if (objm_client_send_request(conn, &req) < 0) {
        fprintf(stderr, "Failed to send PUT request\n");
        return -1;
    }
    
    /* Receive response with FD */
    objm_response_t *resp = NULL;
    if (objm_client_recv_response(conn, &resp) < 0) {
        fprintf(stderr, "Failed to receive response\n");
        return -1;
    }
    
    if (resp->status != OBJM_STATUS_OK) {
        fprintf(stderr, "PUT failed: %s\n",
                resp->error_msg ? resp->error_msg : "Unknown error");
        objm_response_free(resp);
        return -1;
    }
    
    /* We got an FD - write data to it */
    int dest_fd = resp->fd;
    if (dest_fd < 0) {
        fprintf(stderr, "No FD received\n");
        objm_response_free(resp);
        return -1;
    }
    
    /* Open source file */
    int src_fd = open(source_path, O_RDONLY);
    if (src_fd < 0) {
        perror("open source file");
        close(dest_fd);
        objm_response_free(resp);
        return -1;
    }
    
    /* Copy data */
    char buffer[BUFFER_SIZE];
    ssize_t total_written = 0;
    ssize_t bytes_read;
    
    while ((bytes_read = read(src_fd, buffer, sizeof(buffer))) > 0) {
        ssize_t bytes_written = write(dest_fd, buffer, bytes_read);
        if (bytes_written < 0) {
            perror("write");
            close(src_fd);
            close(dest_fd);
            objm_response_free(resp);
            return -1;
        }
        total_written += bytes_written;
    }
    
    close(src_fd);
    close(dest_fd);
    
    printf("Wrote %zd bytes\n", total_written);
    
    objm_response_free(resp);
    return 0;
}

static int cmd_get(objm_connection_t *conn, const char *uri, const char *dest_path) {
    /* Send GET request */
    objm_request_t req = {
        .id = 0,
        .flags = 0,
        .mode = OBJM_MODE_FDPASS,
        .uri = (char *)uri,
        .uri_len = strlen(uri)
    };
    
    printf("GET %s -> %s\n", uri, dest_path);
    
    if (objm_client_send_request(conn, &req) < 0) {
        fprintf(stderr, "Failed to send GET request\n");
        return -1;
    }
    
    /* Receive response with FD */
    objm_response_t *resp = NULL;
    if (objm_client_recv_response(conn, &resp) < 0) {
        fprintf(stderr, "Failed to receive response\n");
        return -1;
    }
    
    if (resp->status != OBJM_STATUS_OK) {
        fprintf(stderr, "GET failed: %s\n",
                resp->error_msg ? resp->error_msg : "Unknown error");
        objm_response_free(resp);
        return -1;
    }
    
    /* We got an FD - read data from it */
    int src_fd = resp->fd;
    if (src_fd < 0) {
        fprintf(stderr, "No FD received\n");
        objm_response_free(resp);
        return -1;
    }
    
    size_t content_len = resp->content_len;
    printf("Content length: %zu bytes\n", content_len);
    
    /* Open destination file */
    int dest_fd = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dest_fd < 0) {
        perror("open dest file");
        close(src_fd);
        objm_response_free(resp);
        return -1;
    }
    
    /* Copy data */
    char buffer[BUFFER_SIZE];
    ssize_t total_read = 0;
    ssize_t bytes_read;
    
    while ((bytes_read = read(src_fd, buffer, sizeof(buffer))) > 0) {
        ssize_t bytes_written = write(dest_fd, buffer, bytes_read);
        if (bytes_written < 0) {
            perror("write");
            close(src_fd);
            close(dest_fd);
            objm_response_free(resp);
            return -1;
        }
        total_read += bytes_read;
    }
    
    close(src_fd);
    close(dest_fd);
    
    printf("Read %zd bytes\n", total_read);
    
    objm_response_free(resp);
    return 0;
}

static int cmd_delete(objm_connection_t *conn, const char *uri) {
    /* Send DELETE request (via special URI prefix) */
    char delete_uri[OBJM_MAX_URI_LEN];
    snprintf(delete_uri, sizeof(delete_uri), "/delete%s", uri);
    
    objm_request_t req = {
        .id = 0,
        .flags = 0,
        .mode = OBJM_MODE_FDPASS,
        .uri = delete_uri,
        .uri_len = strlen(delete_uri)
    };
    
    printf("DELETE %s\n", uri);
    
    if (objm_client_send_request(conn, &req) < 0) {
        fprintf(stderr, "Failed to send DELETE request\n");
        return -1;
    }
    
    /* Receive response */
    objm_response_t *resp = NULL;
    if (objm_client_recv_response(conn, &resp) < 0) {
        fprintf(stderr, "Failed to receive response\n");
        return -1;
    }
    
    if (resp->status != OBJM_STATUS_OK) {
        fprintf(stderr, "DELETE failed: %s\n",
                resp->error_msg ? resp->error_msg : "Unknown error");
        objm_response_free(resp);
        return -1;
    }
    
    printf("Deleted successfully\n");
    
    objm_response_free(resp);
    return 0;
}

static int cmd_list(objm_connection_t *conn) {
    /* Send LIST request */
    objm_request_t req = {
        .id = 0,
        .flags = 0,
        .mode = OBJM_MODE_FDPASS,
        .uri = "/list",
        .uri_len = 5
    };
    
    printf("LIST\n");
    
    if (objm_client_send_request(conn, &req) < 0) {
        fprintf(stderr, "Failed to send LIST request\n");
        return -1;
    }
    
    /* Receive response */
    objm_response_t *resp = NULL;
    if (objm_client_recv_response(conn, &resp) < 0) {
        fprintf(stderr, "Failed to receive response\n");
        return -1;
    }
    
    if (resp->status != OBJM_STATUS_OK) {
        fprintf(stderr, "LIST failed: %s\n",
                resp->error_msg ? resp->error_msg : "Unknown error");
        objm_response_free(resp);
        return -1;
    }
    
    printf("Object count: %zu\n", resp->content_len);
    
    objm_response_free(resp);
    return 0;
}

/* ============================================================================
 * Main
 * ============================================================================ */

static void print_usage(const char *prog) {
    printf("Usage: %s [socket_path] <command> [args]\n", prog);
    printf("\nCommands:\n");
    printf("  put <uri> <file>     Upload file to URI\n");
    printf("  get <uri> <file>     Download URI to file\n");
    printf("  delete <uri>         Delete object at URI\n");
    printf("  list                 List all objects\n");
    printf("\nExamples:\n");
    printf("  %s put /data/test.txt myfile.txt\n", prog);
    printf("  %s get /data/test.txt output.txt\n", prog);
    printf("  %s delete /data/test.txt\n", prog);
    printf("  %s list\n", prog);
}

int main(int argc, char **argv) {
    const char *socket_path = DEFAULT_SOCKET_PATH;
    int arg_offset = 1;
    
    /* Check if first arg is socket path */
    if (argc > 1 && argv[1][0] == '/') {
        socket_path = argv[1];
        arg_offset = 2;
    }
    
    if (argc < arg_offset + 1) {
        print_usage(argv[0]);
        return 1;
    }
    
    const char *command = argv[arg_offset];
    
    /* Connect to server */
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }
    
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
    
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        fprintf(stderr, "Failed to connect to %s\n", socket_path);
        fprintf(stderr, "Is the server running?\n");
        close(sock);
        return 1;
    }
    
    /* Create protocol connection */
    objm_connection_t *conn = objm_client_create(sock, OBJM_PROTO_V1);
    if (!conn) {
        fprintf(stderr, "Failed to create client connection\n");
        close(sock);
        return 1;
    }
    
    /* V1 protocol - no handshake needed */
    
    /* Execute command */
    int ret = 0;
    
    if (strcmp(command, "put") == 0) {
        if (argc < arg_offset + 3) {
            fprintf(stderr, "Usage: put <uri> <file>\n");
            ret = 1;
        } else {
            ret = cmd_put(conn, argv[arg_offset + 1], argv[arg_offset + 2]);
        }
    } else if (strcmp(command, "get") == 0) {
        if (argc < arg_offset + 3) {
            fprintf(stderr, "Usage: get <uri> <file>\n");
            ret = 1;
        } else {
            ret = cmd_get(conn, argv[arg_offset + 1], argv[arg_offset + 2]);
        }
    } else if (strcmp(command, "delete") == 0) {
        if (argc < arg_offset + 2) {
            fprintf(stderr, "Usage: delete <uri>\n");
            ret = 1;
        } else {
            ret = cmd_delete(conn, argv[arg_offset + 1]);
        }
    } else if (strcmp(command, "list") == 0) {
        ret = cmd_list(conn);
    } else {
        fprintf(stderr, "Unknown command: %s\n", command);
        print_usage(argv[0]);
        ret = 1;
    }
    
    /* Cleanup */
    objm_client_close(conn, OBJM_CLOSE_NORMAL);
    objm_client_destroy(conn);
    close(sock);
    
    return ret;
}
