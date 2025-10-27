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
    printf("  -s PATH    Socket path (default: %s)\n", OBJMAPPER_SOCK_PATH);
    printf("  -b DIR     Backing directory (required)\n");
    printf("  -c DIR     Cache directory (optional)\n");
    printf("  -l SIZE    Cache limit in bytes (default: 1GB)\n");
    printf("  -m NUM     Max connections (default: 10)\n");
    printf("  -h         Show this help\n");
}

int main(int argc, char *argv[])
{
    server_config_t config = {
        .socket_path = OBJMAPPER_SOCK_PATH,
        .backing_dir = NULL,
        .cache_dir = NULL,
        .cache_limit = 1024 * 1024 * 1024,  /* 1GB default */
        .max_connections = 10
    };
    
    int opt;
    while ((opt = getopt(argc, argv, "s:b:c:l:m:h")) != -1) {
        switch (opt) {
            case 's':
                config.socket_path = optarg;
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
    
    printf("Starting objmapper server...\n");
    
    if (objmapper_server_start(&config) < 0) {
        fprintf(stderr, "Failed to start server\n");
        return 1;
    }
    
    return 0;
}
