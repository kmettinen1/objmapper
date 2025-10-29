/**
 * @file server.c
 * @brief objmapper server with Unix socket and FD passing
 * 
 * Full implementation of objmapper server using:
 * - Unix domain sockets for local IPC
 * - FD passing for zero-copy object access
 * - Backend manager for multi-tier storage
 * - objm protocol V1 (simple ordered requests)
 */

#include "lib/protocol/protocol.h"
#include "lib/backend/backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <pthread.h>

/* ============================================================================
 * Configuration
 * ============================================================================ */

#define DEFAULT_SOCKET_PATH "/tmp/objmapper.sock"
#define LISTEN_BACKLOG 128
#define MAX_CONCURRENT_CLIENTS 64

/* Backend configuration */
/* Can be overridden for benchmarking with smaller limits */
#ifndef MEMORY_CACHE_SIZE
#define MEMORY_CACHE_SIZE (4ULL * 1024 * 1024 * 1024)   /* 4GB */
#endif
#ifndef PERSISTENT_SIZE
#define PERSISTENT_SIZE   (100ULL * 1024 * 1024 * 1024) /* 100GB */
#endif
#define CACHE_CHECK_INTERVAL_US (1000000)  /* 1 second */
#define CACHE_HOTNESS_THRESHOLD 0.7

/* ============================================================================
 * Global State
 * ============================================================================ */

static backend_manager_t *g_backend_mgr = NULL;
static volatile sig_atomic_t g_running = 1;
static int g_memory_backend_id = -1;
static int g_persistent_backend_id = -1;

static struct {
    atomic_size_t requests_total;
    atomic_size_t gets;
    atomic_size_t puts;
    atomic_size_t deletes;
    atomic_size_t errors;
    atomic_size_t active_connections;
} g_stats = {0};

/* ============================================================================
 * Request Handlers
 * ============================================================================ */

/**
 * Handle GET request
 * 
 * For FD pass mode (mode '1'):
 * - Lookup object in backend
 * - Send FD via SCM_RIGHTS
 * - Client can read directly from FD (zero-copy)
 */
static int handle_get(objm_connection_t *conn, const objm_request_t *req) {
    fd_ref_t ref;
    
    /* Lookup object */
    if (backend_get_object(g_backend_mgr, req->uri, &ref) < 0) {
        objm_server_send_error(conn, req->id, OBJM_STATUS_NOT_FOUND,
                              "Object not found");
        return -1;
    }
    
    /* For FD pass mode, send the file descriptor */
    if (req->mode == OBJM_MODE_FDPASS) {
        /* Build response - for FD pass, content_len should be 0 
         * (client gets size from fstat on the FD) */
        objm_response_t resp = {
            .request_id = req->id,
            .status = OBJM_STATUS_OK,
            .fd = ref.fd,
            .content_len = 0,  /* FD pass doesn't use content_len */
            .metadata = NULL,
            .metadata_len = 0,
            .error_msg = NULL
        };
        
        /* Send response with FD */
        int ret = objm_server_send_response(conn, &resp);
        
        /* Release our reference (client now owns the FD) */
        fd_ref_release(&ref);
        
        if (ret < 0) {
            return -1;
        }
        
        atomic_fetch_add(&g_stats.gets, 1);
        return 0;
    } else {
        /* Copy mode not implemented yet */
        fd_ref_release(&ref);
        objm_server_send_error(conn, req->id, OBJM_STATUS_UNSUPPORTED_OP,
                              "Only FD pass mode supported for GET");
        return -1;
    }
}

/**
 * Handle PUT request
 * 
 * For FD pass mode:
 * - Create object in backend
 * - Send our FD to client via SCM_RIGHTS
 * - Client writes directly to FD
 * - Client closes FD when done
 */
