# Backend Architecture Design

## Overview

The objmapper backend system provides a flexible, pluggable storage architecture with:
- Multiple backend types (memory, SSD, HDD, network)
- Performance-based tiering and automatic migration
- Security constraints (ephemeral vs persistent)
- Unified interface with backend-specific optimizations
- Lifecycle management and cache eviction

## Core Concepts

### Backend Types

1. **Memory Backend (tmpfs)**
   - Fastest access (performance factor: 1.0)
   - Volatile storage (lost on reboot)
   - Security flag: ephemeral-only objects cannot migrate to persistent storage
   - Use cases: Hot cache, sensitive data, temporary objects

2. **NVMe/M.2 Backend**
   - Very fast SSD (performance factor: 2-5)
   - Persistent storage
   - Use cases: Active working set, frequently accessed data

3. **SATA SSD Backend**
   - Fast SSD (performance factor: 5-10)
   - Persistent storage
   - Use cases: General storage tier

4. **HDD Backend**
   - Slow rotational disk (performance factor: 50-100)
   - Persistent storage
   - Use cases: Cold storage, archival

5. **Network Backend (NFS/S3)**
   - Variable latency (performance factor: 100-1000)
   - Persistent storage
   - Use cases: Shared storage, backup, archival

### Performance Factor

Relative latency multiplier compared to memory:
- Memory (tmpfs): 1.0x (baseline ~1μs)
- NVMe: 2-5x (~2-5μs)
- SATA SSD: 5-10x (~5-10μs)
- HDD: 50-100x (~50-100μs)
- Network: 100-1000x (~100μs-1ms+)

## Architecture

### Backend Interface

```
┌─────────────────────────────────────────────────────────────┐
│                    Client Requests                          │
└───────────────────────────┬─────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────┐
│                 Backend Manager                             │
│  - Route requests to appropriate backend                    │
│  - Handle multi-backend lookups                             │
│  - Coordinate migrations                                    │
└───────────────────────────┬─────────────────────────────────┘
                            │
        ┌───────────────────┼───────────────────┬──────────────┐
        ▼                   ▼                   ▼              ▼
┌───────────────┐   ┌───────────────┐   ┌───────────────┐   ┌──────────────┐
│ Memory Backend│   │  SSD Backend  │   │  HDD Backend  │   │ Net Backend  │
│  (tmpfs)      │   │  (ext4/xfs)   │   │  (ext4/xfs)   │   │  (NFS/S3)    │
├───────────────┤   ├───────────────┤   ├───────────────┤   ├──────────────┤
│ Ephemeral     │   │ Persistent    │   │ Persistent    │   │ Persistent   │
│ Perf: 1.0x    │   │ Perf: 5x      │   │ Perf: 50x     │   │ Perf: 500x   │
└───────────────┘   └───────────────┘   └───────────────┘   └──────────────┘
```

### Object Lifecycle Manager

```
┌─────────────────────────────────────────────────────────────┐
│              Object Lifecycle Manager                       │
│                                                             │
│  ┌─────────────┐  ┌──────────────┐  ┌─────────────────┐   │
│  │   Access    │  │  Migration   │  │   Eviction      │   │
│  │   Tracker   │  │   Policy     │  │   Policy        │   │
│  └─────────────┘  └──────────────┘  └─────────────────┘   │
└─────────────────────────────────────────────────────────────┘
         │                   │                    │
         ▼                   ▼                    ▼
  Track hotness        Move objects         Free space
  per object          between tiers        when needed
```

## Data Structures

### Backend Descriptor

