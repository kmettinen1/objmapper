/**
 * @file main.c
 * @brief Objmapper server main entry point
 */

#include "objmapper.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>

static void print_usage(const char *prog)
{
    printf("Usage: %s [OPTIONS]\n", prog);
    printf("\nOptions:\n");
    printf("  -t TYPE    Transport type: unix, tcp, udp (default: unix)\n");
    printf("  -s PATH    Socket path for Unix transport (default: %s)\n", OBJMAPPER_SOCK_PATH);
    printf("  -H HOST    Host for TCP/UDP transport (default: *)\n");
    printf("  -p PORT    Port for TCP/UDP transport (default: %d/%d)\n", 
           OBJMAPPER_TCP_PORT, OBJMAPPER_UDP_PORT);
    printf("  -b DIR     Backing directory (required)\n");
    printf("  -c DIR     Cache directory (optional)\n");
    printf("  -l SIZE    Cache limit in bytes (default: 1GB)\n");
    printf("  -m NUM     Max connections (default: 10)\n");
    printf("  -h         Show this help\n");
    printf("\nTransport Types:\n");
    printf("  unix       Unix domain socket (primary, supports FD passing)\n");
    printf("  tcp        TCP socket (stream-based, no FD passing)\n");
    printf("  udp        UDP socket (datagram-based, no FD passing)\n");
}

int main(int argc, char *argv[])
{
    server_config_t config = {
        .transport = OBJMAPPER_TRANSPORT_UNIX,
        .socket_path = OBJMAPPER_SOCK_PATH,
        .backing_dir = NULL,
        .cache_dir = NULL,
        .cache_limit = 1024 * 1024 * 1024,  /* 1GB default */
        .max_connections = 10
    };
    
    const char *host = NULL;
    uint16_t port = 0;
    
    int opt;
    while ((opt = getopt(argc, argv, "t:s:H:p:b:c:l:m:h")) != -1) {
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
            case 'b':
                config.backing_dir = optarg;
                break;
            case 'c':
                config.cache_dir = optarg;
                break;
            case 'l':
                config.cache_limit = strtoull(optarg, NULL, 10);
                break;
            case 'm':
                config.max_connections = atoi(optarg);
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }
    
    if (!config.backing_dir) {
        fprintf(stderr, "Error: Backing directory (-b) is required\n\n");
        print_usage(argv[0]);
        return 1;
    }
    
    /* Set network config if TCP/UDP */
    if (config.transport != OBJMAPPER_TRANSPORT_UNIX) {
        config.net.host = host;
        config.net.port = port;
    }
    
    printf("Starting objmapper server...\n");
    
    if (objmapper_server_start(&config) < 0) {
        fprintf(stderr, "Failed to start server\n");
        return 1;
    }
    
    return 0;
}
