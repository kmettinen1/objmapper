/**
 * @file backend.c
 * @brief Backend manager implementation
 */

#include "backend.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <dirent.h>

static void backend_mark_index_dirty(backend_info_t *backend) {
    if (!backend || !backend->index) {
        return;
    }

    atomic_store(&backend->index->dirty, 1);
}

/* Helper to get monotonic time in microseconds */
static uint64_t get_monotonic_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* Helper to create directory recursively (like mkdir -p) */
static int mkdir_p(const char *path) {
    char tmp[1024];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/')
        tmp[len - 1] = 0;
        
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) < 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    
    if (mkdir(tmp, 0755) < 0 && errno != EEXIST) {
        return -1;
    }
    
    return 0;
}

/* ============================================================================
 * Backend Type Utilities
 * ============================================================================ */

const char *backend_type_name(backend_type_t type) {
    switch (type) {
        case BACKEND_TYPE_MEMORY:  return "memory";
        case BACKEND_TYPE_NVME:    return "nvme";
        case BACKEND_TYPE_SSD:     return "ssd";
        case BACKEND_TYPE_HDD:     return "hdd";
        case BACKEND_TYPE_NETWORK: return "network";
        default:                   return "unknown";
    }
}

double backend_default_perf_factor(backend_type_t type) {
    switch (type) {
        case BACKEND_TYPE_MEMORY:  return 1.0;
        case BACKEND_TYPE_NVME:    return 3.0;
        case BACKEND_TYPE_SSD:     return 7.5;   /* Average of 5-10 */
        case BACKEND_TYPE_HDD:     return 80.0;
        case BACKEND_TYPE_NETWORK: return 500.0;
        default:                   return 1.0;
    }
}

uint64_t backend_expected_latency(backend_type_t type) {
    /* Base latency is ~8μs for cached index lookup */
    const uint64_t base_latency_us = 8;
    return (uint64_t)(base_latency_us * backend_default_perf_factor(type));
}

/* ============================================================================
 * Backend Manager Creation/Destruction
 * ============================================================================ */

backend_manager_t *backend_manager_create(size_t index_buckets, size_t max_open_fds) {
    backend_manager_t *mgr = calloc(1, sizeof(backend_manager_t));
    if (!mgr) return NULL;
    
    /* Create global index */
    mgr->global_index = global_index_create(index_buckets, max_open_fds);
    if (!mgr->global_index) {
        free(mgr);
        return NULL;
    }
    
    /* Allocate backends array */
    mgr->max_backends = 16;
    mgr->backends = calloc(mgr->max_backends, sizeof(backend_info_t *));
    if (!mgr->backends) {
        global_index_destroy(mgr->global_index);
        free(mgr);
        return NULL;
    }
    
    mgr->num_backends = 0;
    mgr->default_backend_id = -1;
    mgr->ephemeral_backend_id = -1;
    mgr->cache_backend_id = -1;
    mgr->cache_check_interval_us = 5 * 1000000; /* 5 seconds default */
    mgr->cache_threshold = 0.7; /* Cache objects with hotness > 0.7 */
    atomic_init(&mgr->cache_running, 0);
    atomic_init(&mgr->total_objects, 0);
    atomic_init(&mgr->total_bytes, 0);
    
    pthread_rwlock_init(&mgr->backends_lock, NULL);
    
    return mgr;
}

void backend_manager_destroy(backend_manager_t *mgr) {
    if (!mgr) return;
    
    /* Stop caching thread if running */
    backend_stop_caching(mgr);
    
    /* Destroy all backends */
    for (size_t i = 0; i < mgr->num_backends; i++) {
        backend_info_t *backend = mgr->backends[i];
        if (backend) {
            if (backend->index) {
                backend_index_destroy(backend->index);
            }
            free(backend->mount_path);
            free(backend->name);
            pthread_rwlock_destroy(&backend->rwlock);
            free(backend);
        }
    }
    
    free(mgr->backends);
    global_index_destroy(mgr->global_index);
    pthread_rwlock_destroy(&mgr->backends_lock);
    free(mgr);
}

/* ============================================================================
 * Backend Registration
 * ============================================================================ */