```c
typedef enum {
    BACKEND_TYPE_MEMORY,     /* tmpfs, RAM disk */
    BACKEND_TYPE_NVME,       /* NVMe SSD */
    BACKEND_TYPE_SSD,        /* SATA SSD */
    BACKEND_TYPE_HDD,        /* Rotational disk */
    BACKEND_TYPE_NETWORK     /* NFS, S3, etc. */
} backend_type_t;

typedef enum {
    BACKEND_FLAG_PERSISTENT   = 0x01,  /* Survives reboot */
    BACKEND_FLAG_EPHEMERAL    = 0x02,  /* Volatile, lost on reboot */
    BACKEND_FLAG_READABLE     = 0x04,  /* Can read objects */
    BACKEND_FLAG_WRITABLE     = 0x08,  /* Can write objects */
    BACKEND_FLAG_MIGRATION_SRC = 0x10, /* Can migrate objects out */
    BACKEND_FLAG_MIGRATION_DST = 0x20, /* Can accept migrated objects */
} backend_flags_t;

typedef struct {
    uint32_t backend_id;          /* Unique backend ID */
    backend_type_t type;          /* Backend type */
    uint32_t flags;               /* Backend flags */
    
    /* Performance characteristics */
    float perf_factor;            /* Latency multiplier (1.0 = memory) */
    uint64_t bandwidth_mbps;      /* Sequential bandwidth */
    uint32_t iops;                /* Random I/O operations/sec */
    
    /* Capacity */
    uint64_t capacity_bytes;      /* Total capacity */
    uint64_t used_bytes;          /* Currently used */
    uint64_t reserved_bytes;      /* Reserved space */
    
    /* Configuration */
    char *mount_path;             /* Filesystem mount point */
    char *name;                   /* Human-readable name */
    
    /* Statistics */
    uint64_t num_objects;         /* Objects stored */
    uint64_t total_reads;         /* Read operations */
    uint64_t total_writes;        /* Write operations */
    uint64_t total_migrations_in; /* Objects migrated in */
    uint64_t total_migrations_out; /* Objects migrated out */
} backend_info_t;
```

### Object Metadata

```c
typedef enum {
    OBJECT_FLAG_EPHEMERAL      = 0x01,  /* Must stay in volatile storage */
    OBJECT_FLAG_PERSISTENT     = 0x02,  /* Must be in persistent storage */
    OBJECT_FLAG_PINNED         = 0x04,  /* Cannot be evicted/migrated */
    OBJECT_FLAG_ENCRYPTED      = 0x08,  /* Encrypted at rest */
    OBJECT_FLAG_COMPRESSED     = 0x10,  /* Compressed */
    OBJECT_FLAG_HOT            = 0x20,  /* Frequently accessed (auto-set) */
    OBJECT_FLAG_COLD           = 0x40,  /* Rarely accessed (auto-set) */
} object_flags_t;

typedef struct {
    char *uri;                    /* Object URI */
    uint32_t backend_id;          /* Current backend */
    uint32_t flags;               /* Object flags */
    
    /* Access tracking */
    uint64_t access_count;        /* Total accesses */
    uint64_t last_access_time;    /* Unix timestamp */
    uint64_t creation_time;       /* Unix timestamp */
    uint64_t modification_time;   /* Unix timestamp */
    
    /* Size */
    uint64_t size_bytes;          /* Object size */
    
    /* Hotness score (for migration decisions) */
    float hotness_score;          /* 0.0-1.0, higher = hotter */
    
    /* Migration state */
    uint32_t migration_target;    /* Target backend ID (if migrating) */
    uint8_t migration_progress;   /* 0-100% */
} object_metadata_t;
```

### Backend Manager

```c
typedef struct backend_manager backend_manager_t;

struct backend_manager {
    /* Registered backends (array sorted by perf_factor) */
    backend_info_t **backends;
    size_t num_backends;
    size_t backends_capacity;
    
    /* Object metadata index (hash table: URI -> metadata) */
    void *object_index;  /* Hash table */
    
    /* Migration queue */
    void *migration_queue;  /* Priority queue */
    
    /* Lifecycle manager */
    void *lifecycle_mgr;
    
    /* Statistics */
    uint64_t total_lookups;
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t total_migrations;
};
```

## API Design

### Backend Management

