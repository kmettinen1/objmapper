/**
 * @file demo_integration.c
 * @brief Demonstration of backend manager integration
 * 
 * Shows how to:
 * - Create and configure backend manager
 * - Register multiple backends (memory + persistent)
 * - Enable automatic caching
 * - Create, read, update, delete objects
 * - Query backend status
 */

#include "lib/backend/backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

#define MEMORY_CACHE_SIZE (1ULL * 1024 * 1024 * 1024)  /* 1GB */
#define PERSISTENT_SIZE   (10ULL * 1024 * 1024 * 1024) /* 10GB */

static void print_banner(const char *title) {
    printf("\n=== %s ===\n", title);
}

static void print_backend_status(backend_manager_t *mgr, int backend_id, const char *name) {
    uint64_t capacity, used;
    size_t obj_count;
    double utilization;
    
    if (backend_get_status(mgr, backend_id, &capacity, &used, &obj_count, &utilization) == 0) {
        printf("%s backend:\n", name);
        printf("  Capacity:  %.2f MB\n", capacity / (1024.0 * 1024.0));
        printf("  Used:      %.2f MB\n", used / (1024.0 * 1024.0));
        printf("  Objects:   %lu\n", obj_count);
        printf("  Util:      %.1f%%\n", utilization * 100.0);
    } else {
        printf("%s backend: ERROR\n", name);
    }
}

static void demo_create_and_read(backend_manager_t *mgr) {
    print_banner("Create and Read Objects");
    
    /* Create some test objects */
    const char *uris[] = {
        "/data/file1.txt",
        "/data/file2.txt",
        "/cache/temp1.txt"
    };
    
    const char *contents[] = {
        "This is file 1 content",
        "This is file 2 content",
        "This is temporary data"
    };
    
    for (size_t i = 0; i < 3; i++) {
        object_create_req_t req = {
            .uri = uris[i],
            .backend_id = -1,  /* Auto-select */
            .ephemeral = (i == 2),  /* Last one is ephemeral */
            .size_hint = strlen(contents[i]),
            .flags = 0
        };
        
        fd_ref_t ref;
        if (backend_create_object(mgr, &req, &ref) < 0) {
            printf("Failed to create %s\n", uris[i]);
            continue;
        }
        
        /* Write content */
        ssize_t written = write(ref.fd, contents[i], strlen(contents[i]));
        if (written < 0) {
            perror("write");
        }
        
        /* Update size */
        backend_update_size(mgr, uris[i], strlen(contents[i]));
        
        printf("Created %s (FD %d, ephemeral=%d)\n", uris[i], ref.fd, req.ephemeral);
        
        /* Release reference */
        fd_ref_release(&ref);
    }
    
    printf("\n");
    
    /* Read back objects */
    for (size_t i = 0; i < 3; i++) {
        fd_ref_t ref;
        if (backend_get_object(mgr, uris[i], &ref) < 0) {
            printf("Failed to get %s\n", uris[i]);
            continue;
        }
        
        char buffer[256] = {0};
        lseek(ref.fd, 0, SEEK_SET);
        ssize_t bytes_read = read(ref.fd, buffer, sizeof(buffer) - 1);
        
        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';
            printf("Read %s: \"%s\"\n", uris[i], buffer);
        }
        
        fd_ref_release(&ref);
    }
}

static void demo_hotness_tracking(backend_manager_t *mgr) {
    print_banner("Hotness Tracking");
    
    const char *uri = "/data/file1.txt";
    
    /* Access object multiple times to increase hotness */
    printf("Accessing %s multiple times...\n", uri);
    
    for (int i = 0; i < 10; i++) {
        fd_ref_t ref;
        if (backend_get_object(mgr, uri, &ref) == 0) {
            char buf[128];
            (void)read(ref.fd, buf, sizeof(buf));  /* Suppress unused result warning */
            fd_ref_release(&ref);
        }
    }
    
    /* Check hotness */
    object_metadata_t metadata;
    if (backend_get_metadata(mgr, uri, &metadata) == 0) {
        printf("\nObject metadata for %s:\n", uri);
        printf("  Backend ID:    %d\n", metadata.backend_id);
        printf("  Size:          %zu bytes\n", metadata.size_bytes);
        printf("  Hotness:       %.4f\n", metadata.hotness);
        printf("  Access count:  %lu\n", metadata.access_count);
        
        free(metadata.uri);
        free(metadata.fs_path);
    }
}

