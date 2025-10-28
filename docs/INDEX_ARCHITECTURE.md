# Index Architecture Design

## Overview

The objmapper index provides fast URI → file descriptor lookups with:
- Hash-based O(1) lookup
- Backend integration for multi-tier storage
- RCU-style concurrency (lock-free reads, coordinated updates)
- File descriptor lifecycle management
- Optional persistent index for fast startup
- Concurrent access support with reference counting

## Core Concepts

### Index Types

1. **In-Memory Hash Index**
   - Primary lookup structure
   - Hash table: URI → index_entry
   - Lock-free reads using RCU
   - Coordinated writes with minimal locking

2. **Persistent Index (per backend)**
   - Stored on-disk for fast initialization
   - Format: Binary hash table dump or B-tree
   - Loaded at startup to warm cache
   - Updated asynchronously

3. **Backend-Specific Indexes**
   - Each backend maintains its own index
   - Enables parallel lookups across backends
   - Independent lifecycle management

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                       Client Request                            │
│                     GET /path/to/object                         │
└────────────────────────────┬────────────────────────────────────┘
                             │
                             ▼
┌─────────────────────────────────────────────────────────────────┐
│                     Global Index Manager                        │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │         RCU Hash Table (URI → index_entry)               │  │
│  │  - Lock-free reads                                       │  │
│  │  - Reference counting for FDs                            │  │
│  │  - Grace period for safe reclamation                     │  │
│  └──────────────────────────────────────────────────────────┘  │
└────────────────────────────┬────────────────────────────────────┘
                             │
        ┌────────────────────┼────────────────────┐
        ▼                    ▼                    ▼
┌───────────────┐    ┌───────────────┐    ┌───────────────┐
│ Backend Index │    │ Backend Index │    │ Backend Index │
│   (Memory)    │    │    (NVMe)     │    │    (HDD)      │
├───────────────┤    ├───────────────┤    ├───────────────┤
│ • In-memory   │    │ • Persistent  │    │ • Persistent  │
│   only        │    │   index file  │    │   index file  │
│ • No persist  │    │ • Fast load   │    │ • Fast load   │
└───────────────┘    └───────────────┘    └───────────────┘
```

## Data Structures

### Index Entry

```c
typedef struct index_entry index_entry_t;

struct index_entry {
    /* Key */
    char *uri;                    /* Object URI (owned) */
    uint64_t uri_hash;            /* Precomputed hash */
    
    /* Location */
    uint32_t backend_id;          /* Backend where object lives */
    char *backend_path;           /* Cached full path */
    
    /* File descriptor state */
    int fd;                       /* Open FD (-1 if closed) */
    atomic_int fd_refcount;       /* Reference count for FD */
    atomic_int fd_generation;     /* Generation counter for reopen detection */
    
    /* Object metadata */
    uint64_t size_bytes;          /* Object size */
    uint64_t mtime;               /* Modification time */
    uint32_t flags;               /* Object flags */
    
    /* Access tracking */
    atomic_uint64_t access_count; /* Total accesses */
    atomic_uint64_t last_access;  /* Last access timestamp */
    float hotness_score;          /* Cached hotness (updated periodically) */
    
    /* RCU support */
    struct rcu_head rcu;          /* RCU callback for safe deletion */
    atomic_int entry_refcount;    /* Entry reference count */
    
