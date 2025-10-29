/**
 * @file benchmark.c
 * @brief FD passing performance benchmark with concurrency testing
 * 
 * Tests:
 * - Single-threaded throughput
 * - Multi-threaded concurrency (1, 4, 16, 64 clients)
 * - Long-lived connections vs reconnect overhead
 * - Mixed read/write workloads
 * - Object size variations (1KB - 10MB)
 * 
 * Limits:
 * - Memory: 1GB backend
 * - Disk: 20GB backend
 * - Max object size: 10MB
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <fcntl.h>
#include <errno.h>
#include <stdatomic.h>

#include "lib/protocol/protocol.h"

/* ============================================================================
 * Configuration
 * ============================================================================ */

#define SOCKET_PATH "/tmp/objmapper.sock"
#define MAX_OBJECT_SIZE (10 * 1024 * 1024)  /* 10MB */
#define BENCHMARK_DURATION_SEC 5  /* Reduced from 10 for faster testing */

/* Test parameters */
static const int CONCURRENCY_LEVELS[] = {1, 4, 16};  /* Reduced from 64 */
static const int NUM_CONCURRENCY_LEVELS = 3;
static const size_t OBJECT_SIZES[] = {1024, 4096, 65536, 1024*1024};  /* Removed 10MB */
static const int NUM_OBJECT_SIZES = 4;

/* Global statistics */
typedef struct {
    atomic_uint_least64_t operations;
    atomic_uint_least64_t bytes_transferred;
    atomic_uint_least64_t errors;
    atomic_uint_least64_t put_attempts;
    atomic_uint_least64_t put_success;
    atomic_uint_least64_t get_attempts;
    atomic_uint_least64_t get_success;
    atomic_uint_least64_t send_errors;
    atomic_uint_least64_t recv_errors;
    atomic_uint_least64_t status_errors;
    atomic_uint_least64_t fd_errors;
    atomic_uint_least64_t io_errors;
    atomic_uint_least64_t connects;
    atomic_uint_least64_t disconnects;
    atomic_uint_least64_t total_latency_us;
} stats_t;

static stats_t g_stats;

/* Test control */
static atomic_bool g_stop_test = false;

/* ============================================================================
 * Utilities
 * ============================================================================ */

static uint64_t get_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

static void reset_stats(void) {
    atomic_store(&g_stats.operations, 0);
    atomic_store(&g_stats.bytes_transferred, 0);
    atomic_store(&g_stats.errors, 0);
    atomic_store(&g_stats.put_attempts, 0);
    atomic_store(&g_stats.put_success, 0);
    atomic_store(&g_stats.get_attempts, 0);
    atomic_store(&g_stats.get_success, 0);
    atomic_store(&g_stats.send_errors, 0);
    atomic_store(&g_stats.recv_errors, 0);
    atomic_store(&g_stats.status_errors, 0);
    atomic_store(&g_stats.fd_errors, 0);
    atomic_store(&g_stats.io_errors, 0);
    atomic_store(&g_stats.connects, 0);
    atomic_store(&g_stats.disconnects, 0);
    atomic_store(&g_stats.total_latency_us, 0);
}