int backend_manager_register(backend_manager_t *mgr,
                             backend_type_t type,
                             const char *mount_path,
                             const char *name,
                             uint64_t capacity_bytes,
                             uint32_t flags) {
    if (!mgr || !mount_path || !name) return -1;
    
    pthread_rwlock_wrlock(&mgr->backends_lock);
    
    /* Check capacity */
    if (mgr->num_backends >= mgr->max_backends) {
        pthread_rwlock_unlock(&mgr->backends_lock);
        return -1;
    }
    
    /* Create backend info */
    backend_info_t *backend = calloc(1, sizeof(backend_info_t));
    if (!backend) {
        pthread_rwlock_unlock(&mgr->backends_lock);
        return -1;
    }
    
    backend->id = mgr->num_backends;
    backend->type = type;
    backend->mount_path = strdup(mount_path);
    backend->name = strdup(name);
    backend->flags = flags | BACKEND_FLAG_ENABLED;
    backend->capacity_bytes = capacity_bytes;
    
    /* Set performance characteristics */
    backend->perf_factor = backend_default_perf_factor(type);
    backend->expected_latency_us = backend_expected_latency(type);
    
    /* Default watermarks */
    backend->high_watermark = 0.85;  /* 85% triggers migration */
    backend->low_watermark = 0.70;   /* 70% stops migration */
    
    /* Default migration policy */
    backend->migration_policy = MIGRATION_POLICY_HYBRID;
    backend->hotness_threshold = 0.5;
    backend->hotness_halflife_us = 3600ULL * 1000000; /* 1 hour */
    
    /* Initialize atomics */
    atomic_init(&backend->used_bytes, 0);
    atomic_init(&backend->object_count, 0);
    atomic_init(&backend->reads, 0);
    atomic_init(&backend->writes, 0);
    atomic_init(&backend->migrations_in, 0);
    atomic_init(&backend->migrations_out, 0);
    
    pthread_rwlock_init(&backend->rwlock, NULL);
    
    /* Create backend index with persistence */
    char index_path[512];
    snprintf(index_path, sizeof(index_path), "%s/.objmapper.idx", mount_path);
    backend->index = backend_index_create(backend->id, index_path, 256 * 1024);
    if (!backend->index) {
        free(backend->mount_path);
        free(backend->name);
        pthread_rwlock_destroy(&backend->rwlock);
        free(backend);
        pthread_rwlock_unlock(&mgr->backends_lock);
        return -1;
    }
    
    /* Try to load existing index */
    int loaded = backend_index_load(backend->index);
    if (loaded > 0) {
        printf("Loaded %d objects from backend %d (%s)\n", loaded, backend->id, name);
        
        /* Add all entries to global index */
        /* TODO: Iterate backend index and add to global index */
    }
    
    mgr->backends[mgr->num_backends] = backend;
    int backend_id = mgr->num_backends;
    mgr->num_backends++;
    
    pthread_rwlock_unlock(&mgr->backends_lock);
    
    printf("Registered backend %d: %s (%s) at %s, capacity=%lu GB\n",
           backend_id, name, backend_type_name(type), mount_path,
           capacity_bytes / (1024UL * 1024 * 1024));
    
    return backend_id;
}

backend_info_t *backend_manager_get_backend(backend_manager_t *mgr, int backend_id) {
    if (!mgr || backend_id < 0) return NULL;
    
    pthread_rwlock_rdlock(&mgr->backends_lock);
    
    backend_info_t *backend = NULL;
    if ((size_t)backend_id < mgr->num_backends) {
        backend = mgr->backends[backend_id];
    }
    
    pthread_rwlock_unlock(&mgr->backends_lock);
    return backend;
}

int backend_manager_set_default(backend_manager_t *mgr, int backend_id) {
    if (!mgr) return -1;
    
    backend_info_t *backend = backend_manager_get_backend(mgr, backend_id);
    if (!backend) return -1;
    
    /* Default backend must support persistent objects */
    if (backend->flags & BACKEND_FLAG_EPHEMERAL_ONLY) {
        return -1;
    }
    
    mgr->default_backend_id = backend_id;
    return 0;
}

