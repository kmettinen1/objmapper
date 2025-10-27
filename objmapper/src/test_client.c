/**
 * @file test_client.c
 * @brief Simple test client for objmapper
 */

#include "objmapper.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>

static void print_usage(const char *prog)
{
    printf("Usage: %s [OPTIONS] URI\n", prog);
    printf("\nOptions:\n");
    printf("  -s PATH    Socket path (default: %s)\n", OBJMAPPER_SOCK_PATH);
    printf("  -m MODE    Operation mode: 1=fdpass, 2=copy, 3=splice (default: 1)\n");
    printf("  -o FILE    Output file (default: stdout)\n");
    printf("  -h         Show this help\n");
}

int main(int argc, char *argv[])
{
    client_config_t config = {
        .socket_path = OBJMAPPER_SOCK_PATH,
        .operation_mode = OP_FDPASS
    };
    
    const char *output_file = NULL;
    int opt;
    
    while ((opt = getopt(argc, argv, "s:m:o:h")) != -1) {
        switch (opt) {
            case 's':
                config.socket_path = optarg;
                break;
            case 'm':
                config.operation_mode = optarg[0];
                break;
            case 'o':
                output_file = optarg;
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }
    
    if (optind >= argc) {
        fprintf(stderr, "Error: URI required\n\n");
        print_usage(argv[0]);
        return 1;
    }
    
    const char *uri = argv[optind];
    
    /* Connect to server */
    int sock = objmapper_client_connect(&config);
    if (sock < 0) {
        fprintf(stderr, "Failed to connect to server\n");
        return 1;
    }
    
    printf("Connected to objmapper server\n");
    printf("Requesting: %s (mode=%c)\n", uri, config.operation_mode);
    
    /* Request object */
    int fd = objmapper_client_request(sock, uri, config.operation_mode);
    if (fd < 0) {
        fprintf(stderr, "Failed to get object\n");
        objmapper_client_close(sock);
        return 1;
    }
    
    printf("Received file descriptor: %d\n", fd);
    
    /* Output data */
    int out_fd = STDOUT_FILENO;
    if (output_file) {
        out_fd = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out_fd < 0) {
            perror("open output");
            close(fd);
            objmapper_client_close(sock);
            return 1;
        }
    }
    
    /* Copy data */
    char buffer[8192];
    ssize_t total = 0;
    ssize_t n;
    
    while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
        write(out_fd, buffer, n);
        total += n;
    }
    
    printf("Received %zd bytes\n", total);
    
    if (output_file) {
        close(out_fd);
    }
    close(fd);
    
    objmapper_client_close(sock);
    
    return 0;
}