```c
/**
 * Initialize backend manager
 */
backend_manager_t *backend_manager_create(void);

/**
 * Register a backend
 * 
 * @param mgr Backend manager
 * @param info Backend information
 * @return Backend ID, or 0 on error
 */
uint32_t backend_register(backend_manager_t *mgr, const backend_info_t *info);

/**
 * Unregister a backend (moves all objects first)
 * 
 * @param mgr Backend manager
 * @param backend_id Backend to remove
 * @return 0 on success, -1 on error
 */
int backend_unregister(backend_manager_t *mgr, uint32_t backend_id);

/**
 * Get backend info
 * 
 * @param mgr Backend manager
 * @param backend_id Backend ID
 * @return Backend info, or NULL if not found
 */
const backend_info_t *backend_get_info(backend_manager_t *mgr, uint32_t backend_id);

/**
 * List all backends (sorted by performance)
 * 
 * @param mgr Backend manager
 * @param backends Output: array of backend IDs
 * @param max_backends Maximum backends to return
 * @return Number of backends
 */
size_t backend_list(backend_manager_t *mgr, uint32_t *backends, size_t max_backends);
```

### Object Operations

```c
/**
 * Store object in appropriate backend
 * 
 * @param mgr Backend manager
 * @param uri Object URI
 * @param data Object data (or NULL for FD passing)
 * @param size Object size
 * @param flags Object flags (EPHEMERAL, PERSISTENT, etc.)
 * @return 0 on success, -1 on error
 */
int backend_put_object(backend_manager_t *mgr, const char *uri,
                       const void *data, size_t size, uint32_t flags);

/**
 * Retrieve object (from any backend)
 * 
 * @param mgr Backend manager
 * @param uri Object URI
 * @param backend_id Output: backend ID where object was found (can be NULL)
 * @return File descriptor, or -1 if not found
 */
int backend_get_object(backend_manager_t *mgr, const char *uri, uint32_t *backend_id);

/**
 * Delete object from backend
 * 
 * @param mgr Backend manager
 * @param uri Object URI
 * @return 0 on success, -1 on error
 */
int backend_delete_object(backend_manager_t *mgr, const char *uri);

/**
 * Get object metadata
 * 
 * @param mgr Backend manager
 * @param uri Object URI
 * @return Metadata, or NULL if not found
 */
const object_metadata_t *backend_get_metadata(backend_manager_t *mgr, const char *uri);

/**
 * Check if object exists in any backend
 * 
 * @param mgr Backend manager
 * @param uri Object URI
 * @return 1 if exists, 0 if not
 */
int backend_object_exists(backend_manager_t *mgr, const char *uri);
```

### Migration Operations

```c
/**
 * Migrate object to different backend
 * 
 * @param mgr Backend manager
 * @param uri Object URI
 * @param target_backend_id Target backend
 * @param flags Migration flags (ASYNC, VERIFY, etc.)
 * @return 0 on success, -1 on error
 */
int backend_migrate_object(backend_manager_t *mgr, const char *uri,
                           uint32_t target_backend_id, uint32_t flags);

/**
 * Migrate object to best backend based on access pattern
 * 
 * @param mgr Backend manager
 * @param uri Object URI
 * @return 0 on success, -1 on error
 */
int backend_auto_migrate_object(backend_manager_t *mgr, const char *uri);

/**
 * Get migration status
 * 
 * @param mgr Backend manager
 * @param uri Object URI
 * @param progress Output: progress percentage (0-100)
 * @return 1 if migrating, 0 if not, -1 on error
 */
int backend_get_migration_status(backend_manager_t *mgr, const char *uri, uint8_t *progress);
```

### Lifecycle Management