int backend_manager_set_ephemeral(backend_manager_t *mgr, int backend_id) {
    if (!mgr) return -1;
    
    backend_info_t *backend = backend_manager_get_backend(mgr, backend_id);
    if (!backend) return -1;
    
    /* Ephemeral backend must have EPHEMERAL_ONLY flag */
    if (!(backend->flags & BACKEND_FLAG_EPHEMERAL_ONLY)) {
        return -1;
    }
    
    mgr->ephemeral_backend_id = backend_id;
    return 0;
}

int backend_manager_set_cache(backend_manager_t *mgr, int backend_id) {
    if (!mgr) return -1;
    
    backend_info_t *backend = backend_manager_get_backend(mgr, backend_id);
    if (!backend) return -1;
    
    /* Cache backend must be memory type */
    if (backend->type != BACKEND_TYPE_MEMORY) {
        return -1;
    }
    
    mgr->cache_backend_id = backend_id;
    return 0;
}

/* ============================================================================
 * Object Operations
 * ============================================================================ */

int backend_create_object(backend_manager_t *mgr,
                          const object_create_req_t *req,
                          fd_ref_t *ref_out) {
    if (!mgr || !req || !req->uri || !ref_out) return -1;
    
    /* Determine target backend */
    int backend_id = req->backend_id;
    if (backend_id < 0) {
        /* Auto-select based on ephemeral flag */
        backend_id = req->ephemeral ? mgr->ephemeral_backend_id : mgr->default_backend_id;
        if (backend_id < 0) return -1;
    }
    
    backend_info_t *backend = backend_manager_get_backend(mgr, backend_id);
    if (!backend || !(backend->flags & BACKEND_FLAG_ENABLED)) {
        return -1;
    }
    
    /* Security check: ephemeral objects only on ephemeral backends */
    if (req->ephemeral && !(backend->flags & BACKEND_FLAG_EPHEMERAL_ONLY)) {
        return -1;
    }
    
    pthread_rwlock_rdlock(&backend->rwlock);
    
    /* Build filesystem path */
    char fs_path[1024];
    snprintf(fs_path, sizeof(fs_path), "%s%s", backend->mount_path, req->uri);
    
    /* Create parent directories */
    char dir_path[1024];
    strncpy(dir_path, fs_path, sizeof(dir_path) - 1);
    dir_path[sizeof(dir_path) - 1] = '\0';
    char *last_slash = strrchr(dir_path, '/');
    if (last_slash) {
        *last_slash = '\0';
        if (mkdir_p(dir_path) < 0) {
            pthread_rwlock_unlock(&backend->rwlock);
            return -1;
        }
    }
    
    /* Create file */
    int fd = open(fs_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        pthread_rwlock_unlock(&backend->rwlock);
        return -1;
    }
    
    /* Create index entry */
    index_entry_t *entry = index_entry_create(req->uri, backend_id, fs_path);
    if (!entry) {
        close(fd);
        unlink(fs_path);
        pthread_rwlock_unlock(&backend->rwlock);
        return -1;
    }
    
    entry->size_bytes = 0;
    entry->flags = req->flags;
    if (req->ephemeral) {
        entry->flags |= INDEX_FLAG_EPHEMERAL;
    } else {
        entry->flags |= INDEX_FLAG_PERSISTENT;
    }
    
    /* Insert into global index */
    if (global_index_insert(mgr->global_index, entry) < 0) {
        index_entry_put(entry);
        close(fd);
        unlink(fs_path);
        pthread_rwlock_unlock(&backend->rwlock);
        return -1;
    }
    
    /* Insert into backend index */
    backend_index_insert(backend->index, entry);
    
    /* Update statistics */
    atomic_fetch_add(&backend->object_count, 1);
    atomic_fetch_add(&backend->writes, 1);
    atomic_fetch_add(&mgr->total_objects, 1);
    
    /* Store FD in entry */
    atomic_store(&entry->fd, fd);
    atomic_store(&entry->fd_refcount, 1);
    
    /* Return reference (increment entry refcount) */
    index_entry_get(entry);
    ref_out->entry = entry;
    ref_out->fd = fd;
    ref_out->generation = 0;
    
    pthread_rwlock_unlock(&backend->rwlock);
    
    return 0;
}

