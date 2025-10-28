/**
 * @file backend.h
 * @brief Backend manager for multi-tier object storage
 *
 * The backend manager provides:
 * - Multiple storage backends (memory, NVMe, SSD, HDD, network)
 * - Performance-based automatic tiering
 * - Ephemeral vs persistent object security
 * - Hotness-based migration
 * - Integration with index for fast lookups
 */

#ifndef BACKEND_H
#define BACKEND_H

#include "../index/index.h"
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

/* Backend types */
typedef enum {
    BACKEND_TYPE_MEMORY,     /* tmpfs - volatile, ephemeral only */
    BACKEND_TYPE_NVME,       /* NVMe SSD - fast persistent */
    BACKEND_TYPE_SSD,        /* SATA SSD - medium persistent */
    BACKEND_TYPE_HDD,        /* Hard disk - slow persistent */
    BACKEND_TYPE_NETWORK,    /* Network storage - slowest */
} backend_type_t;

/* Backend flags */
#define BACKEND_FLAG_EPHEMERAL_ONLY  (1 << 0)  /* Only ephemeral objects allowed */
#define BACKEND_FLAG_PERSISTENT      (1 << 1)  /* Supports persistent objects */
#define BACKEND_FLAG_ENABLED         (1 << 2)  /* Backend is active */
#define BACKEND_FLAG_READONLY        (1 << 3)  /* Read-only mode */
#define BACKEND_FLAG_MIGRATION_SRC   (1 << 4)  /* Can migrate objects out */
#define BACKEND_FLAG_MIGRATION_DST   (1 << 5)  /* Can migrate objects in */

/* Migration policy */
typedef enum {
    MIGRATION_POLICY_NONE,       /* No automatic migration */
    MIGRATION_POLICY_HOTNESS,    /* Migrate based on hotness score */
    MIGRATION_POLICY_CAPACITY,   /* Migrate based on capacity thresholds */
    MIGRATION_POLICY_HYBRID,     /* Both hotness and capacity */
} migration_policy_t;

/**
 * Backend information structure
 */
typedef struct backend_info {
    int id;                          /* Unique backend identifier */
    backend_type_t type;             /* Backend type */
    char *mount_path;                /* Filesystem mount point */
    char *name;                      /* Human-readable name */
    uint32_t flags;                  /* BACKEND_FLAG_* */
    
    /* Performance characteristics */
    double perf_factor;              /* Relative performance (1.0 = baseline) */
    uint64_t expected_latency_us;    /* Expected operation latency */
    
    /* Capacity */
    uint64_t capacity_bytes;         /* Total capacity */
    atomic_uint_least64_t used_bytes; /* Currently used */
    atomic_size_t object_count;      /* Number of objects */
    
    /* Thresholds for migration */
    double high_watermark;           /* Trigger migration out (0.0-1.0) */
    double low_watermark;            /* Stop migration out (0.0-1.0) */
    
    /* Migration policy */
    migration_policy_t migration_policy;
    double hotness_threshold;        /* Minimum hotness for migration in */
    uint64_t hotness_halflife_us;    /* Hotness decay halflife */
    
    /* Associated index */
    backend_index_t *index;          /* Object index for this backend */
    
    /* Statistics */
    atomic_size_t reads;             /* Total read operations */
    atomic_size_t writes;            /* Total write operations */
    atomic_size_t migrations_in;     /* Objects migrated in */
    atomic_size_t migrations_out;    /* Objects migrated out */
    
    /* Thread safety */
    pthread_rwlock_t rwlock;         /* Protects backend state */
    
} backend_info_t;

/**
 * Backend manager - coordinates multiple backends
 */
typedef struct backend_manager {
    global_index_t *global_index;    /* Global URI -> FD index */
    
    backend_info_t **backends;       /* Array of backends */
    size_t num_backends;             /* Number of backends */
    size_t max_backends;             /* Allocated capacity */
    
    /* Default backend for new objects */
    int default_backend_id;
    int ephemeral_backend_id;        /* For ephemeral objects */
    int cache_backend_id;            /* Memory backend for caching (-1 if none) */
    
    /* Caching thread (local migration) */
    pthread_t cache_thread;
    atomic_int cache_running;
    uint64_t cache_check_interval_us; /* How often to check for cache promotion */
    double cache_threshold;           /* Minimum hotness for caching */
    
    /* Thread safety */
    pthread_rwlock_t backends_lock;  /* Protects backends array */
    
    /* Statistics */
    atomic_size_t total_objects;
    atomic_size_t total_bytes;
    
} backend_manager_t;

/**
 * Object creation request
 */
typedef struct object_create_req {
    const char *uri;                 /* Object URI */
    int backend_id;                  /* Target backend (-1 = auto) */
    bool ephemeral;                  /* Ephemeral vs persistent */
    size_t size_hint;                /* Expected size (for allocation) */
    uint32_t flags;                  /* Additional flags */
} object_create_req_t;