static void demo_caching(backend_manager_t *mgr) {
    print_banner("Automatic Caching");
    
    printf("Starting automatic cache promotion...\n");
    printf("Hot objects (hotness >= 0.5) will be cached to memory backend\n\n");
    
    /* Start caching with 1-second check interval */
    if (backend_start_caching(mgr, 1000000, 0.5) == 0) {
        printf("Caching thread started\n");
        
        /* Let it run for a few seconds */
        printf("Waiting 3 seconds for cache promotion...\n");
        sleep(3);
        
        printf("\nStopping caching thread...\n");
        backend_stop_caching(mgr);
        printf("Caching stopped\n");
    } else {
        printf("Failed to start caching\n");
    }
}

static void demo_delete(backend_manager_t *mgr) {
    print_banner("Delete Objects");
    
    const char *uris[] = {
        "/data/file1.txt",
        "/data/file2.txt",
        "/cache/temp1.txt"
    };
    
    for (size_t i = 0; i < 3; i++) {
        if (backend_delete_object(mgr, uris[i]) == 0) {
            printf("Deleted %s\n", uris[i]);
        } else {
            printf("Failed to delete %s\n", uris[i]);
        }
    }
}

int main(void) {
    printf("objmapper Backend Manager Integration Demo\n");
    printf("===========================================\n");
    
    /* Create backend manager */
    backend_manager_t *mgr = backend_manager_create(8192, 1000);
    if (!mgr) {
        fprintf(stderr, "Failed to create backend manager\n");
        return 1;
    }
    
    printf("Backend manager created\n");
    printf("  Index buckets: 8192\n");
    printf("  Max open FDs:  1000\n");
    
    /* Register memory backend */
    const char *memory_path = "/tmp/objmapper_memory";
    uint32_t memory_flags = BACKEND_FLAG_EPHEMERAL_ONLY | BACKEND_FLAG_ENABLED |
                            BACKEND_FLAG_MIGRATION_SRC | BACKEND_FLAG_MIGRATION_DST;
    
    mkdir(memory_path, 0755);
    
    int memory_id = backend_manager_register(mgr,
        BACKEND_TYPE_MEMORY,
        memory_path,
        "Memory Cache",
        MEMORY_CACHE_SIZE,
        memory_flags);
    
    if (memory_id < 0) {
        fprintf(stderr, "Failed to register memory backend\n");
        backend_manager_destroy(mgr);
        return 1;
    }
    
    printf("\nRegistered memory backend (ID %d)\n", memory_id);
    
    /* Register persistent backend */
    const char *persistent_path = "/tmp/objmapper_persistent";
    uint32_t persistent_flags = BACKEND_FLAG_PERSISTENT | BACKEND_FLAG_ENABLED |
                                BACKEND_FLAG_MIGRATION_SRC | BACKEND_FLAG_MIGRATION_DST;
    
    mkdir(persistent_path, 0755);
    
    int persistent_id = backend_manager_register(mgr,
        BACKEND_TYPE_SSD,
        persistent_path,
        "Persistent SSD",
        PERSISTENT_SIZE,
        persistent_flags);
    
    if (persistent_id < 0) {
        fprintf(stderr, "Failed to register persistent backend\n");
        backend_manager_destroy(mgr);
        return 1;
    }
    
    printf("Registered persistent backend (ID %d)\n", persistent_id);
    
    /* Configure backend roles */
    backend_manager_set_default(mgr, persistent_id);
    backend_manager_set_ephemeral(mgr, memory_id);
    backend_manager_set_cache(mgr, memory_id);
    
    printf("\nBackend roles configured:\n");
    printf("  Default:    Persistent SSD (%d)\n", persistent_id);
    printf("  Ephemeral:  Memory Cache (%d)\n", memory_id);
    printf("  Cache:      Memory Cache (%d)\n", memory_id);
    
    /* Show initial status */
    print_banner("Initial Backend Status");
    print_backend_status(mgr, memory_id, "Memory");
    printf("\n");
    print_backend_status(mgr, persistent_id, "Persistent");
    
    /* Run demos */
    demo_create_and_read(mgr);
    
    print_banner("Backend Status After Creates");
    print_backend_status(mgr, memory_id, "Memory");
    printf("\n");
    print_backend_status(mgr, persistent_id, "Persistent");
    
    demo_hotness_tracking(mgr);
    demo_caching(mgr);
    
    print_banner("Final Backend Status");
    print_backend_status(mgr, memory_id, "Memory");
    printf("\n");
    print_backend_status(mgr, persistent_id, "Persistent");
    
    demo_delete(mgr);
    
    print_banner("Backend Status After Deletes");
    print_backend_status(mgr, memory_id, "Memory");
    printf("\n");
    print_backend_status(mgr, persistent_id, "Persistent");
    
    /* Cleanup */
    backend_manager_destroy(mgr);
    
    printf("\n=== Demo Complete ===\n");
    
    return 0;
}