int backend_get_object(backend_manager_t *mgr,
                       const char *uri,
                       fd_ref_t *ref_out) {
    if (!mgr || !uri || !ref_out) return -1;
    
    /* Fast path: lookup in global index */
    int ret = global_index_lookup(mgr->global_index, uri, ref_out);
    if (ret == 0) {
        /* Update access statistics */
        index_entry_record_access(ref_out->entry);
        
        backend_info_t *backend = backend_manager_get_backend(mgr, ref_out->entry->backend_id);
        if (backend) {
            atomic_fetch_add(&backend->reads, 1);
        }
        
        return 0;
    }
    
    /* Not found */
    return -1;
}

int backend_delete_object(backend_manager_t *mgr, const char *uri) {
    if (!mgr || !uri) return -1;
    
    /* Lookup object */
    fd_ref_t ref;
    if (global_index_lookup(mgr->global_index, uri, &ref) < 0) {
        return -1;  /* Not found */
    }
    
    backend_info_t *backend = backend_manager_get_backend(mgr, ref.entry->backend_id);
    if (!backend) {
        fd_ref_release(&ref);
        return -1;
    }
    
    pthread_rwlock_wrlock(&backend->rwlock);
    
    /* Delete from filesystem */
    unlink(ref.entry->backend_path);
    
    /* Update statistics */
    atomic_fetch_sub(&backend->object_count, 1);
    atomic_fetch_sub(&backend->used_bytes, ref.entry->size_bytes);
    atomic_fetch_sub(&mgr->total_objects, 1);
    atomic_fetch_sub(&mgr->total_bytes, ref.entry->size_bytes);
    
    /* Remove from indexes */
    backend_index_remove(backend->index, uri);
    global_index_remove(mgr->global_index, uri);
    
    pthread_rwlock_unlock(&backend->rwlock);
    
    fd_ref_release(&ref);
    
    return 0;
}

int backend_manager_scan(backend_manager_t *mgr, int backend_id) {
    if (!mgr) return -1;
    
    backend_info_t *backend = backend_manager_get_backend(mgr, backend_id);
    if (!backend || !(backend->flags & BACKEND_FLAG_ENABLED)) {
        return -1;
    }
    
    /* Helper function to recursively scan directory */
    int count = 0;
    size_t mount_path_len = strlen(backend->mount_path);
    
    int scan_dir_recursive(const char *dir_path) {
        DIR *dir = opendir(dir_path);
        if (!dir) return 0;
        
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            /* Skip . and .. */
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            
            /* Build full path */
            char full_path[2048];
            snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
            
            struct stat st;
            if (stat(full_path, &st) < 0) {
                continue;
            }
            
            if (S_ISDIR(st.st_mode)) {
                /* Recursively scan subdirectory */
                scan_dir_recursive(full_path);
            } else if (S_ISREG(st.st_mode)) {
                /* Regular file - create index entry */
                /* URI is the path relative to mount point */
                const char *uri = full_path + mount_path_len;
                
                /* Create index entry */
                index_entry_t *idx_entry = index_entry_create(uri, backend_id, full_path);
                if (idx_entry) {
                    idx_entry->size_bytes = st.st_size;
                    idx_entry->flags = (backend->flags & BACKEND_FLAG_EPHEMERAL_ONLY) ? 
                                       INDEX_FLAG_EPHEMERAL : INDEX_FLAG_PERSISTENT;
                    
                    /* Insert into global index */
                    if (global_index_insert(mgr->global_index, idx_entry) >= 0) {
                        /* Also insert into backend index */
                        backend_index_insert(backend->index, idx_entry);
                        count++;
                        
                        /* Update statistics */
                        atomic_fetch_add(&backend->object_count, 1);
                        atomic_fetch_add(&backend->used_bytes, st.st_size);
                        atomic_fetch_add(&mgr->total_objects, 1);
                        atomic_fetch_add(&mgr->total_bytes, st.st_size);
                    }
                    
                    index_entry_put(idx_entry);  /* Release our reference */
                }
            }
        }
        
        closedir(dir);
        return 0;
    }
    
    pthread_rwlock_wrlock(&backend->rwlock);
    scan_dir_recursive(backend->mount_path);
    pthread_rwlock_unlock(&backend->rwlock);
    
    return count;
}