static int handle_put(objm_connection_t *conn, const objm_request_t *req) {
    /* Determine if ephemeral or persistent */
    bool ephemeral = (req->flags & OBJM_REQ_PRIORITY) ? true : false;
    
    object_create_req_t create_req = {
        .uri = req->uri,
        .backend_id = -1,  /* Auto-select */
        .ephemeral = ephemeral,
        .size_hint = 0,
        .flags = 0
    };
    
    fd_ref_t ref;
    
    /* Check if object exists - delete old version */
    if (backend_get_object(g_backend_mgr, req->uri, &ref) == 0) {
        fd_ref_release(&ref);
        backend_delete_object(g_backend_mgr, req->uri);
    }
    
    /* Create new object */
    if (backend_create_object(g_backend_mgr, &create_req, &ref) < 0) {
        objm_server_send_error(conn, req->id, OBJM_STATUS_STORAGE_ERROR,
                              "Failed to create object");
        return -1;
    }
    
    /* For FD pass mode, send the FD to client for writing */
    if (req->mode == OBJM_MODE_FDPASS) {
        objm_response_t resp = {
            .request_id = req->id,
            .status = OBJM_STATUS_OK,
            .fd = ref.fd,
            .content_len = 0,
            .metadata = NULL,
            .metadata_len = 0,
            .error_msg = NULL
        };
        
        int ret = objm_server_send_response(conn, &resp);
        
        /* Release our reference (client now owns the FD for writing) */
        fd_ref_release(&ref);
        
        if (ret < 0) {
            /* Client will close FD, object remains but empty */
            return -1;
        }
        
        atomic_fetch_add(&g_stats.puts, 1);
        return 0;
    } else {
        fd_ref_release(&ref);
        objm_server_send_error(conn, req->id, OBJM_STATUS_UNSUPPORTED_OP,
                              "Only FD pass mode supported for PUT");
        return -1;
    }
}

/**
 * Handle DELETE request
 */
static int handle_delete(objm_connection_t *conn, const objm_request_t *req) {
    int ret = backend_delete_object(g_backend_mgr, req->uri);
    
    if (ret == 0) {
        objm_response_t resp = {
            .request_id = req->id,
            .status = OBJM_STATUS_OK,
            .fd = -1,
            .content_len = 1,  /* Non-zero to indicate no FD in response */
            .metadata = NULL,
            .metadata_len = 0,
            .error_msg = NULL
        };
        
        if (objm_server_send_response(conn, &resp) < 0) {
            return -1;
        }
        
        atomic_fetch_add(&g_stats.deletes, 1);
        return 0;
    } else {
        objm_server_send_error(conn, req->id, OBJM_STATUS_NOT_FOUND,
                              "Object not found");
        return -1;
    }
}

/**
 * Handle LIST request (disabled - should be management API)
 * 
 * Note: LIST functionality should be implemented as a separate
 * management/admin interface, not as part of the main object
 * storage protocol. Use a dedicated management socket or REST API.
 */
static int handle_list(objm_connection_t *conn, const objm_request_t *req) {
    (void)req;  /* Unused */
    
    objm_server_send_error(conn, req->id, OBJM_STATUS_UNSUPPORTED_OP,
                          "LIST is disabled - use management API");
    return -1;
    
    /* Original implementation disabled:
    int backend_id = g_persistent_backend_id;
    
    if (req->uri && strncmp(req->uri, "/backend/", 9) == 0) {
        backend_id = atoi(req->uri + 9);
    }
    
    char **uris = NULL;
    size_t count = 0;
    
    if (backend_list_objects(g_backend_mgr, backend_id, &uris, &count) < 0) {
        objm_server_send_error(conn, req->id, OBJM_STATUS_INTERNAL_ERROR,
                              "Failed to list objects");
        return -1;
    }
    
    objm_response_t resp = {
        .request_id = req->id,
        .status = OBJM_STATUS_OK,
        .fd = -1,
        .content_len = count,
        .metadata = NULL,
        .metadata_len = 0,
        .error_msg = NULL
    };
    
    int ret = objm_server_send_response(conn, &resp);
    
    for (size_t i = 0; i < count; i++) {
        free(uris[i]);
    }
    free(uris);
    
    return ret;
    */
}