static void print_stats(const char *test_name, uint64_t duration_us) {
    uint64_t ops = atomic_load(&g_stats.operations);
    uint64_t bytes = atomic_load(&g_stats.bytes_transferred);
    uint64_t errors = atomic_load(&g_stats.errors);
    uint64_t connects = atomic_load(&g_stats.connects);
    uint64_t put_attempts = atomic_load(&g_stats.put_attempts);
    uint64_t put_success = atomic_load(&g_stats.put_success);
    uint64_t get_attempts = atomic_load(&g_stats.get_attempts);
    uint64_t get_success = atomic_load(&g_stats.get_success);
    uint64_t send_errors = atomic_load(&g_stats.send_errors);
    uint64_t recv_errors = atomic_load(&g_stats.recv_errors);
    uint64_t status_errors = atomic_load(&g_stats.status_errors);
    uint64_t fd_errors = atomic_load(&g_stats.fd_errors);
    uint64_t io_errors = atomic_load(&g_stats.io_errors);
    uint64_t total_latency = atomic_load(&g_stats.total_latency_us);
    
    double duration_sec = duration_us / 1000000.0;
    double ops_per_sec = ops / duration_sec;
    double mb_per_sec = (bytes / (1024.0 * 1024.0)) / duration_sec;
    double avg_latency_ms = ops > 0 ? (total_latency / (double)ops) / 1000.0 : 0;
    double put_success_rate = put_attempts > 0 ? (put_success * 100.0 / put_attempts) : 0;
    double get_success_rate = get_attempts > 0 ? (get_success * 100.0 / get_attempts) : 0;
    
    printf("%-40s: %8.1f ops/sec, %8.2f MB/s, %6.2fms avg\n",
           test_name, ops_per_sec, mb_per_sec, avg_latency_ms);
    printf("  PUT: %lu/%lu (%.1f%%), GET: %lu/%lu (%.1f%%)\n",
           put_success, put_attempts, put_success_rate,
           get_success, get_attempts, get_success_rate);
    printf("  Errors: send=%lu, recv=%lu, status=%lu, fd=%lu, io=%lu\n",
           send_errors, recv_errors, status_errors, fd_errors, io_errors);
}

/* ============================================================================
 * Connection Management
 * ============================================================================ */

static int connect_to_server(void) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    }
    
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    
    atomic_fetch_add(&g_stats.connects, 1);
    return sock;
}

static void disconnect_from_server(int sock, objm_connection_t *conn) {
    if (conn) {
        objm_client_close(conn, OBJM_CLOSE_NORMAL);
        objm_client_destroy(conn);
    }
    if (sock >= 0) {
        close(sock);
        atomic_fetch_add(&g_stats.disconnects, 1);
    }
}

/* ============================================================================
 * Benchmark Operations
 * ============================================================================ */

static int do_put_operation(objm_connection_t *conn, const char *uri, 
                           size_t size, const char *data) {
    uint64_t start_time = get_time_us();
    atomic_fetch_add(&g_stats.put_attempts, 1);
    
    objm_request_t req = {
        .id = 0,
        .flags = 0,
        .mode = OBJM_MODE_FDPASS,
        .uri = (char *)uri,
        .uri_len = strlen(uri)
    };
    
    if (objm_client_send_request(conn, &req) < 0) {
        atomic_fetch_add(&g_stats.send_errors, 1);
        return -1;
    }
    
    objm_response_t *resp = NULL;
    if (objm_client_recv_response(conn, &resp) < 0) {
        atomic_fetch_add(&g_stats.recv_errors, 1);
        return -1;
    }
    
    if (resp->status != OBJM_STATUS_OK) {
        atomic_fetch_add(&g_stats.status_errors, 1);
        objm_response_free(resp);
        return -1;
    }
    
    if (resp->fd < 0) {
        static int printed_badfd = 0;
        if (!printed_badfd) {
            fprintf(stderr, "PUT: Invalid FD from server: fd=%d\n", resp->fd);
            printed_badfd = 1;
        }
        atomic_fetch_add(&g_stats.fd_errors, 1);
        objm_response_free(resp);
        return -1;
    }
    
    int fd = resp->fd;
    resp->fd = -1;  /* Prevent objm_response_free from closing our FD */
    objm_response_free(resp);
    
    /* Verify FD is valid before using */
    if (fcntl(fd, F_GETFD) < 0) {
        static int printed_fcntl = 0;
        if (!printed_fcntl) {
            fprintf(stderr, "PUT: FD invalid after recv: fd=%d, errno=%d (%s)\n",
                    fd, errno, strerror(errno));
            printed_fcntl = 1;
        }
        atomic_fetch_add(&g_stats.fd_errors, 1);
        close(fd);
        return -1;
    }
    
    /* Write data */
    size_t total_written = 0;
    while (total_written < size) {
        ssize_t n = write(fd, data + total_written, size - total_written);
        if (n < 0) {
            static int printed_errno = 0;
            if (!printed_errno) {
                fprintf(stderr, "write() failed: %s (errno=%d, fd=%d)\n", 
                        strerror(errno), errno, fd);
                printed_errno = 1;
            }
            atomic_fetch_add(&g_stats.io_errors, 1);
            close(fd);
            return -1;
        }
        if (n == 0) {
            static int printed_zero = 0;
            if (!printed_zero) {
                fprintf(stderr, "write() returned 0 (fd=%d)\n", fd);
                printed_zero = 1;
            }
            atomic_fetch_add(&g_stats.io_errors, 1);
            close(fd);
            return -1;
        }
        total_written += n;
    }
    
    close(fd);
    
    uint64_t latency = get_time_us() - start_time;
    atomic_fetch_add(&g_stats.operations, 1);
    atomic_fetch_add(&g_stats.put_success, 1);
    atomic_fetch_add(&g_stats.bytes_transferred, size);
    atomic_fetch_add(&g_stats.total_latency_us, latency);
    
    return 0;
}