    /* Hash table linkage */
    index_entry_t *next;          /* Collision chain */
};
```

### Global Index

```c
typedef struct {
    /* Hash table */
    index_entry_t **buckets;      /* Array of entry lists */
    size_t num_buckets;           /* Number of buckets (power of 2) */
    atomic_size_t num_entries;    /* Total entries */
    
    /* RCU synchronization */
    atomic_uint64_t rcu_gp;       /* RCU grace period counter */
    pthread_mutex_t write_lock;   /* Serializes writers */
    
    /* FD management */
    size_t max_open_fds;          /* Max FDs to keep open */
    atomic_size_t num_open_fds;   /* Currently open FDs */
    
    /* LRU for FD eviction */
    struct list_head fd_lru;      /* LRU list of open FDs */
    pthread_mutex_t lru_lock;     /* Protects LRU list */
    
    /* Statistics */
    atomic_uint64_t lookups;
    atomic_uint64_t hits;
    atomic_uint64_t misses;
    atomic_uint64_t fd_cache_hits;
    atomic_uint64_t fd_opens;
    atomic_uint64_t fd_closes;
    
} global_index_t;
```

### Backend Index

```c
typedef struct {
    uint32_t backend_id;          /* Associated backend */
    
    /* In-memory index */
    index_entry_t **buckets;      /* Hash table buckets */
    size_t num_buckets;
    atomic_size_t num_entries;
    
    /* Persistent index */
    char *index_file_path;        /* Path to persistent index file */
    int persist_enabled;          /* Whether to persist index */
    atomic_int dirty;             /* Needs sync to disk */
    
    /* Statistics */
    atomic_uint64_t backend_lookups;
    atomic_uint64_t backend_hits;
    
    /* RCU */
    atomic_uint64_t rcu_gp;
    pthread_mutex_t write_lock;
    
} backend_index_t;
```

### FD Reference Handle

```c
typedef struct {
    index_entry_t *entry;         /* Associated index entry */
    int fd;                       /* File descriptor */
    int generation;               /* Generation when acquired */
} fd_ref_t;
```

## API Design

### Global Index Operations

```c
/**
 * Create global index
 * 
 * @param num_buckets Number of hash buckets (power of 2)
 * @param max_open_fds Maximum open file descriptors to cache
 * @return Global index, or NULL on error
 */
global_index_t *global_index_create(size_t num_buckets, size_t max_open_fds);

/**
 * Destroy global index
 * 
 * @param idx Global index
 */
void global_index_destroy(global_index_t *idx);

/**
 * Lookup object and get FD reference (lock-free read)
 * 
 * @param idx Global index
 * @param uri Object URI
 * @param fd_ref Output: FD reference (must be released)
 * @return 0 on success, -1 if not found
 */
int global_index_lookup(global_index_t *idx, const char *uri, fd_ref_t *fd_ref);

/**
 * Insert entry into global index
 * 
 * @param idx Global index
 * @param entry Entry to insert (takes ownership)
 * @return 0 on success, -1 on error
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
 * Update entry metadata (for migrations, etc.)
 * 
 * @param idx Global index
 * @param uri Object URI
 * @param backend_id New backend ID
 * @return 0 on success, -1 if not found
 */
int global_index_update_backend(global_index_t *idx, const char *uri, 
                                uint32_t backend_id);
```

### FD Reference Management

```c
/**
 * Acquire FD reference (increments refcount)
 * This gives you a guaranteed-valid FD that won't be closed
 * while you hold the reference.
 * 
 * @param fd_ref FD reference from lookup
 * @return File descriptor, or -1 on error
 */
int fd_ref_acquire(fd_ref_t *fd_ref);

/**
 * Release FD reference (decrements refcount)
 * Must be called when done using the FD.
 * 
 * @param fd_ref FD reference
 */
void fd_ref_release(fd_ref_t *fd_ref);

/**
 * Duplicate FD from reference (for long-term holding)
 * Returns a dup()'ed FD that caller owns.
 * 
 * @param fd_ref FD reference
 * @return Duplicated FD, or -1 on error
 */
int fd_ref_dup(fd_ref_t *fd_ref);
```

### Backend Index Operations

```c
/**
 * Create backend index
 * 
 * @param backend_id Backend ID
 * @param index_file_path Path to persistent index file (NULL to disable)
 * @param num_buckets Number of hash buckets
 * @return Backend index, or NULL on error
 */
backend_index_t *backend_index_create(uint32_t backend_id, 
                                      const char *index_file_path,
                                      size_t num_buckets);

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
 * Walks directory tree and indexes all objects.
 * 
 * @param idx Backend index
 * @param backend Backend info (for mount_path)
 * @param progress_cb Optional progress callback
 * @return Number of objects indexed, or -1 on error
 */