/* ============================================================================
 * Client Connection Handler
 * ============================================================================ */

typedef struct {
    int client_fd;
    struct sockaddr_un client_addr;
} client_info_t;

static void *client_thread(void *arg) {
    client_info_t *info = (client_info_t *)arg;
    int client_fd = info->client_fd;
    free(info);
    
    atomic_fetch_add(&g_stats.active_connections, 1);
    
    /* Create protocol connection */
    objm_connection_t *conn = objm_server_create(client_fd);
    if (!conn) {
        fprintf(stderr, "Failed to create server connection\n");
        close(client_fd);
        atomic_fetch_sub(&g_stats.active_connections, 1);
        return NULL;
    }
    
    /* Perform handshake - will auto-detect V1 or V2 */
    objm_hello_t hello = {
        .capabilities = 0,  /* V1 protocol */
        .max_pipeline = 1,
        .backend_parallelism = 2  /* Memory + persistent */
    };
    
    objm_params_t params;
    if (objm_server_handshake(conn, &hello, &params) < 0) {
        fprintf(stderr, "Handshake failed\n");
        objm_server_destroy(conn);
        close(client_fd);
        atomic_fetch_sub(&g_stats.active_connections, 1);
        return NULL;
    }
    
    printf("Client connected (V%d)\n", params.version);
    
    /* Request handling loop */
    while (g_running) {
        objm_request_t *req = NULL;
        int ret = objm_server_recv_request(conn, &req);
        
        if (ret == 1) {
            /* Clean connection close */
            printf("Client disconnected gracefully\n");
            break;
        }
        
        if (ret < 0) {
            /* For V1, socket close is normal - don't treat as error */
            if (params.version == OBJM_PROTO_V1) {
                printf("Client disconnected\n");
            } else {
                fprintf(stderr, "Error receiving request\n");
                atomic_fetch_add(&g_stats.errors, 1);
            }
            break;
        }
        
        atomic_fetch_add(&g_stats.requests_total, 1);
        
        /* Determine operation from URI pattern
         * - GET: URI starts with / and exists
         * - PUT: URI starts with / and doesn't exist, or mode is FDPASS for write
         * - DELETE: URI starts with /delete/
         * - LIST: URI is /list or /backend/N
         */
        
        if (strncmp(req->uri, "/delete/", 8) == 0) {
            /* DELETE */
            char *real_uri = req->uri + 7;  /* Skip "/delete" */
            char uri_copy[OBJM_MAX_URI_LEN];
            strncpy(uri_copy, real_uri, sizeof(uri_copy) - 1);
            uri_copy[sizeof(uri_copy) - 1] = '\0';
            
            free(req->uri);
            req->uri = strdup(uri_copy);
            
            if (handle_delete(conn, req) < 0) {
                atomic_fetch_add(&g_stats.errors, 1);
            }
        } else if (strcmp(req->uri, "/list") == 0 || 
                   strncmp(req->uri, "/backend/", 9) == 0) {
            /* LIST */
            if (handle_list(conn, req) < 0) {
                atomic_fetch_add(&g_stats.errors, 1);
            }
        } else {
            /* GET or PUT - check if object exists */
            fd_ref_t ref;
            bool exists = (backend_get_object(g_backend_mgr, req->uri, &ref) == 0);
            if (exists) {
                fd_ref_release(&ref);
            }
            
            /* For FD pass mode:
             * - If exists: GET (read)
             * - If not exists: PUT (create + write)
             * 
             * This is a simplification. Real implementation would use:
             * - Separate GET/PUT operations
             * - Or flags in metadata to distinguish
             */
            
            if (exists) {
                /* GET */
                if (handle_get(conn, req) < 0) {
                    atomic_fetch_add(&g_stats.errors, 1);
                }
            } else {
                /* PUT */
                if (handle_put(conn, req) < 0) {
                    atomic_fetch_add(&g_stats.errors, 1);
                }
            }
        }
        
        objm_request_free(req);
    }
    
    objm_server_destroy(conn);
    close(client_fd);
    atomic_fetch_sub(&g_stats.active_connections, 1);
    
    printf("Client connection closed\n");
    return NULL;
}