static int do_get_operation(objm_connection_t *conn, const char *uri, 
                           char *buffer, size_t buffer_size) {
    uint64_t start_time = get_time_us();
    atomic_fetch_add(&g_stats.get_attempts, 1);
    
    objm_request_t req = {
        .id = 0,
        .flags = 0,
        .mode = OBJM_MODE_FDPASS,
        .uri = (char *)uri,
        .uri_len = strlen(uri)
    };
    
    if (objm_client_send_request(conn, &req) < 0) {
        atomic_fetch_add(&g_stats.send_errors, 1);
        return -1;
    }
    
    objm_response_t *resp = NULL;
    if (objm_client_recv_response(conn, &resp) < 0) {
        atomic_fetch_add(&g_stats.recv_errors, 1);
        return -1;
    }
    
    if (resp->status != OBJM_STATUS_OK) {
        atomic_fetch_add(&g_stats.status_errors, 1);
        objm_response_free(resp);
        return -1;
    }
    
    if (resp->fd < 0) {
        atomic_fetch_add(&g_stats.fd_errors, 1);
        objm_response_free(resp);
        return -1;
    }
    
    int fd = resp->fd;
    resp->fd = -1;  /* Prevent objm_response_free from closing our FD */
    objm_response_free(resp);
    
    /* Read data */
    size_t total_read = 0;
    while (total_read < buffer_size) {
        ssize_t n = read(fd, buffer + total_read, buffer_size - total_read);
        if (n < 0) {
            static int printed_read_errno = 0;
            if (!printed_read_errno) {
                fprintf(stderr, "read() failed: %s (errno=%d, fd=%d)\n",
                        strerror(errno), errno, fd);
                printed_read_errno = 1;
            }
            break;
        }
        if (n == 0) break;  /* EOF */
        total_read += n;
    }
    
    close(fd);
    
    if (total_read == 0) {
        atomic_fetch_add(&g_stats.io_errors, 1);
        return -1;
    }
    
    uint64_t latency = get_time_us() - start_time;
    atomic_fetch_add(&g_stats.operations, 1);
    atomic_fetch_add(&g_stats.get_success, 1);
    atomic_fetch_add(&g_stats.bytes_transferred, total_read);
    atomic_fetch_add(&g_stats.total_latency_us, latency);
    
    return 0;
}

/* ============================================================================
 * Benchmark Worker Threads
 * ============================================================================ */

typedef struct {
    int thread_id;
    size_t object_size;
    bool long_lived;  /* Keep connection open vs reconnect each time */
    int read_write_ratio;  /* Percentage reads (0-100) */
} worker_config_t;