int backend_index_scan(backend_index_t *idx, const backend_info_t *backend,
                       void (*progress_cb)(size_t count, void *data), void *data);

/**
 * Insert entry into backend index
 * 
 * @param idx Backend index
 * @param entry Entry to insert
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
```

### Index Entry Management

```c
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
 * Acquire reference to entry (RCU-safe)
 * 
 * @param entry Index entry
 */
void index_entry_get(index_entry_t *entry);

/**
 * Release reference to entry
 * When refcount reaches zero, entry is freed via RCU.
 * 
 * @param entry Index entry
 */
void index_entry_put(index_entry_t *entry);

/**
 * Update access time and count
 * 
 * @param entry Index entry
 */
void index_entry_record_access(index_entry_t *entry);
```

## RCU Implementation

### Read-Side Critical Section

```c
/* Lock-free lookup */
int global_index_lookup(global_index_t *idx, const char *uri, fd_ref_t *fd_ref) {
    /* Enter RCU read-side critical section */
    rcu_read_lock();
    
    uint64_t hash = hash_string(uri);
    size_t bucket = hash & (idx->num_buckets - 1);
    
    /* Walk collision chain (lock-free) */
    index_entry_t *entry = atomic_load_explicit(&idx->buckets[bucket], 
                                                memory_order_consume);
    
    while (entry) {
        if (entry->uri_hash == hash && strcmp(entry->uri, uri) == 0) {
            /* Found! Acquire entry reference */
            index_entry_get(entry);
            
            /* Exit RCU critical section */
            rcu_read_unlock();
            
            /* Prepare FD reference */
            fd_ref->entry = entry;
            fd_ref->fd = -1;
            fd_ref->generation = 0;
            
            /* Record access */
            index_entry_record_access(entry);
            
            atomic_fetch_add(&idx->hits, 1);
            return 0;
        }
        
        entry = atomic_load_explicit(&entry->next, memory_order_consume);
    }
    
    rcu_read_unlock();
    atomic_fetch_add(&idx->misses, 1);
    return -1;
}
```

### Write-Side (Insert)

```c
int global_index_insert(global_index_t *idx, index_entry_t *entry) {
    /* Serialize writers */
    pthread_mutex_lock(&idx->write_lock);
    
    uint64_t hash = hash_string(entry->uri);
    entry->uri_hash = hash;
    size_t bucket = hash & (idx->num_buckets - 1);
    
    /* Check for duplicate */
    index_entry_t *existing = idx->buckets[bucket];
    while (existing) {
        if (existing->uri_hash == hash && strcmp(existing->uri, entry->uri) == 0) {
            pthread_mutex_unlock(&idx->write_lock);
            return -1;  /* Duplicate */
        }
        existing = existing->next;
    }
    
    /* Insert at head of collision chain */
    entry->next = idx->buckets[bucket];
    
    /* Atomic pointer update (becomes visible to readers) */
    atomic_store_explicit(&idx->buckets[bucket], entry, memory_order_release);
    
    atomic_fetch_add(&idx->num_entries, 1);
    
    pthread_mutex_unlock(&idx->write_lock);
    return 0;
}
```

### Write-Side (Remove)

```c
int global_index_remove(global_index_t *idx, const char *uri) {
    pthread_mutex_lock(&idx->write_lock);
    
    uint64_t hash = hash_string(uri);
    size_t bucket = hash & (idx->num_buckets - 1);
    
    index_entry_t **prev = &idx->buckets[bucket];
    index_entry_t *entry = *prev;
    
    while (entry) {
        if (entry->uri_hash == hash && strcmp(entry->uri, uri) == 0) {
            /* Unlink from chain */
            *prev = entry->next;
            
            /* Close FD if open */
            if (entry->fd >= 0) {
                close(entry->fd);
                atomic_fetch_sub(&idx->num_open_fds, 1);
            }
            
            /* Schedule RCU-deferred free */
            call_rcu(&entry->rcu, index_entry_free_rcu);
            
            atomic_fetch_sub(&idx->num_entries, 1);
            
            pthread_mutex_unlock(&idx->write_lock);
            return 0;
        }
        
        prev = &entry->next;
        entry = entry->next;
    }
    
    pthread_mutex_unlock(&idx->write_lock);
    return -1;
}
```

## FD Lifecycle Management

### FD Acquisition

```c
int fd_ref_acquire(fd_ref_t *fd_ref) {
    index_entry_t *entry = fd_ref->entry;
    
    /* Fast path: FD already open */
    int fd = atomic_load(&entry->fd);
    if (fd >= 0) {
        /* Increment FD refcount */
        int prev = atomic_fetch_add(&entry->fd_refcount, 1);
        
        /* Check generation (detect if FD was closed and reopened) */
        int gen = atomic_load(&entry->fd_generation);
        
        if (prev >= 0 && fd >= 0) {
            /* Still valid */
            fd_ref->fd = fd;
            fd_ref->generation = gen;
            return fd;
        }
        
        /* Race: FD closed, retry */
        atomic_fetch_sub(&entry->fd_refcount, 1);
    }
    
    /* Slow path: Need to open FD */
    if (index_entry_open_fd(entry) < 0) {
        return -1;
    }
    
    /* Acquire reference */
    atomic_fetch_add(&entry->fd_refcount, 1);
    fd_ref->fd = atomic_load(&entry->fd);
    fd_ref->generation = atomic_load(&entry->fd_generation);
    
    return fd_ref->fd;
}
```

### FD Release

```c
void fd_ref_release(fd_ref_t *fd_ref) {
    if (!fd_ref->entry) return;
    
    index_entry_t *entry = fd_ref->entry;
    
    /* Decrement FD refcount */
    int prev = atomic_fetch_sub(&entry->fd_refcount, 1);
    
    /* If refcount reached zero and FD is still open, update LRU */
    if (prev == 1 && atomic_load(&entry->fd) >= 0) {
        /* FD now eligible for eviction - move to LRU tail */
        update_fd_lru(entry);
    }
    
    /* Release entry reference */
    index_entry_put(entry);
    
    fd_ref->entry = NULL;
    fd_ref->fd = -1;
}
```

### FD Eviction (LRU)

```c
void evict_fd_if_needed(global_index_t *idx) {
    size_t num_open = atomic_load(&idx->num_open_fds);
    
    if (num_open < idx->max_open_fds) {
        return;  /* Under limit */
    }
    
    pthread_mutex_lock(&idx->lru_lock);
    
    /* Walk LRU from least recently used */
    struct list_head *pos, *tmp;
    list_for_each_safe(pos, tmp, &idx->fd_lru) {
        index_entry_t *entry = list_entry(pos, index_entry_t, lru_node);
        
        /* Check if FD has no references */
        int refcount = atomic_load(&entry->fd_refcount);
        if (refcount > 0) {
            continue;  /* Still in use */
        }
        
        /* Close FD */
        int fd = atomic_exchange(&entry->fd, -1);
        if (fd >= 0) {
            close(fd);
            atomic_fetch_sub(&idx->num_open_fds, 1);
            atomic_fetch_add(&idx->fd_closes, 1);
            
            /* Increment generation */
            atomic_fetch_add(&entry->fd_generation, 1);
        }
        
        /* Remove from LRU */
        list_del(pos);
        
        /* Evicted one, check if we're under limit */
        if (atomic_load(&idx->num_open_fds) < idx->max_open_fds * 0.9) {
            break;  /* Reached target */
        }
    }
    
    pthread_mutex_unlock(&idx->lru_lock);
}
```

## Persistent Index Format

### Binary Format (Simple)

```
┌────────────────────────────────────────────────────────────┐
│                    Index File Header                       │
├────────────────────────────────────────────────────────────┤
│ Magic: "OBJIDX" (6 bytes)                                  │
│ Version: uint16_t                                          │
│ Backend ID: uint32_t                                       │
│ Num Entries: uint64_t                                      │
│ Num Buckets: uint64_t                                      │
│ CRC32: uint32_t (header checksum)                          │
├────────────────────────────────────────────────────────────┤
│                   Entry Records (repeated)                 │
├────────────────────────────────────────────────────────────┤
│ URI Length: uint16_t                                       │
│ URI: char[uri_len]                                         │
│ Backend Path Length: uint16_t                              │
│ Backend Path: char[path_len]                               │
│ Size: uint64_t                                             │
│ Mtime: uint64_t                                            │
│ Flags: uint32_t                                            │
│ Access Count: uint64_t                                     │
│ Last Access: uint64_t                                      │
├────────────────────────────────────────────────────────────┤
│                    Footer                                  │
├────────────────────────────────────────────────────────────┤
│ CRC32: uint32_t (entire file checksum)                     │
└────────────────────────────────────────────────────────────┘
```

### Persistent Index Operations

```c
int backend_index_save(backend_index_t *idx) {
    char tmp_path[PATH_MAX];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", idx->index_file_path);
    
    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    
    /* Write header */
    struct index_header {
        char magic[6];
        uint16_t version;
        uint32_t backend_id;
        uint64_t num_entries;
        uint64_t num_buckets;
        uint32_t header_crc;
    } header;
    
    memcpy(header.magic, "OBJIDX", 6);
    header.version = 1;
    header.backend_id = idx->backend_id;
    header.num_entries = atomic_load(&idx->num_entries);
    header.num_buckets = idx->num_buckets;
    header.header_crc = 0;  /* Calculate after */
    
    header.header_crc = crc32(0, &header, sizeof(header) - 4);
    
    if (write_all(fd, &header, sizeof(header)) < 0) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }
    
    /* Write entries */
    uint32_t file_crc = 0;
    
    for (size_t i = 0; i < idx->num_buckets; i++) {
        index_entry_t *entry = idx->buckets[i];
        
        while (entry) {
            /* Entry record */
            uint16_t uri_len = strlen(entry->uri);
            uint16_t path_len = strlen(entry->backend_path);
            
            write_all(fd, &uri_len, sizeof(uri_len));
            write_all(fd, entry->uri, uri_len);
            write_all(fd, &path_len, sizeof(path_len));
            write_all(fd, entry->backend_path, path_len);
            write_all(fd, &entry->size_bytes, sizeof(entry->size_bytes));
            write_all(fd, &entry->mtime, sizeof(entry->mtime));
            write_all(fd, &entry->flags, sizeof(entry->flags));
            
            uint64_t access_count = atomic_load(&entry->access_count);
            uint64_t last_access = atomic_load(&entry->last_access);
            write_all(fd, &access_count, sizeof(access_count));
            write_all(fd, &last_access, sizeof(last_access));
            
            /* Update CRC */
            file_crc = crc32(file_crc, entry->uri, uri_len);
            
            entry = entry->next;
        }
    }
    
    /* Write footer */
    write_all(fd, &file_crc, sizeof(file_crc));
    
    close(fd);
    
    /* Atomic replace */
    if (rename(tmp_path, idx->index_file_path) < 0) {
        unlink(tmp_path);
        return -1;
    }
    
    atomic_store(&idx->dirty, 0);
    return 0;
}
```

## Integration with Backend Manager

### Initialization Flow

```c
/* At startup */
backend_manager_t *mgr = backend_manager_create();