int backend_get_metadata(backend_manager_t *mgr,
                         const char *uri,
                         object_metadata_t *metadata_out) {
    if (!mgr || !uri || !metadata_out) return -1;
    
    fd_ref_t ref;
    if (global_index_lookup(mgr->global_index, uri, &ref) < 0) {
        return -1;
    }
    
    index_entry_t *entry = ref.entry;
    
    metadata_out->uri = strdup(entry->uri);
    metadata_out->backend_id = entry->backend_id;
    metadata_out->fs_path = strdup(entry->backend_path);
    metadata_out->size_bytes = entry->size_bytes;
    metadata_out->mtime = entry->mtime;
    metadata_out->flags = entry->flags;
    metadata_out->hotness = entry->hotness_score;
    metadata_out->access_count = atomic_load(&entry->access_count);
    index_entry_get_payload(entry, &metadata_out->payload);
    metadata_out->has_payload = (metadata_out->payload.variant_count > 0);
    
    fd_ref_release(&ref);
    
    return 0;
}

int backend_set_payload_metadata(backend_manager_t *mgr,
                                 const char *uri,
                                 const objm_payload_descriptor_t *payload) {
    if (!mgr || !uri || !payload) {
        return -1;
    }

    char error_buf[128];
    if (objm_payload_descriptor_validate(payload, error_buf, sizeof(error_buf)) < 0) {
        fprintf(stderr, "payload metadata invalid for %s: %s\n", uri, error_buf);
        return -1;
    }

    fd_ref_t ref;
    if (global_index_lookup(mgr->global_index, uri, &ref) < 0) {
        return -1;
    }

    index_entry_set_payload(ref.entry, payload);

    backend_info_t *backend = backend_manager_get_backend(mgr, ref.entry->backend_id);
    backend_mark_index_dirty(backend);

    fd_ref_release(&ref);
    return 0;
}

int backend_get_payload_metadata(backend_manager_t *mgr,
                                 const char *uri,
                                 objm_payload_descriptor_t *payload_out) {
    if (!mgr || !uri || !payload_out) {
        return -1;
    }

    fd_ref_t ref;
    if (global_index_lookup(mgr->global_index, uri, &ref) < 0) {
        return -1;
    }

    index_entry_get_payload(ref.entry, payload_out);

    fd_ref_release(&ref);
    return 0;
}

int backend_update_size(backend_manager_t *mgr,
                        const char *uri,
                        size_t new_size) {
    if (!mgr || !uri) return -1;
    
    fd_ref_t ref;
    if (global_index_lookup(mgr->global_index, uri, &ref) < 0) {
        return -1;
    }
    
    backend_info_t *backend = backend_manager_get_backend(mgr, ref.entry->backend_id);
    if (!backend) {
        fd_ref_release(&ref);
        return -1;
    }
    
    /* Update size difference */
    size_t old_size = ref.entry->size_bytes;
    bool mark_dirty = (new_size != old_size);
    bool seeded_payload = false;
    ref.entry->size_bytes = new_size;
    
    if (new_size > old_size) {
        size_t delta = new_size - old_size;
        atomic_fetch_add(&backend->used_bytes, delta);
        atomic_fetch_add(&mgr->total_bytes, delta);
    } else if (new_size < old_size) {
        size_t delta = old_size - new_size;
        atomic_fetch_sub(&backend->used_bytes, delta);
        atomic_fetch_sub(&mgr->total_bytes, delta);
    }

    if (new_size > 0 && ref.entry->payload.variant_count == 0) {
        index_entry_seed_identity_payload(ref.entry, new_size);
        seeded_payload = true;
    }

    if (mark_dirty || seeded_payload) {
        backend_mark_index_dirty(backend);
    }
    
    fd_ref_release(&ref);
    
    return 0;
}