static void *benchmark_worker(void *arg) {
    worker_config_t *config = (worker_config_t *)arg;
    
    char *data_buffer = malloc(config->object_size);
    if (!data_buffer) {
        atomic_fetch_add(&g_stats.errors, 1);
        return NULL;
    }
    
    /* Initialize with random data */
    for (size_t i = 0; i < config->object_size; i++) {
        data_buffer[i] = (char)(rand() % 256);
    }
    
    char uri[256];
    snprintf(uri, sizeof(uri), "/bench/%d/object.bin", config->thread_id);
    
    int sock = -1;
    objm_connection_t *conn = NULL;
    
    /* Long-lived connection setup */
    if (config->long_lived) {
        sock = connect_to_server();
        if (sock < 0) {
            free(data_buffer);
            atomic_fetch_add(&g_stats.errors, 1);
            return NULL;
        }
        conn = objm_client_create(sock, OBJM_PROTO_V1);
        if (!conn) {
            close(sock);
            free(data_buffer);
            atomic_fetch_add(&g_stats.errors, 1);
            return NULL;
        }
    }
    
    /* First PUT to create object */
    if (!config->long_lived) {
        sock = connect_to_server();
        if (sock < 0) {
            free(data_buffer);
            return NULL;
        }
        conn = objm_client_create(sock, OBJM_PROTO_V1);
        if (!conn) {
            close(sock);
            free(data_buffer);
            return NULL;
        }
    }
    
    if (do_put_operation(conn, uri, config->object_size, data_buffer) < 0) {
        atomic_fetch_add(&g_stats.errors, 1);
    }
    
    if (!config->long_lived) {
        disconnect_from_server(sock, conn);
    }
    
    /* Main benchmark loop */
    while (!atomic_load(&g_stop_test)) {
        bool do_read = (rand() % 100) < config->read_write_ratio;
        
        if (!config->long_lived) {
            sock = connect_to_server();
            if (sock < 0) {
                atomic_fetch_add(&g_stats.errors, 1);
                usleep(1000);
                continue;
            }
            conn = objm_client_create(sock, OBJM_PROTO_V1);
            if (!conn) {
                close(sock);
                atomic_fetch_add(&g_stats.errors, 1);
                usleep(1000);
                continue;
            }
        }
        
        if (do_read) {
            if (do_get_operation(conn, uri, data_buffer, config->object_size) < 0) {
                atomic_fetch_add(&g_stats.errors, 1);
            }
        } else {
            if (do_put_operation(conn, uri, config->object_size, data_buffer) < 0) {
                atomic_fetch_add(&g_stats.errors, 1);
            }
        }
        
        if (!config->long_lived) {
            disconnect_from_server(sock, conn);
        }
    }
    
    /* Cleanup */
    if (config->long_lived) {
        disconnect_from_server(sock, conn);
    }
    
    free(data_buffer);
    return NULL;
}

/* ============================================================================
 * Benchmark Tests
 * ============================================================================ */

static void run_benchmark(const char *test_name, int num_threads, 
                         size_t object_size, bool long_lived, 
                         int read_write_ratio) {
    pthread_t *threads = malloc(num_threads * sizeof(pthread_t));
    worker_config_t *configs = malloc(num_threads * sizeof(worker_config_t));
    
    if (!threads || !configs) {
        fprintf(stderr, "Failed to allocate memory for benchmark\n");
        free(threads);
        free(configs);
        return;
    }
    
    /* Reset stats and control */
    reset_stats();
    atomic_store(&g_stop_test, false);
    
    /* Start workers */
    for (int i = 0; i < num_threads; i++) {
        configs[i].thread_id = i;
        configs[i].object_size = object_size;
        configs[i].long_lived = long_lived;
        configs[i].read_write_ratio = read_write_ratio;
        
        if (pthread_create(&threads[i], NULL, benchmark_worker, &configs[i]) != 0) {
            fprintf(stderr, "Failed to create thread %d\n", i);
        }
    }
    
    /* Let benchmark run */
    uint64_t start = get_time_us();
    sleep(BENCHMARK_DURATION_SEC);
    atomic_store(&g_stop_test, true);
    uint64_t end = get_time_us();
    
    /* Wait for workers */
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }
    
    /* Print results */
    print_stats(test_name, end - start);
    
    free(threads);
    free(configs);
}

/* ============================================================================
 * Cleanup
 * ============================================================================ */