/* Create global index */
global_index_t *global_idx = global_index_create(
    1024 * 1024,  /* 1M buckets */
    10000         /* Max 10K open FDs */
);

mgr->global_index = global_idx;

/* Register backends with indexes */
backend_info_t nvme_backend = { ... };
uint32_t nvme_id = backend_register(mgr, &nvme_backend);

/* Create backend index with persistence */
backend_index_t *nvme_idx = backend_index_create(
    nvme_id,
    "/mnt/objmapper/nvme/.objmapper_index",
    256 * 1024  /* 256K buckets */
);

/* Try to load persistent index */
if (backend_index_load(nvme_idx) < 0) {
    /* No index file, scan filesystem */
    printf("Scanning NVMe backend...\n");
    backend_index_scan(nvme_idx, &nvme_backend, progress_callback, NULL);
    
    /* Save for next startup */
    backend_index_save(nvme_idx);
}

/* Populate global index from backend index */
populate_global_from_backend(global_idx, nvme_idx);

mgr->backend_indexes[nvme_id] = nvme_idx;
```

### Request Handling

```c
int handle_request(objm_connection_t *conn, objm_request_t *req) {
    backend_manager_t *mgr = get_backend_manager();
    global_index_t *idx = mgr->global_index;
    
    /* Lookup in global index */
    fd_ref_t fd_ref;
    if (global_index_lookup(idx, req->uri, &fd_ref) == 0) {
        /* Found in index - acquire FD */
        int fd = fd_ref_acquire(&fd_ref);
        
        if (fd >= 0) {
            /* Fast path: Got FD from index */
            struct stat st;
            fstat(fd, &st);
            
            /* Build response */
            objm_response_t resp = {
                .request_id = req->id,
                .status = OBJM_STATUS_OK,
                .fd = fd,
                .content_len = st.st_size
            };
            
            /* Add backend info to metadata */
            uint8_t *metadata = objm_metadata_create(50);
            size_t meta_len = objm_metadata_add_backend(metadata, 0, 
                                                        fd_ref.entry->backend_id);
            resp.metadata = metadata;
            resp.metadata_len = meta_len;
            
            objm_server_send_response(conn, &resp);
            
            free(metadata);
            
            /* Release FD reference */
            fd_ref_release(&fd_ref);
            
            return 0;
        }
        
        /* FD acquisition failed, fall through to slow path */
        fd_ref_release(&fd_ref);
    }
    
    /* Slow path: Not in index or FD failed, search backends */
    uint32_t backend_id;
    int fd = backend_get_object(mgr, req->uri, &backend_id);
    
    if (fd < 0) {
        objm_server_send_error(conn, req->id, OBJM_STATUS_NOT_FOUND, NULL);
        return 0;
    }
    
    /* Add to index for next time */
    backend_info_t *backend = get_backend(mgr, backend_id);
    char full_path[PATH_MAX];
    snprintf(full_path, sizeof(full_path), "%s/%s", backend->mount_path, req->uri);
    
    index_entry_t *entry = index_entry_create(req->uri, backend_id, full_path);
    global_index_insert(idx, entry);
    
    /* Send response */
    objm_response_t resp = {
        .request_id = req->id,
        .status = OBJM_STATUS_OK,
        .fd = fd,
    };
    
    objm_server_send_response(conn, &resp);
    close(fd);
    
    return 0;
}
```

### Migration Integration

```c
int backend_migrate_object(backend_manager_t *mgr, const char *uri,
                           uint32_t target_backend_id, uint32_t flags) {
    global_index_t *idx = mgr->global_index;
    
    /* Lookup in global index */
    fd_ref_t fd_ref;
    if (global_index_lookup(idx, uri, &fd_ref) < 0) {
        return -1;  /* Not found */
    }
    
    index_entry_t *entry = fd_ref.entry;
    
    /* Perform migration (as before) */
    backend_info_t *src = get_backend(mgr, entry->backend_id);
    backend_info_t *dst = get_backend(mgr, target_backend_id);
    
    /* ... migration logic ... */
    
    /* Update global index */
    global_index_update_backend(idx, uri, target_backend_id);
    
    /* Update backend indexes */
    backend_index_remove(mgr->backend_indexes[entry->backend_id], uri);
    backend_index_insert(mgr->backend_indexes[target_backend_id], entry);
    
    /* Mark both backend indexes dirty */
    atomic_store(&mgr->backend_indexes[entry->backend_id]->dirty, 1);
    atomic_store(&mgr->backend_indexes[target_backend_id]->dirty, 1);
    
    fd_ref_release(&fd_ref);
    
    return 0;
}
```

## Concurrent Use Cases

### Use Case 1: Multiple Readers

```c
/* Thread 1 */
fd_ref_t ref1;
global_index_lookup(idx, "/path/obj", &ref1);
int fd1 = fd_ref_acquire(&ref1);
read(fd1, buf1, size);
fd_ref_release(&ref1);

