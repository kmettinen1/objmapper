/**
 * @file index.h
 * @brief RCU-based hash index for objmapper with FD lifecycle management
 */

#ifndef OBJMAPPER_INDEX_H
#define OBJMAPPER_INDEX_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <pthread.h>

#include "../backend/metadata_schema.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct index_entry index_entry_t;
typedef struct global_index global_index_t;
typedef struct backend_index backend_index_t;
typedef struct fd_ref fd_ref_t;

/* ============================================================================
 * Constants
 * ============================================================================ */

#define INDEX_DEFAULT_BUCKETS  (1024 * 1024)  /* 1M buckets */
#define INDEX_MAX_OPEN_FDS     10000          /* Max cached FDs */
#define INDEX_MAGIC            "OBJIDX"
#define INDEX_VERSION          2

/* Object flags */
#define INDEX_FLAG_EPHEMERAL   0x01  /* Volatile storage only */
#define INDEX_FLAG_PERSISTENT  0x02  /* Persistent storage */
#define INDEX_FLAG_PINNED      0x04  /* Cannot evict/migrate */
#define INDEX_FLAG_ENCRYPTED   0x08  /* Encrypted at rest */
#define INDEX_FLAG_COMPRESSED  0x10  /* Compressed */

/* ============================================================================
 * Types
 * ============================================================================ */

/**
 * Index entry (hash table node)
 * Lock-free reads via RCU, coordinated writes
 */
struct index_entry {
    /* Key */
    char *uri;                       /* Object URI (owned) */
    uint64_t uri_hash;               /* Precomputed hash */
    
    /* Location */
    uint32_t backend_id;             /* Backend where object lives */
    char *backend_path;              /* Full path to object */
    
    /* File descriptor state */
    atomic_int fd;                   /* Open FD (-1 if closed) */
    atomic_int fd_refcount;          /* FD reference count */
    atomic_int fd_generation;        /* Generation for reopen detection */
    
    /* Object metadata */
    uint64_t size_bytes;             /* Object size */
    uint64_t mtime;                  /* Modification time */
    uint32_t flags;                  /* Object flags */
    
    /* Access tracking */
    atomic_uint_fast64_t access_count;  /* Total accesses */
    atomic_uint_fast64_t last_access;   /* Last access timestamp (monotonic) */
    float hotness_score;             /* Cached hotness (updated periodically) */
    
    /* Entry lifecycle */
    atomic_int entry_refcount;       /* Entry reference count */
    
    /* Hash table linkage */
    atomic_uintptr_t next;           /* Next in collision chain (atomic for RCU) */

    /* Payload metadata */
    objm_payload_descriptor_t payload;
};

/**
 * FD reference handle
 * Holds reference to entry and FD, ensures FD stays valid
 */
struct fd_ref {
    index_entry_t *entry;            /* Associated entry */
    int fd;                          /* Cached FD value */
    int generation;                  /* Generation when acquired */
};

/**
 * Global index
 * RCU-protected hash table for fast lookups
 */
struct global_index {
    /* Hash table */
    atomic_uintptr_t *buckets;       /* Array of entry lists (atomic for RCU) */
    size_t num_buckets;              /* Number of buckets (power of 2) */
    atomic_size_t num_entries;       /* Total entries */
    
    /* Write coordination */
    pthread_mutex_t write_lock;      /* Serializes writers */
    
    /* FD management */
    size_t max_open_fds;             /* Max FDs to cache */
    atomic_size_t num_open_fds;      /* Currently open FDs */
    
    /* LRU for FD eviction */
    pthread_mutex_t lru_lock;        /* Protects LRU list */
    index_entry_t *lru_head;         /* LRU list head (MRU) */
    index_entry_t *lru_tail;         /* LRU list tail (LRU) */
    
    /* Statistics */
    atomic_uint_fast64_t stat_lookups;
    atomic_uint_fast64_t stat_hits;
    atomic_uint_fast64_t stat_misses;
    atomic_uint_fast64_t stat_fd_cache_hits;
    atomic_uint_fast64_t stat_fd_opens;
    atomic_uint_fast64_t stat_fd_closes;
    atomic_uint_fast64_t stat_fd_evictions;
};