static void cleanup_benchmark_objects(void) {
    /* Connect and delete all benchmark objects */
    int sock = connect_to_server();
    if (sock < 0) return;
    
    objm_connection_t *conn = objm_client_create(sock, OBJM_PROTO_V1);
    if (!conn) {
        close(sock);
        return;
    }
    
    /* Try to delete common benchmark paths */
    for (int i = 0; i < 1000; i++) {
        char uri[256];
        snprintf(uri, sizeof(uri), "/delete/bench/%d/object.bin", i);
        
        objm_request_t req = {
            .id = 0,
            .flags = 0,
            .mode = OBJM_MODE_FDPASS,
            .uri = uri,
            .uri_len = strlen(uri)
        };
        
        objm_client_send_request(conn, &req);
        
        objm_response_t *resp = NULL;
        objm_client_recv_response(conn, &resp);
        if (resp) {
            objm_response_free(resp);
        }
    }
    
    disconnect_from_server(sock, conn);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    printf("=== FD Passing Performance Benchmark ===\n");
    printf("Configuration:\n");
    printf("  Duration per test: %d seconds\n", BENCHMARK_DURATION_SEC);
    printf("  Max object size: %d MB\n", MAX_OBJECT_SIZE / (1024*1024));
    printf("\n");
    
    srand(time(NULL));
    
    /* Test 1: Throughput by object size (single thread, long-lived) */
    printf("Test 1: Single-threaded throughput (long-lived connection)\n");
    printf("%-40s  %10s  %10s  %8s  %10s\n", 
           "Test", "Ops/sec", "MB/sec", "Errors", "Connects");
    printf("--------------------------------------------------------------------------------\n");
    
    for (int i = 0; i < NUM_OBJECT_SIZES; i++) {
        char test_name[256];
        size_t size_kb = OBJECT_SIZES[i] / 1024;
        if (size_kb < 1024) {
            snprintf(test_name, sizeof(test_name), "  %zuKB objects (50%% read)", size_kb);
        } else {
            snprintf(test_name, sizeof(test_name), "  %zuMB objects (50%% read)", 
                    size_kb / 1024);
        }
        
        run_benchmark(test_name, 1, OBJECT_SIZES[i], true, 50);
    }
    
    printf("\n");
    
    /* Test 2: Concurrency scaling (4KB objects, long-lived) */
    printf("Test 2: Concurrency scaling (4KB objects, long-lived)\n");
    printf("%-40s  %10s  %10s  %8s  %10s\n", 
           "Test", "Ops/sec", "MB/sec", "Errors", "Connects");
    printf("--------------------------------------------------------------------------------\n");
    
    for (int i = 0; i < NUM_CONCURRENCY_LEVELS; i++) {
        char test_name[256];
        snprintf(test_name, sizeof(test_name), "  %d threads (50%% read)", 
                CONCURRENCY_LEVELS[i]);
        
        run_benchmark(test_name, CONCURRENCY_LEVELS[i], 4096, true, 50);
    }
    
    printf("\n");
    
    /* Test 3: Long-lived vs reconnect (4KB, 16 threads) */
    printf("Test 3: Connection model comparison (4KB, 16 threads)\n");
    printf("%-40s  %10s  %10s  %8s  %10s\n", 
           "Test", "Ops/sec", "MB/sec", "Errors", "Connects");
    printf("--------------------------------------------------------------------------------\n");
    
    run_benchmark("  Long-lived connections (50% read)", 16, 4096, true, 50);
    run_benchmark("  Reconnect each op (50% read)", 16, 4096, false, 50);
    
    printf("\n");
    
    /* Test 4: Read/write ratio (1MB, 16 threads, long-lived) */
    printf("Test 4: Read/write ratio (1MB objects, 16 threads, long-lived)\n");
    printf("%-40s  %10s  %10s  %8s  %10s\n", 
           "Test", "Ops/sec", "MB/sec", "Errors", "Connects");
    printf("--------------------------------------------------------------------------------\n");
    
    run_benchmark("  100% reads", 16, 1024*1024, true, 100);
    run_benchmark("  75% reads", 16, 1024*1024, true, 75);
    run_benchmark("  50% reads", 16, 1024*1024, true, 50);
    run_benchmark("  25% reads", 16, 1024*1024, true, 25);
    run_benchmark("  100% writes", 16, 1024*1024, true, 0);
    
    printf("\n");
    
    /* Cleanup */
    printf("Cleaning up benchmark objects...\n");
    cleanup_benchmark_objects();
    
    printf("\nBenchmark complete!\n");
    return 0;
}