/**
 * Object metadata
 */
typedef struct object_metadata {
    char *uri;
    int backend_id;
    char *fs_path;
    size_t size_bytes;
    time_t mtime;
    uint32_t flags;
    double hotness;
    uint64_t access_count;
} object_metadata_t;

/* ============================================================================
 * Backend Manager API
 * ============================================================================ */

/**
 * Create a new backend manager
 *
 * @param index_buckets Number of buckets for global index
 * @param max_open_fds Maximum open file descriptors
 * @return New backend manager or NULL on error
 */
backend_manager_t *backend_manager_create(size_t index_buckets, size_t max_open_fds);

/**
 * Destroy backend manager and all backends
 *
 * @param mgr Backend manager to destroy
 */
void backend_manager_destroy(backend_manager_t *mgr);

/**
 * Register a new backend
 *
 * @param mgr Backend manager
 * @param type Backend type
 * @param mount_path Filesystem mount point
 * @param name Human-readable name
 * @param capacity_bytes Total capacity
 * @param flags BACKEND_FLAG_* flags
 * @return Backend ID (>= 0) or -1 on error
 */
int backend_manager_register(backend_manager_t *mgr,
                             backend_type_t type,
                             const char *mount_path,
                             const char *name,
                             uint64_t capacity_bytes,
                             uint32_t flags);

/**
 * Get backend info by ID
 *
 * @param mgr Backend manager
 * @param backend_id Backend ID
 * @return Backend info or NULL if not found
 */
backend_info_t *backend_manager_get_backend(backend_manager_t *mgr, int backend_id);

/**
 * Set default backend for new objects
 *
 * @param mgr Backend manager
 * @param backend_id Backend ID
 * @return 0 on success, -1 on error
 */
int backend_manager_set_default(backend_manager_t *mgr, int backend_id);

/**
 * Set ephemeral backend (must have EPHEMERAL_ONLY flag)
 *
 * @param mgr Backend manager
 * @param backend_id Backend ID
 * @return 0 on success, -1 on error
 */
int backend_manager_set_ephemeral(backend_manager_t *mgr, int backend_id);

/**
 * Set cache backend (must be memory type)
 *
 * @param mgr Backend manager
 * @param backend_id Backend ID
 * @return 0 on success, -1 on error
 */
int backend_manager_set_cache(backend_manager_t *mgr, int backend_id);

/* ============================================================================
 * Object Operations API
 * ============================================================================ */

/**
 * Create a new object
 *
 * @param mgr Backend manager
 * @param req Creation request
 * @param ref_out Output FD reference
 * @return 0 on success, -1 on error
 */
int backend_create_object(backend_manager_t *mgr,
                          const object_create_req_t *req,
                          fd_ref_t *ref_out);

/**
 * Get an existing object
 *
 * @param mgr Backend manager
 * @param uri Object URI
 * @param ref_out Output FD reference
 * @return 0 on success, -1 on error
 */
int backend_get_object(backend_manager_t *mgr,
                       const char *uri,
                       fd_ref_t *ref_out);

/**
 * Delete an object
 *
 * @param mgr Backend manager
 * @param uri Object URI
 * @return 0 on success, -1 on error
 */
int backend_delete_object(backend_manager_t *mgr, const char *uri);

/**
 * Scan a backend filesystem and populate indexes
 *
 * @param mgr Backend manager
 * @param backend_id Backend to scan
 * @return Number of objects indexed, or -1 on error
 */
int backend_manager_scan(backend_manager_t *mgr, int backend_id);

/**
 * Get object metadata
 *
 * @param mgr Backend manager
 * @param uri Object URI
 * @param metadata_out Output metadata structure
 * @return 0 on success, -1 on error
 */
int backend_get_metadata(backend_manager_t *mgr,
                         const char *uri,
                         object_metadata_t *metadata_out);

/**
 * Update object size (after write)
 *
 * @param mgr Backend manager
 * @param uri Object URI
 * @param new_size New size in bytes
 * @return 0 on success, -1 on error
 */
int backend_update_size(backend_manager_t *mgr,
                        const char *uri,
                        size_t new_size);

/* ============================================================================
 * Migration API
 * ============================================================================ */

/**
 * Migrate an object to a different backend
 *
 * @param mgr Backend manager
 * @param uri Object URI
 * @param target_backend_id Destination backend
 * @return 0 on success, -1 on error
 */
int backend_migrate_object(backend_manager_t *mgr,
                           const char *uri,
                           int target_backend_id);

/**
 * Start automatic caching (hot objects to memory backend)
 *
 * @param mgr Backend manager
 * @param check_interval_us How often to check for cache promotion (microseconds)
 * @param cache_threshold Minimum hotness for caching (0.0-1.0)
 * @return 0 on success, -1 on error
 */