/**
 * Backend index
 * Per-backend index with optional persistence
 */
struct backend_index {
    uint32_t backend_id;             /* Associated backend */
    
    /* Hash table */
    atomic_uintptr_t *buckets;       /* Entry lists */
    size_t num_buckets;
    atomic_size_t num_entries;
    
    /* Persistence */
    char *index_file_path;           /* Path to persistent index */
    int persist_enabled;             /* Whether to persist */
    atomic_int dirty;                /* Needs sync to disk */
    
    /* Statistics */
    atomic_uint_fast64_t stat_lookups;
    atomic_uint_fast64_t stat_hits;
    
    /* Write coordination */
    pthread_mutex_t write_lock;
};

/**
 * Index statistics
 */
typedef struct {
    uint64_t num_entries;
    uint64_t num_open_fds;
    uint64_t lookups;
    uint64_t hits;
    uint64_t misses;
    uint64_t fd_cache_hits;
    uint64_t fd_opens;
    uint64_t fd_closes;
    uint64_t fd_evictions;
    double hit_rate;
    double fd_cache_rate;
} index_stats_t;

/* ============================================================================
 * Global Index API
 * ============================================================================ */

/**
 * Create global index
 * 
 * @param num_buckets Number of hash buckets (will be rounded to power of 2)
 * @param max_open_fds Maximum open FDs to cache
 * @return Global index, or NULL on error
 */
global_index_t *global_index_create(size_t num_buckets, size_t max_open_fds);

/**
 * Destroy global index
 * Closes all FDs and frees memory.
 * 
 * @param idx Global index
 */
void global_index_destroy(global_index_t *idx);

/**
 * Lookup object and get FD reference (lock-free)
 * 
 * @param idx Global index
 * @param uri Object URI
 * @param fd_ref Output: FD reference (must be released with fd_ref_release)
 * @return 0 on success, -1 if not found
 */
int global_index_lookup(global_index_t *idx, const char *uri, fd_ref_t *fd_ref);

/**
 * Insert entry into global index
 * Takes ownership of entry.
 * 
 * @param idx Global index
 * @param entry Entry to insert
 * @return 0 on success, -1 on error (duplicate or error)
 */
int global_index_insert(global_index_t *idx, index_entry_t *entry);

/**
 * Remove entry from global index
 * 
 * @param idx Global index
 * @param uri Object URI
 * @return 0 on success, -1 if not found
 */
int global_index_remove(global_index_t *idx, const char *uri);

/**
 * Update entry backend location (for migrations)
 * 
 * @param idx Global index
 * @param uri Object URI
 * @param backend_id New backend ID
 * @param backend_path New backend path
 * @return 0 on success, -1 if not found
 */
int global_index_update_backend(global_index_t *idx, const char *uri,
                                uint32_t backend_id, const char *backend_path);

/**
 * Get index statistics
 * 
 * @param idx Global index
 * @param stats Output: statistics
 */
void global_index_get_stats(global_index_t *idx, index_stats_t *stats);

/* ============================================================================
 * FD Reference API
 * ============================================================================ */

/**
 * Acquire FD from reference
 * Increments refcount and ensures FD is open.
 * 
 * @param fd_ref FD reference from lookup
 * @return File descriptor, or -1 on error
 */
int fd_ref_acquire(fd_ref_t *fd_ref);

/**
 * Release FD reference
 * Decrements refcount. Must be called when done with FD.
 * 
 * @param fd_ref FD reference
 */
void fd_ref_release(fd_ref_t *fd_ref);

/**
 * Duplicate FD from reference (for long-term ownership)
 * Returns a dup()'ed FD that caller must close.
 * 
 * @param fd_ref FD reference
 * @return Duplicated FD, or -1 on error
 */
int fd_ref_dup(fd_ref_t *fd_ref);

/* ============================================================================
 * Backend Index API
 * ============================================================================ */

/**
 * Create backend index
 * 
 * @param backend_id Backend ID
 * @param index_file_path Path to persistent index (NULL to disable)
 * @param num_buckets Number of hash buckets
 * @return Backend index, or NULL on error
 */