/* ============================================================================
 * Backend Initialization
 * ============================================================================ */

static int init_backends(const char *memory_path, const char *persistent_path) {
    /* Create backend manager */
    g_backend_mgr = backend_manager_create(8192, 2000);
    if (!g_backend_mgr) {
        fprintf(stderr, "Failed to create backend manager\n");
        return -1;
    }
    
    printf("Backend manager created (8192 buckets, 2000 max FDs)\n");
    
    /* Register memory backend */
    mkdir(memory_path, 0755);
    
    g_memory_backend_id = backend_manager_register(g_backend_mgr,
        BACKEND_TYPE_MEMORY,
        memory_path,
        "Memory Cache",
        MEMORY_CACHE_SIZE,
        BACKEND_FLAG_EPHEMERAL_ONLY | BACKEND_FLAG_ENABLED |
        BACKEND_FLAG_MIGRATION_SRC | BACKEND_FLAG_MIGRATION_DST);
    
    if (g_memory_backend_id < 0) {
        fprintf(stderr, "Failed to register memory backend\n");
        return -1;
    }
    
    printf("Registered memory backend (ID %d): %s, %.1f GB\n",
           g_memory_backend_id, memory_path,
           MEMORY_CACHE_SIZE / (1024.0 * 1024.0 * 1024.0));
    
    /* Register persistent backend */
    mkdir(persistent_path, 0755);
    
    g_persistent_backend_id = backend_manager_register(g_backend_mgr,
        BACKEND_TYPE_SSD,
        persistent_path,
        "Persistent SSD",
        PERSISTENT_SIZE,
        BACKEND_FLAG_PERSISTENT | BACKEND_FLAG_ENABLED |
        BACKEND_FLAG_MIGRATION_SRC | BACKEND_FLAG_MIGRATION_DST);
    
    if (g_persistent_backend_id < 0) {
        fprintf(stderr, "Failed to register persistent backend\n");
        return -1;
    }
    
    printf("Registered persistent backend (ID %d): %s, %.1f GB\n",
           g_persistent_backend_id, persistent_path,
           PERSISTENT_SIZE / (1024.0 * 1024.0 * 1024.0));
    
    /* Configure backend roles */
    backend_manager_set_default(g_backend_mgr, g_persistent_backend_id);
    backend_manager_set_ephemeral(g_backend_mgr, g_memory_backend_id);
    backend_manager_set_cache(g_backend_mgr, g_memory_backend_id);
    
    printf("Backend roles: default=%d, ephemeral=%d, cache=%d\n",
           g_persistent_backend_id, g_memory_backend_id, g_memory_backend_id);
    
    /* Scan backends to rebuild index from existing files */
    int memory_count = backend_manager_scan(g_backend_mgr, g_memory_backend_id);
    if (memory_count >= 0) {
        printf("Scanned memory backend: %d objects found\n", memory_count);
    }
    
    int persistent_count = backend_manager_scan(g_backend_mgr, g_persistent_backend_id);
    if (persistent_count >= 0) {
        printf("Scanned persistent backend: %d objects found\n", persistent_count);
    }
    
    /* Start automatic caching */
    if (backend_start_caching(g_backend_mgr, CACHE_CHECK_INTERVAL_US,
                             CACHE_HOTNESS_THRESHOLD) == 0) {
        printf("Automatic caching started (threshold=%.2f, interval=%.1fs)\n",
               CACHE_HOTNESS_THRESHOLD,
               CACHE_CHECK_INTERVAL_US / 1000000.0);
    }
    
    return 0;
}

static void cleanup_backends(void) {
    if (g_backend_mgr) {
        backend_stop_caching(g_backend_mgr);
        backend_manager_destroy(g_backend_mgr);
        g_backend_mgr = NULL;
    }
}

/* ============================================================================
 * Signal Handling
 * ============================================================================ */

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static void setup_signals(void) {
    struct sigaction sa = {0};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}