```c
/**
 * Update object access time and count (called on each access)
 * 
 * @param mgr Backend manager
 * @param uri Object URI
 */
void backend_record_access(backend_manager_t *mgr, const char *uri);

/**
 * Run lifecycle policies (migration, eviction)
 * Should be called periodically by background thread
 * 
 * @param mgr Backend manager
 * @return Number of actions taken
 */
int backend_run_lifecycle(backend_manager_t *mgr);

/**
 * Set lifecycle policy parameters
 */
typedef struct {
    /* Migration thresholds */
    float hot_threshold;          /* Hotness score to promote (0.0-1.0) */
    float cold_threshold;         /* Hotness score to demote (0.0-1.0) */
    
    /* Eviction */
    float evict_when_full;        /* Start evicting at X% capacity */
    float evict_target;           /* Evict until X% capacity */
    
    /* Timing */
    uint32_t hotness_decay_secs;  /* Decay half-life for hotness score */
    uint32_t migration_batch_size; /* Max objects to migrate per cycle */
    
} lifecycle_policy_t;

int backend_set_lifecycle_policy(backend_manager_t *mgr, const lifecycle_policy_t *policy);
```

## Implementation Strategy

### 1. Backend Selection (for new objects)

```c
uint32_t select_backend_for_object(backend_manager_t *mgr, uint32_t flags) {
    /* Ephemeral objects MUST go to memory backend */
    if (flags & OBJECT_FLAG_EPHEMERAL) {
        return find_backend_by_type(mgr, BACKEND_TYPE_MEMORY);
    }
    
    /* Persistent objects prefer fastest available backend with capacity */
    for (size_t i = 0; i < mgr->num_backends; i++) {
        backend_info_t *backend = mgr->backends[i];
        
        /* Skip non-persistent backends */
        if (!(backend->flags & BACKEND_FLAG_PERSISTENT)) continue;
        
        /* Check capacity */
        uint64_t available = backend->capacity_bytes - backend->used_bytes;
        if (available < needed_size) continue;
        
        return backend->backend_id;
    }
    
    return 0;  /* No suitable backend */
}
```

### 2. Object Lookup (multi-backend search)

```c
int backend_get_object(backend_manager_t *mgr, const char *uri, uint32_t *backend_id) {
    /* Check metadata index first */
    object_metadata_t *meta = hash_table_get(mgr->object_index, uri);
    
    if (meta) {
        /* Found in index - go directly to backend */
        backend_info_t *backend = get_backend(mgr, meta->backend_id);
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", backend->mount_path, uri);
        
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            /* Update access tracking */
            backend_record_access(mgr, uri);
            if (backend_id) *backend_id = meta->backend_id;
            return fd;
        }
    }
    
    /* Not in index or open failed - search all backends */
    for (size_t i = 0; i < mgr->num_backends; i++) {
        backend_info_t *backend = mgr->backends[i];
        
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", backend->mount_path, uri);
        
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            /* Found! Update index */
            update_or_create_metadata(mgr, uri, backend->backend_id);
            backend_record_access(mgr, uri);
            if (backend_id) *backend_id = backend->backend_id;
            return fd;
        }
    }
    
    return -1;  /* Not found in any backend */
}
```

### 3. Hotness Calculation

```c
float calculate_hotness(object_metadata_t *meta, uint64_t current_time) {
    /* Time-decayed access frequency */
    uint64_t age_secs = current_time - meta->last_access_time;
    uint64_t decay_halflife = 3600;  /* 1 hour */
    
    float time_factor = exp(-0.693 * age_secs / decay_halflife);
    
    /* Recent access count (last 24 hours) */
    float access_factor = (float)meta->access_count / 1000.0;
    if (access_factor > 1.0) access_factor = 1.0;
    
    /* Combined hotness score */
    return 0.7 * time_factor + 0.3 * access_factor;
}
```

### 4. Migration Decision