/* Thread 2 (concurrent) */
fd_ref_t ref2;
global_index_lookup(idx, "/path/obj", &ref2);
int fd2 = fd_ref_acquire(&ref2);  // Same FD as thread 1
read(fd2, buf2, size);
fd_ref_release(&ref2);

/* Both threads get the same cached FD, refcount prevents premature close */
```

### Use Case 2: Read During Migration

```c
/* Reader thread */
fd_ref_t ref;
global_index_lookup(idx, "/obj", &ref);
int fd = fd_ref_acquire(&ref);
/* Safe to use FD - won't be closed during migration */
read(fd, buf, size);
fd_ref_release(&ref);

/* Migration thread (concurrent) */
backend_migrate_object(mgr, "/obj", new_backend, 0);
/* Migration waits for readers to finish before closing old FD */
```

### Use Case 3: Insert During Lookup

```c
/* Reader thread */
global_index_lookup(idx, "/new_obj", &ref);  // Returns -1

/* Writer thread (concurrent) */
index_entry_t *entry = index_entry_create("/new_obj", ...);
global_index_insert(idx, entry);  // Becomes visible atomically

/* Reader thread retries */
global_index_lookup(idx, "/new_obj", &ref);  // Returns 0 now
```

## Performance Characteristics

### Lookup Performance

- **Index lookup**: O(1) hash table, ~50-100ns
- **FD acquisition (cached)**: Atomic refcount increment, ~10ns
- **FD acquisition (open)**: open() syscall, ~1-5μs
- **Total (cached)**: ~100ns vs ~8μs for backend_get_object()
- **Speedup**: ~80× for cached FDs

### Memory Overhead

Per entry:
- Index entry: ~200 bytes
- Hash table overhead: ~8 bytes (pointer)
- 1M entries: ~200 MB

### Concurrency

- **Read throughput**: Millions of lookups/sec (lock-free)
- **Write throughput**: ~100K inserts/sec (serialized by write_lock)
- **FD refcount**: Atomic operations, no locks

## Configuration

```c
typedef struct {
    /* Global index */
    size_t global_index_buckets;   /* 1M default */
    size_t max_open_fds;            /* 10K default */
    
    /* Backend indexes */
    int enable_persistence;         /* 1 = enable */
    int auto_scan_on_load_failure;  /* 1 = scan if load fails */
    size_t backend_index_buckets;   /* 256K default */
    
    /* FD eviction */
    float fd_evict_threshold;       /* 0.9 = start at 90% capacity */
    float fd_evict_target;          /* 0.8 = evict down to 80% */
    
    /* Index sync */
    int sync_interval_secs;         /* Save dirty indexes every N seconds */
    
} index_config_t;
```

## Summary

This index architecture provides:

✅ **O(1) lookups** - Hash-based global and backend indexes  
✅ **Lock-free reads** - RCU for scalable concurrent lookups  
✅ **FD caching** - Reference counting prevents premature close  
✅ **Persistent indexes** - Fast startup, no filesystem scan  
✅ **Backend integration** - Seamless multi-tier lookups  
✅ **Concurrent safety** - Atomic operations, RCU grace periods  
✅ **80× speedup** - Cached FD vs filesystem open()  
✅ **Scalable** - Millions of lookups/sec on modern hardware  

Next: Implement index and integrate with backend manager + protocol library.