/* ============================================================================
 * Server Statistics
 * ============================================================================ */

static void print_stats(void) {
    printf("\n=== Server Statistics ===\n");
    printf("Total requests:      %zu\n", g_stats.requests_total);
    printf("  GET:               %zu\n", g_stats.gets);
    printf("  PUT:               %zu\n", g_stats.puts);
    printf("  DELETE:            %zu\n", g_stats.deletes);
    printf("  Errors:            %zu\n", g_stats.errors);
    printf("Active connections:  %zu\n", g_stats.active_connections);
    
    if (g_backend_mgr) {
        uint64_t capacity, used;
        size_t obj_count;
        double utilization;
        
        if (backend_get_status(g_backend_mgr, g_memory_backend_id,
                              &capacity, &used, &obj_count, &utilization) == 0) {
            printf("\nMemory backend:\n");
            printf("  Objects:           %zu\n", obj_count);
            printf("  Used:              %.2f MB / %.2f MB\n",
                   used / (1024.0 * 1024.0),
                   capacity / (1024.0 * 1024.0));
            printf("  Utilization:       %.1f%%\n", utilization * 100.0);
        }
        
        if (backend_get_status(g_backend_mgr, g_persistent_backend_id,
                              &capacity, &used, &obj_count, &utilization) == 0) {
            printf("\nPersistent backend:\n");
            printf("  Objects:           %zu\n", obj_count);
            printf("  Used:              %.2f MB / %.2f MB\n",
                   used / (1024.0 * 1024.0),
                   capacity / (1024.0 * 1024.0));
            printf("  Utilization:       %.1f%%\n", utilization * 100.0);
        }
    }
}

/* ============================================================================
 * Main Server Loop
 * ============================================================================ */

int main(int argc, char **argv) {
    const char *socket_path = DEFAULT_SOCKET_PATH;
    const char *memory_path = "/tmp/objmapper_memory";
    const char *persistent_path = "/tmp/objmapper_persistent";
    
    if (argc > 1) socket_path = argv[1];
    if (argc > 2) memory_path = argv[2];
    if (argc > 3) persistent_path = argv[3];
    
    printf("objmapper server starting\n");
    printf("Socket: %s\n", socket_path);
    
    setup_signals();
    
    /* Initialize backends */
    if (init_backends(memory_path, persistent_path) < 0) {
        return 1;
    }
    
    /* Create Unix socket */
    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        cleanup_backends();
        return 1;
    }
    
    /* Remove old socket file if exists */
    unlink(socket_path);
    
    /* Bind to socket */
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
    
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd);
        cleanup_backends();
        return 1;
    }
    
    /* Set socket permissions */
    chmod(socket_path, 0666);
    
    if (listen(listen_fd, LISTEN_BACKLOG) < 0) {
        perror("listen");
        close(listen_fd);
        unlink(socket_path);
        cleanup_backends();
        return 1;
    }
    
    printf("Listening on %s\n", socket_path);
    printf("Press Ctrl+C to stop\n\n");
    
    /* Accept loop */
    while (g_running) {
        struct sockaddr_un client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        
        /* Spawn thread for client */
        client_info_t *info = malloc(sizeof(*info));
        if (!info) {
            close(client_fd);
            continue;
        }
        
        info->client_fd = client_fd;
        info->client_addr = client_addr;
        
        pthread_t thread;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        
        if (pthread_create(&thread, &attr, client_thread, info) != 0) {
            perror("pthread_create");
            close(client_fd);
            free(info);
        }
        
        pthread_attr_destroy(&attr);
    }
    
    printf("\nShutting down...\n");
    
    /* Wait for active connections to finish */
    printf("Waiting for %zu active connections to close...\n",
           g_stats.active_connections);
    
    while (g_stats.active_connections > 0) {
        usleep(100000);  /* 100ms */
    }
    
    close(listen_fd);
    unlink(socket_path);
    cleanup_backends();
    
    print_stats();
    
    printf("\nServer stopped\n");
    return 0;
}
