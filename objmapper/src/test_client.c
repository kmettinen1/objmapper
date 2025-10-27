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
    printf("  -t TYPE    Transport type: unix, tcp, udp (default: unix)\n");
    printf("  -s PATH    Socket path for Unix transport (default: %s)\n", OBJMAPPER_SOCK_PATH);
    printf("  -H HOST    Host for TCP/UDP transport (default: localhost)\n");
    printf("  -p PORT    Port for TCP/UDP transport (default: %d/%d)\n",
           OBJMAPPER_TCP_PORT, OBJMAPPER_UDP_PORT);
    printf("  -m MODE    Operation mode: 1=fdpass, 2=copy, 3=splice (default: 1)\n");
    printf("  -o FILE    Output file (default: stdout)\n");
    printf("  -h         Show this help\n");
    printf("\nTransport Types:\n");
    printf("  unix       Unix domain socket (supports FD passing)\n");
    printf("  tcp        TCP socket (copy mode only)\n");
    printf("  udp        UDP socket (copy mode only)\n");
    printf("\nNote: FD passing (mode 1) only works with Unix sockets.\n");
}

int main(int argc, char *argv[])
{
    client_config_t config = {
        .transport = OBJMAPPER_TRANSPORT_UNIX,
        .socket_path = OBJMAPPER_SOCK_PATH,
        .operation_mode = OP_FDPASS
    };
    
    const char *host = "localhost";
    uint16_t port = 0;
    const char *output_file = NULL;
    int opt;
    
    while ((opt = getopt(argc, argv, "t:s:H:p:m:o:h")) != -1) {
        switch (opt) {
            case 't':
                if (strcmp(optarg, "unix") == 0) {
                    config.transport = OBJMAPPER_TRANSPORT_UNIX;
                } else if (strcmp(optarg, "tcp") == 0) {
                    config.transport = OBJMAPPER_TRANSPORT_TCP;
                } else if (strcmp(optarg, "udp") == 0) {
                    config.transport = OBJMAPPER_TRANSPORT_UDP;
                } else {
                    fprintf(stderr, "Invalid transport type: %s\n", optarg);
                    return 1;
                }
                break;
            case 's':
                config.socket_path = optarg;
                break;
            case 'H':
                host = optarg;
                break;
            case 'p':
                port = atoi(optarg);
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
    
    /* Set network config if TCP/UDP */
    if (config.transport != OBJMAPPER_TRANSPORT_UNIX) {
        config.net.host = host;
        config.net.port = port;
        
        /* Force copy mode for non-Unix transports */
        if (config.operation_mode == OP_FDPASS) {
            fprintf(stderr, "Warning: FD passing not supported on TCP/UDP, using copy mode\n");
            config.operation_mode = OP_COPY;
        }
    }
    
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
        ssize_t written = 0;
        while (written < n) {
            ssize_t w = write(out_fd, buffer + written, n - written);
            if (w <= 0) {
                perror("write output");
                if (output_file) close(out_fd);
                close(fd);
                objmapper_client_close(sock);
                return 1;
            }
            written += w;
        }
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