backend_index_t *backend_index_create(uint32_t backend_id,
                                      const char *index_file_path,
                                      size_t num_buckets);

/**
 * Destroy backend index
 * 
 * @param idx Backend index
 */
void backend_index_destroy(backend_index_t *idx);

/**
 * Load persistent index from disk
 * 
 * @param idx Backend index
 * @return Number of entries loaded, or -1 on error
 */
int backend_index_load(backend_index_t *idx);

/**
 * Save index to disk
 * 
 * @param idx Backend index
 * @return 0 on success, -1 on error
 */
int backend_index_save(backend_index_t *idx);

/**
 * Scan backend filesystem and populate index
 * 
 * @param idx Backend index
 * @param mount_path Backend mount path
 * @param progress_cb Optional progress callback
 * @param user_data User data for callback
 * @return Number of objects indexed, or -1 on error
 */
int backend_index_scan(backend_index_t *idx, const char *mount_path,
                       void (*progress_cb)(size_t count, void *data),
                       void *user_data);

/**
 * Insert entry into backend index
 * 
 * @param idx Backend index
 * @param entry Entry to insert (shares ownership with caller)
 * @return 0 on success, -1 on error
 */
int backend_index_insert(backend_index_t *idx, index_entry_t *entry);

/**
 * Lookup in backend index
 * 
 * @param idx Backend index
 * @param uri Object URI
 * @return Index entry, or NULL if not found
 */
index_entry_t *backend_index_lookup(backend_index_t *idx, const char *uri);

/**
 * Remove from backend index
 * 
 * @param idx Backend index
 * @param uri Object URI
 * @return 0 on success, -1 if not found
 */
int backend_index_remove(backend_index_t *idx, const char *uri);

/* ============================================================================
 * Index Entry API
 * ============================================================================ */

/**
 * Create index entry
 * 
 * @param uri Object URI
 * @param backend_id Backend ID
 * @param backend_path Full path to object
 * @return Index entry, or NULL on error
 */
index_entry_t *index_entry_create(const char *uri, uint32_t backend_id,
                                  const char *backend_path);

/**
 * Acquire reference to entry (RCU-safe)
 * 
 * @param entry Index entry
 */
void index_entry_get(index_entry_t *entry);

/**
 * Release reference to entry
 * When refcount reaches zero, entry is freed.
 * 
 * @param entry Index entry
 */
void index_entry_put(index_entry_t *entry);

/**
 * Open FD for entry (if not already open)
 * 
 * @param entry Index entry
 * @return 0 on success, -1 on error
 */
int index_entry_open_fd(index_entry_t *entry);

/**
 * Close FD for entry
 * Only closes if refcount is zero.
 * 
 * @param entry Index entry
 */
void index_entry_close_fd(index_entry_t *entry);

/**
 * Record access to entry (updates access count and time)
 * 
 * @param entry Index entry
 */
void index_entry_record_access(index_entry_t *entry);

void index_entry_set_payload(index_entry_t *entry,
                             const objm_payload_descriptor_t *payload);

void index_entry_get_payload(const index_entry_t *entry,
                             objm_payload_descriptor_t *payload_out);

void index_entry_seed_identity_payload(index_entry_t *entry,
                                       uint64_t size_bytes);

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/**
 * Calculate hotness score for entry
 * Based on time-decayed access frequency.
 * 
 * @param entry Index entry
 * @param current_time Current time (monotonic)
 * @param decay_halflife Decay half-life in seconds
 * @return Hotness score (0.0-1.0)
 */
float index_calculate_hotness(const index_entry_t *entry, uint64_t current_time,
                              uint32_t decay_halflife);

/**
 * Hash string to uint64_t
 * 
 * @param str String to hash
 * @return Hash value
 */
uint64_t index_hash_string(const char *str);

/**
 * Round up to next power of 2
 * 
 * @param n Number
 * @return Next power of 2
 */
size_t index_next_power_of_2(size_t n);

#ifdef __cplusplus
}
#endif

#endif /* OBJMAPPER_INDEX_H */