void object_metadata_free(object_metadata_t *metadata) {
    if (!metadata) return;
    free(metadata->uri);
    free(metadata->fs_path);
    memset(metadata, 0, sizeof(*metadata));
}

/* ============================================================================
 * Migration Implementation
 * ============================================================================ */

int backend_migrate_object(backend_manager_t *mgr,
                           const char *uri,
                           int target_backend_id) {
    if (!mgr || !uri) return -1;
    
    /* Get source object */
    fd_ref_t src_ref;
    if (global_index_lookup(mgr->global_index, uri, &src_ref) < 0) {
        return -1;
    }
    
    index_entry_t *src_entry = src_ref.entry;
    int src_backend_id = src_entry->backend_id;
    
    /* Can't migrate to same backend */
    if (src_backend_id == target_backend_id) {
        fd_ref_release(&src_ref);
        return -1;
    }
    
    /* Security check: ephemeral objects cannot migrate to persistent backends */
    if (src_entry->flags & INDEX_FLAG_EPHEMERAL) {
        backend_info_t *target = backend_manager_get_backend(mgr, target_backend_id);
        if (!target || !(target->flags & BACKEND_FLAG_EPHEMERAL_ONLY)) {
            fd_ref_release(&src_ref);
            return -1;
        }
    }
    
    backend_info_t *src_backend = backend_manager_get_backend(mgr, src_backend_id);
    backend_info_t *dst_backend = backend_manager_get_backend(mgr, target_backend_id);
    
    if (!src_backend || !dst_backend) {
        fd_ref_release(&src_ref);
        return -1;
    }
    
    /* Check migration flags */
    if (!(src_backend->flags & BACKEND_FLAG_MIGRATION_SRC) ||
        !(dst_backend->flags & BACKEND_FLAG_MIGRATION_DST)) {
        fd_ref_release(&src_ref);
        return -1;
    }
    
    /* Build destination path */
    char dst_path[1024];
    snprintf(dst_path, sizeof(dst_path), "%s%s", dst_backend->mount_path, uri);
    
    /* Create destination file */
    int dst_fd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst_fd < 0) {
        fd_ref_release(&src_ref);
        return -1;
    }
    
    /* Copy data using sendfile */
    int src_fd = fd_ref_acquire(&src_ref);
    if (src_fd < 0) {
        close(dst_fd);
        unlink(dst_path);
        fd_ref_release(&src_ref);
        return -1;
    }
    
    off_t offset = 0;
    ssize_t copied = sendfile(dst_fd, src_fd, &offset, src_entry->size_bytes);
    
    fd_ref_release(&src_ref);
    close(dst_fd);
    
    if (copied != (ssize_t)src_entry->size_bytes) {
        unlink(dst_path);
        return -1;
    }
    
    /* Update entry */
    pthread_rwlock_wrlock(&src_backend->rwlock);
    pthread_rwlock_wrlock(&dst_backend->rwlock);
    
    /* Remove from source backend index */
    backend_index_remove(src_backend->index, uri);
    atomic_fetch_sub(&src_backend->object_count, 1);
    atomic_fetch_sub(&src_backend->used_bytes, src_entry->size_bytes);
    atomic_fetch_add(&src_backend->migrations_out, 1);
    
    /* Update entry path and backend */
    free(src_entry->backend_path);
    src_entry->backend_path = strdup(dst_path);
    src_entry->backend_id = target_backend_id;
    
    /* Update global index */
    global_index_update_backend(mgr->global_index, uri, target_backend_id, dst_path);
    
    /* Add to destination backend index */
    backend_index_insert(dst_backend->index, src_entry);
    atomic_fetch_add(&dst_backend->object_count, 1);
    atomic_fetch_add(&dst_backend->used_bytes, src_entry->size_bytes);
    atomic_fetch_add(&dst_backend->migrations_in, 1);
    
    pthread_rwlock_unlock(&dst_backend->rwlock);
    pthread_rwlock_unlock(&src_backend->rwlock);
    
    /* Delete source file */
    unlink(src_entry->backend_path);
    
    return 0;
}

/* ============================================================================
 * Caching Implementation (Local Migration)
 * ============================================================================ */