```c
int should_migrate(backend_manager_t *mgr, object_metadata_t *meta) {
    backend_info_t *current = get_backend(mgr, meta->backend_id);
    
    /* Ephemeral objects cannot migrate to persistent backends */
    if ((meta->flags & OBJECT_FLAG_EPHEMERAL) && 
        (current->flags & BACKEND_FLAG_EPHEMERAL)) {
        return 0;  /* Already in correct place */
    }
    
    /* Pinned objects cannot migrate */
    if (meta->flags & OBJECT_FLAG_PINNED) {
        return 0;
    }
    
    /* Hot objects should move to faster backends */
    if (meta->hotness_score > 0.7) {
        /* Find faster backend */
        for (size_t i = 0; i < mgr->num_backends; i++) {
            backend_info_t *candidate = mgr->backends[i];
            
            /* Skip if not faster */
            if (candidate->perf_factor >= current->perf_factor) continue;
            
            /* Check constraints */
            if ((meta->flags & OBJECT_FLAG_EPHEMERAL) && 
                !(candidate->flags & BACKEND_FLAG_EPHEMERAL)) continue;
            
            /* Check capacity */
            uint64_t available = candidate->capacity_bytes - candidate->used_bytes;
            if (available < meta->size_bytes) continue;
            
            /* Found better backend */
            meta->migration_target = candidate->backend_id;
            return 1;
        }
    }
    
    /* Cold objects should move to slower backends */
    if (meta->hotness_score < 0.3) {
        /* Find slower backend with more capacity */
        for (size_t i = mgr->num_backends - 1; i >= 0; i--) {
            backend_info_t *candidate = mgr->backends[i];
            
            /* Skip if not slower */
            if (candidate->perf_factor <= current->perf_factor) continue;
            
            /* Check constraints */
            if ((meta->flags & OBJECT_FLAG_EPHEMERAL) && 
                !(candidate->flags & BACKEND_FLAG_EPHEMERAL)) continue;
            
            /* Check capacity */
            uint64_t available = candidate->capacity_bytes - candidate->used_bytes;
            if (available < meta->size_bytes) continue;
            
            /* Found suitable backend */
            meta->migration_target = candidate->backend_id;
            return 1;
        }
    }
    
    return 0;  /* No migration needed */
}
```

### 5. Object Migration

```c
int migrate_object(backend_manager_t *mgr, object_metadata_t *meta) {
    backend_info_t *src = get_backend(mgr, meta->backend_id);
    backend_info_t *dst = get_backend(mgr, meta->migration_target);
    
    char src_path[PATH_MAX];
    char dst_path[PATH_MAX];
    char tmp_path[PATH_MAX];
    
    snprintf(src_path, sizeof(src_path), "%s/%s", src->mount_path, meta->uri);
    snprintf(dst_path, sizeof(dst_path), "%s/%s", dst->mount_path, meta->uri);
    snprintf(tmp_path, sizeof(tmp_path), "%s/.tmp_%s", dst->mount_path, meta->uri);
    
    /* Copy to temp file on destination */
    int src_fd = open(src_path, O_RDONLY);
    if (src_fd < 0) return -1;
    
    int dst_fd = open(tmp_path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (dst_fd < 0) {
        close(src_fd);
        return -1;
    }
    
    /* Use sendfile for efficient copy */
    off_t offset = 0;
    ssize_t sent = sendfile(dst_fd, src_fd, &offset, meta->size_bytes);
    
    close(src_fd);
    close(dst_fd);
    
    if (sent != meta->size_bytes) {
        unlink(tmp_path);
        return -1;
    }
    
    /* Atomic rename */
    if (rename(tmp_path, dst_path) < 0) {
        unlink(tmp_path);
        return -1;
    }
    
    /* Delete original */
    unlink(src_path);
    
    /* Update metadata */
    meta->backend_id = meta->migration_target;
    meta->migration_target = 0;
    meta->migration_progress = 0;
    
    /* Update backend stats */
    src->used_bytes -= meta->size_bytes;
    src->num_objects--;
    src->total_migrations_out++;
    
    dst->used_bytes += meta->size_bytes;
    dst->num_objects++;
    dst->total_migrations_in++;
    
    return 0;
}
```

## Usage Examples

### Example 1: Configure Tiered Storage