int backend_start_caching(backend_manager_t *mgr, 
                         uint64_t check_interval_us,
                         double cache_threshold);

/**
 * Stop automatic caching thread
 *
 * @param mgr Backend manager
 */
void backend_stop_caching(backend_manager_t *mgr);

/**
 * Manually promote object to cache (memory backend)
 *
 * @param mgr Backend manager
 * @param uri Object URI
 * @return 0 on success, -1 on error
 */
int backend_cache_object(backend_manager_t *mgr, const char *uri);

/**
 * Evict object from cache back to persistent storage
 *
 * @param mgr Backend manager
 * @param uri Object URI
 * @return 0 on success, -1 on error
 */
int backend_evict_object(backend_manager_t *mgr, const char *uri);

/* ============================================================================
 * Backend Management API
 * ============================================================================ */

/**
 * Enable/disable a backend
 *
 * @param mgr Backend manager
 * @param backend_id Backend ID
 * @param enabled true to enable, false to disable
 * @return 0 on success, -1 on error
 */
int backend_set_enabled(backend_manager_t *mgr, int backend_id, bool enabled);

/**
 * Set backend capacity thresholds
 *
 * @param mgr Backend manager
 * @param backend_id Backend ID
 * @param high_watermark Trigger migration out (0.0-1.0)
 * @param low_watermark Stop migration out (0.0-1.0)
 * @return 0 on success, -1 on error
 */
int backend_set_watermarks(backend_manager_t *mgr,
                           int backend_id,
                           double high_watermark,
                           double low_watermark);

/**
 * Set migration policy for backend
 *
 * @param mgr Backend manager
 * @param backend_id Backend ID
 * @param policy Migration policy
 * @param hotness_threshold Minimum hotness for migration in
 * @return 0 on success, -1 on error
 */
int backend_set_migration_policy(backend_manager_t *mgr,
                                 int backend_id,
                                 migration_policy_t policy,
                                 double hotness_threshold);

/**
 * Get backend statistics
 *
 * @param mgr Backend manager
 * @param backend_id Backend ID
 * @param reads_out Output read count
 * @param writes_out Output write count
 * @param migrations_in_out Output migrations in
 * @param migrations_out_out Output migrations out
 * @return 0 on success, -1 on error
 */
int backend_get_stats(backend_manager_t *mgr,
                      int backend_id,
                      size_t *reads_out,
                      size_t *writes_out,
                      size_t *migrations_in_out,
                      size_t *migrations_out_out);

/**
 * Get detailed backend status for external controller
 *
 * @param mgr Backend manager
 * @param backend_id Backend ID
 * @param capacity_out Total capacity in bytes
 * @param used_out Used capacity in bytes
 * @param objects_out Number of objects
 * @param utilization_out Utilization ratio (0.0-1.0)
 * @return 0 on success, -1 on error
 */
int backend_get_status(backend_manager_t *mgr,
                       int backend_id,
                       uint64_t *capacity_out,
                       uint64_t *used_out,
                       size_t *objects_out,
                       double *utilization_out);

/**
 * Get list of all objects in a backend
 *
 * @param mgr Backend manager
 * @param backend_id Backend ID
 * @param uris_out Output array of URIs (caller must free each string and array)
 * @param count_out Output count of URIs
 * @return 0 on success, -1 on error
 */
int backend_list_objects(backend_manager_t *mgr,
                        int backend_id,
                        char ***uris_out,
                        size_t *count_out);

/**
 * Get hotness scores for all objects in backend
 *
 * @param mgr Backend manager
 * @param backend_id Backend ID
 * @param uris_out Output array of URIs (caller must free)
 * @param scores_out Output array of hotness scores (caller must free)
 * @param count_out Output count
 * @return 0 on success, -1 on error
 */
int backend_get_hotness_map(backend_manager_t *mgr,
                            int backend_id,
                            char ***uris_out,
                            double **scores_out,
                            size_t *count_out);

/**
 * Get global index statistics
 *
 * @param mgr Backend manager
 * @param stats_out Output index statistics
 * @return 0 on success, -1 on error
 */
int backend_get_index_stats(backend_manager_t *mgr, index_stats_t *stats_out);

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/**
 * Get backend type name
 *
 * @param type Backend type
 * @return String name
 */
const char *backend_type_name(backend_type_t type);

/**
 * Get default performance factor for backend type
 *
 * @param type Backend type
 * @return Performance factor (1.0 = baseline)
 */
double backend_default_perf_factor(backend_type_t type);

/**
 * Calculate expected latency for backend type
 *
 * @param type Backend type
 * @return Latency in microseconds
 */
uint64_t backend_expected_latency(backend_type_t type);

/**
 * Free object metadata
 *
 * @param metadata Metadata to free
 */
void object_metadata_free(object_metadata_t *metadata);

#endif /* BACKEND_H */