/* Caching thread function */
static void *cache_thread_func(void *arg) {
    backend_manager_t *mgr = (backend_manager_t *)arg;
    
    while (atomic_load(&mgr->cache_running)) {
        /* Check if we have a cache backend */
        if (mgr->cache_backend_id < 0) {
            usleep(mgr->cache_check_interval_us);
            continue;
        }
        
        backend_info_t *cache = backend_manager_get_backend(mgr, mgr->cache_backend_id);
        if (!cache) {
            usleep(mgr->cache_check_interval_us);
            continue;
        }
        
        /* Calculate cache utilization */
        uint64_t used = atomic_load(&cache->used_bytes);
        double utilization = (double)used / cache->capacity_bytes;
        
        /* If cache is full, evict cold objects */
        if (utilization > cache->high_watermark) {
            /* TODO: Find coldest objects in cache and evict them */
        }
        
        /* Scan persistent backends for hot objects to cache */
        if (utilization < cache->low_watermark) {
            for (size_t i = 0; i < mgr->num_backends; i++) {
                backend_info_t *backend = mgr->backends[i];
                if (!backend || backend->id == mgr->cache_backend_id) continue;
                if (backend->flags & BACKEND_FLAG_EPHEMERAL_ONLY) continue;
                
                /* TODO: Find hot objects and cache them */
            }
        }
        
        usleep(mgr->cache_check_interval_us);
    }
    
    return NULL;
}

int backend_start_caching(backend_manager_t *mgr, 
                         uint64_t check_interval_us,
                         double cache_threshold) {
    if (!mgr) return -1;
    
    if (atomic_load(&mgr->cache_running)) {
        return 0;  /* Already running */
    }
    
    mgr->cache_check_interval_us = check_interval_us;
    mgr->cache_threshold = cache_threshold;
    atomic_store(&mgr->cache_running, 1);
    
    if (pthread_create(&mgr->cache_thread, NULL, cache_thread_func, mgr) != 0) {
        atomic_store(&mgr->cache_running, 0);
        return -1;
    }
    
    return 0;
}

void backend_stop_caching(backend_manager_t *mgr) {
    if (!mgr) return;
    
    if (atomic_load(&mgr->cache_running)) {
        atomic_store(&mgr->cache_running, 0);
        pthread_join(mgr->cache_thread, NULL);
    }
}

int backend_cache_object(backend_manager_t *mgr, const char *uri) {
    if (!mgr || !uri) return -1;
    if (mgr->cache_backend_id < 0) return -1;
    
    /* Get object info */
    fd_ref_t ref;
    if (global_index_lookup(mgr->global_index, uri, &ref) < 0) {
        return -1;
    }
    
    int current_backend = ref.entry->backend_id;
    fd_ref_release(&ref);
    
    /* Already in cache? */
    if (current_backend == mgr->cache_backend_id) {
        return 0;
    }
    
    /* Migrate to cache */
    return backend_migrate_object(mgr, uri, mgr->cache_backend_id);
}

int backend_evict_object(backend_manager_t *mgr, const char *uri) {
    if (!mgr || !uri) return -1;
    if (mgr->default_backend_id < 0) return -1;
    
    /* Get object info */
    fd_ref_t ref;
    if (global_index_lookup(mgr->global_index, uri, &ref) < 0) {
        return -1;
    }
    
    int current_backend = ref.entry->backend_id;
    fd_ref_release(&ref);
    
    /* Not in cache? */
    if (current_backend != mgr->cache_backend_id) {
        return 0;
    }
    
    /* Migrate back to default backend */
    return backend_migrate_object(mgr, uri, mgr->default_backend_id);
}

/* ============================================================================
 * Query API for External Controllers
 * ============================================================================ */

int backend_get_status(backend_manager_t *mgr,
                       int backend_id,
                       uint64_t *capacity_out,
                       uint64_t *used_out,
                       size_t *objects_out,
                       double *utilization_out) {
    backend_info_t *backend = backend_manager_get_backend(mgr, backend_id);
    if (!backend) return -1;
    
    uint64_t used = atomic_load(&backend->used_bytes);
    
    if (capacity_out) *capacity_out = backend->capacity_bytes;
    if (used_out) *used_out = used;
    if (objects_out) *objects_out = atomic_load(&backend->object_count);
    if (utilization_out) *utilization_out = (double)used / backend->capacity_bytes;
    
    return 0;
}