```c
backend_manager_t *mgr = backend_manager_create();

/* Register memory backend (tmpfs) */
backend_info_t mem_backend = {
    .type = BACKEND_TYPE_MEMORY,
    .flags = BACKEND_FLAG_EPHEMERAL | BACKEND_FLAG_READABLE | 
             BACKEND_FLAG_WRITABLE | BACKEND_FLAG_MIGRATION_SRC,
    .perf_factor = 1.0,
    .bandwidth_mbps = 50000,  /* 50 GB/s */
    .iops = 1000000,
    .capacity_bytes = 16ULL * 1024 * 1024 * 1024,  /* 16 GB */
    .mount_path = "/mnt/objmapper/memory",
    .name = "Memory Cache"
};
uint32_t mem_id = backend_register(mgr, &mem_backend);

/* Register NVMe SSD */
backend_info_t nvme_backend = {
    .type = BACKEND_TYPE_NVME,
    .flags = BACKEND_FLAG_PERSISTENT | BACKEND_FLAG_READABLE | 
             BACKEND_FLAG_WRITABLE | BACKEND_FLAG_MIGRATION_SRC | 
             BACKEND_FLAG_MIGRATION_DST,
    .perf_factor = 3.0,
    .bandwidth_mbps = 3500,
    .iops = 500000,
    .capacity_bytes = 1ULL * 1024 * 1024 * 1024 * 1024,  /* 1 TB */
    .mount_path = "/mnt/objmapper/nvme",
    .name = "NVMe Hot Tier"
};
uint32_t nvme_id = backend_register(mgr, &nvme_backend);

/* Register HDD */
backend_info_t hdd_backend = {
    .type = BACKEND_TYPE_HDD,
    .flags = BACKEND_FLAG_PERSISTENT | BACKEND_FLAG_READABLE | 
             BACKEND_FLAG_WRITABLE | BACKEND_FLAG_MIGRATION_DST,
    .perf_factor = 80.0,
    .bandwidth_mbps = 200,
    .iops = 150,
    .capacity_bytes = 10ULL * 1024 * 1024 * 1024 * 1024,  /* 10 TB */
    .mount_path = "/mnt/objmapper/hdd",
    .name = "HDD Cold Tier"
};
uint32_t hdd_id = backend_register(mgr, &hdd_backend);

/* Set lifecycle policy */
lifecycle_policy_t policy = {
    .hot_threshold = 0.7,
    .cold_threshold = 0.3,
    .evict_when_full = 0.95,
    .evict_target = 0.85,
    .hotness_decay_secs = 3600,
    .migration_batch_size = 100
};
backend_set_lifecycle_policy(mgr, &policy);
```

### Example 2: Store Ephemeral Object

```c
/* Store sensitive data that must never hit disk */
const char *secret_data = "sensitive authentication token";
int ret = backend_put_object(mgr, "/secrets/auth_token",
                             secret_data, strlen(secret_data),
                             OBJECT_FLAG_EPHEMERAL | OBJECT_FLAG_PINNED);

/* This object will:
 * - Be stored in memory backend only
 * - Never migrate to persistent storage
 * - Be lost on system reboot (intended)
 * - Cannot be evicted (pinned)
 */
```

### Example 3: Store and Auto-Tier Object

```c
/* Store object with auto-tiering */
backend_put_object(mgr, "/images/photo.jpg", photo_data, photo_size,
                  OBJECT_FLAG_PERSISTENT);

/* Object lifecycle:
 * 1. Initially stored in fastest available backend (NVMe)
 * 2. If accessed frequently, stays in fast tier
 * 3. If not accessed, automatically migrates to HDD
 * 4. If accessed again, migrates back to NVMe
 */
```

### Example 4: Manual Migration

```c
/* Move specific object to memory for performance */
backend_migrate_object(mgr, "/config/critical.conf", mem_id, 0);

/* Or use auto-migration based on access pattern */
backend_auto_migrate_object(mgr, "/data/dataset.bin");
```

### Example 5: Background Lifecycle Management