int backend_list_objects(backend_manager_t *mgr,
                        int backend_id,
                        char ***uris_out,
                        size_t *count_out) {
    if (!mgr || !uris_out || !count_out) return -1;
    
    backend_info_t *backend = backend_manager_get_backend(mgr, backend_id);
    if (!backend || !backend->index) return -1;
    
    /* TODO: Implement iteration over backend index */
    /* For now, return empty list */
    *uris_out = NULL;
    *count_out = 0;
    
    return 0;
}

int backend_get_hotness_map(backend_manager_t *mgr,
                            int backend_id,
                            char ***uris_out,
                            double **scores_out,
                            size_t *count_out) {
    if (!mgr || !uris_out || !scores_out || !count_out) return -1;
    
    backend_info_t *backend = backend_manager_get_backend(mgr, backend_id);
    if (!backend || !backend->index) return -1;
    
    /* TODO: Implement iteration with hotness calculation */
    /* For now, return empty list */
    *uris_out = NULL;
    *scores_out = NULL;
    *count_out = 0;
    
    return 0;
}

int backend_get_index_stats(backend_manager_t *mgr, index_stats_t *stats_out) {
    if (!mgr || !stats_out) return -1;
    
    global_index_get_stats(mgr->global_index, stats_out);
    return 0;
}

/* ============================================================================
 * Backend Management
 * ============================================================================ */

int backend_set_enabled(backend_manager_t *mgr, int backend_id, bool enabled) {
    backend_info_t *backend = backend_manager_get_backend(mgr, backend_id);
    if (!backend) return -1;
    
    pthread_rwlock_wrlock(&backend->rwlock);
    
    if (enabled) {
        backend->flags |= BACKEND_FLAG_ENABLED;
    } else {
        backend->flags &= ~BACKEND_FLAG_ENABLED;
    }
    
    pthread_rwlock_unlock(&backend->rwlock);
    
    return 0;
}

int backend_set_watermarks(backend_manager_t *mgr,
                           int backend_id,
                           double high_watermark,
                           double low_watermark) {
    if (high_watermark < 0.0 || high_watermark > 1.0 ||
        low_watermark < 0.0 || low_watermark > 1.0 ||
        low_watermark >= high_watermark) {
        return -1;
    }
    
    backend_info_t *backend = backend_manager_get_backend(mgr, backend_id);
    if (!backend) return -1;
    
    pthread_rwlock_wrlock(&backend->rwlock);
    backend->high_watermark = high_watermark;
    backend->low_watermark = low_watermark;
    pthread_rwlock_unlock(&backend->rwlock);
    
    return 0;
}

int backend_set_migration_policy(backend_manager_t *mgr,
                                 int backend_id,
                                 migration_policy_t policy,
                                 double hotness_threshold) {
    backend_info_t *backend = backend_manager_get_backend(mgr, backend_id);
    if (!backend) return -1;
    
    pthread_rwlock_wrlock(&backend->rwlock);
    backend->migration_policy = policy;
    backend->hotness_threshold = hotness_threshold;
    pthread_rwlock_unlock(&backend->rwlock);
    
    return 0;
}

int backend_get_stats(backend_manager_t *mgr,
                      int backend_id,
                      size_t *reads_out,
                      size_t *writes_out,
                      size_t *migrations_in_out,
                      size_t *migrations_out_out) {
    backend_info_t *backend = backend_manager_get_backend(mgr, backend_id);
    if (!backend) return -1;
    
    if (reads_out) *reads_out = atomic_load(&backend->reads);
    if (writes_out) *writes_out = atomic_load(&backend->writes);
    if (migrations_in_out) *migrations_in_out = atomic_load(&backend->migrations_in);
    if (migrations_out_out) *migrations_out_out = atomic_load(&backend->migrations_out);
    
    return 0;
}