```c
/* Run in background thread */
void *lifecycle_thread(void *arg) {
    backend_manager_t *mgr = arg;
    
    while (1) {
        /* Run lifecycle policies every 60 seconds */
        int actions = backend_run_lifecycle(mgr);
        printf("Lifecycle: %d actions taken\n", actions);
        
        sleep(60);
    }
    
    return NULL;
}
```

## Integration with Protocol Library

The backend manager integrates seamlessly with the protocol library:

```c
/* In server request handler */
int handle_request(objm_connection_t *conn, objm_request_t *req) {
    backend_manager_t *mgr = get_backend_manager();
    
    /* Get object from appropriate backend */
    uint32_t backend_id;
    int fd = backend_get_object(mgr, req->uri, &backend_id);
    
    if (fd < 0) {
        objm_server_send_error(conn, req->id, OBJM_STATUS_NOT_FOUND, NULL);
        return 0;
    }
    
    /* Build response with backend info */
    uint8_t *metadata = objm_metadata_create(100);
    size_t meta_len = 0;
    
    struct stat st;
    fstat(fd, &st);
    
    meta_len = objm_metadata_add_size(metadata, meta_len, st.st_size);
    meta_len = objm_metadata_add_backend(metadata, meta_len, backend_id);
    
    /* Send response */
    objm_response_t resp = {
        .request_id = req->id,
        .status = OBJM_STATUS_OK,
        .fd = fd,
        .metadata = metadata,
        .metadata_len = meta_len
    };
    
    objm_server_send_response(conn, &resp);
    
    free(metadata);
    close(fd);
    
    return 0;
}
```

## Security Considerations

### Ephemeral Objects

Objects marked `OBJECT_FLAG_EPHEMERAL`:
- **MUST** be stored only in backends with `BACKEND_FLAG_EPHEMERAL`
- **CANNOT** migrate to persistent backends
- System enforces this at API level
- Use cases: passwords, tokens, temporary encryption keys

### Migration Constraints

```c
int validate_migration(object_metadata_t *obj, backend_info_t *dst) {
    /* Ephemeral -> Persistent: FORBIDDEN */
    if ((obj->flags & OBJECT_FLAG_EPHEMERAL) && 
        (dst->flags & BACKEND_FLAG_PERSISTENT)) {
        return -1;  /* Security violation */
    }
    
    /* Persistent -> Ephemeral: Allowed but data will be lost on reboot */
    /* (useful for promoting cold data to hot cache) */
    
    return 0;
}
```

## Performance Impact

### Backend Selection Overhead

- Metadata index lookup: O(1) hash table
- Backend sorted by performance: O(1) to find best
- **Total overhead**: ~100ns per request

### Multi-Backend Search

- Worst case: Check all backends: O(N) where N = number of backends
- Typical case: Found in index: O(1)
- Amortized: Very low after warmup

### Migration Overhead

- Async background operation (doesn't block requests)
- Uses `sendfile()` for zero-copy between filesystems
- Rate-limited by `migration_batch_size`

## Future Enhancements

1. **Replication**: Replicate hot objects across multiple backends for redundancy
2. **Prefetching**: Predict access patterns and pre-migrate objects
3. **Compression**: Transparent compression for cold storage backends
4. **Encryption**: Per-backend or per-object encryption
5. **Deduplication**: Content-addressable storage for identical objects
6. **Network backends**: S3, Azure Blob, Google Cloud Storage adapters
7. **Snapshots**: Point-in-time snapshots per backend
8. **Quotas**: Per-user or per-namespace quotas

## Summary

This backend architecture provides:

✅ **Pluggable backends** - Easy to add new storage types  
✅ **Performance tiering** - Automatic hot/cold data migration  
✅ **Security constraints** - Ephemeral objects never hit disk  
✅ **Unified interface** - Same API regardless of backend  
✅ **Lifecycle management** - Background task handles optimization  
✅ **Migration support** - Manual or automatic object movement  
✅ **Scalability** - Efficient multi-backend lookups  
✅ **Integration ready** - Works with protocol library  

Next steps: Implement backend manager and integrate with thread pool + protocol library.
