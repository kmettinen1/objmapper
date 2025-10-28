/**
 * @file index.c
 * @brief RCU-based hash index implementation
 */

#include "index.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <math.h>
#include <dirent.h>
#include <endian.h>

/* ============================================================================
 * Internal helpers
 * ============================================================================ */

/**
 * Get monotonic timestamp in microseconds
 */
static uint64_t get_monotonic_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/**
 * FNV-1a hash
 */
uint64_t index_hash_string(const char *str) {
    uint64_t hash = 14695981039346656037ULL;
    while (*str) {
        hash ^= (uint64_t)(unsigned char)*str++;
        hash *= 1099511628211ULL;
    }
    return hash;
}

/**
 * Round up to next power of 2
 */
size_t index_next_power_of_2(size_t n) {
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n |= n >> 32;
    n++;
    return n;
}

/* ============================================================================
 * Index Entry Implementation
 * ============================================================================ */

index_entry_t *index_entry_create(const char *uri, uint32_t backend_id,
                                  const char *backend_path) {
    index_entry_t *entry = calloc(1, sizeof(*entry));
    if (!entry) return NULL;
    
    entry->uri = strdup(uri);
    entry->backend_path = strdup(backend_path);
    
    if (!entry->uri || !entry->backend_path) {
        free(entry->uri);
        free(entry->backend_path);
        free(entry);
        return NULL;
    }
    
    entry->uri_hash = index_hash_string(uri);
    entry->backend_id = backend_id;
    
    atomic_init(&entry->fd, -1);
    atomic_init(&entry->fd_refcount, 0);
    atomic_init(&entry->fd_generation, 0);
    atomic_init(&entry->access_count, 0);
    atomic_init(&entry->last_access, 0);
    atomic_init(&entry->entry_refcount, 1);  /* Start with 1 reference */
    atomic_init(&entry->next, (uintptr_t)NULL);
    
    return entry;
}

void index_entry_get(index_entry_t *entry) {
    if (entry) {
        atomic_fetch_add(&entry->entry_refcount, 1);
    }
}

void index_entry_put(index_entry_t *entry) {
    if (!entry) return;
    
    int prev = atomic_fetch_sub(&entry->entry_refcount, 1);
    if (prev == 1) {
        /* Last reference - free entry */
        int fd = atomic_load(&entry->fd);
        if (fd >= 0) {
            close(fd);
        }
        free(entry->uri);
        free(entry->backend_path);
        free(entry);
    }
}

int index_entry_open_fd(index_entry_t *entry) {
    if (!entry) return -1;
    
    /* Check if already open */
    int fd = atomic_load(&entry->fd);
    if (fd >= 0) {
        return 0;  /* Already open */
    }
    
    /* Open file */
    fd = open(entry->backend_path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    
    /* Store FD atomically */
    int expected = -1;
    if (atomic_compare_exchange_strong(&entry->fd, &expected, fd)) {
        /* We won the race - our FD is stored */
        return 0;
    } else {
        /* Someone else opened it first */
        close(fd);
        return 0;
    }
}

void index_entry_close_fd(index_entry_t *entry) {
    if (!entry) return;
    
    /* Only close if no references */
    int refcount = atomic_load(&entry->fd_refcount);
    if (refcount > 0) {
        return;  /* Still in use */
    }
    
    /* Close FD atomically */
    int fd = atomic_exchange(&entry->fd, -1);
    if (fd >= 0) {
        close(fd);
        atomic_fetch_add(&entry->fd_generation, 1);
    }
}

void index_entry_record_access(index_entry_t *entry) {
    if (!entry) return;
    
    atomic_fetch_add(&entry->access_count, 1);
    atomic_store(&entry->last_access, get_monotonic_us());
}

float index_calculate_hotness(const index_entry_t *entry, uint64_t current_time,
                              uint32_t decay_halflife) {
    if (!entry) return 0.0f;
    
    uint64_t last_access = atomic_load(&entry->last_access);
    if (last_access == 0) return 0.0f;
    
    /* Time since last access in seconds */
    uint64_t age_us = current_time - last_access;
    double age_secs = age_us / 1000000.0;
    
    /* Exponential decay: score = exp(-0.693 * age / halflife) */
    double time_factor = exp(-0.693 * age_secs / decay_halflife);
    
    /* Access frequency component */
    uint64_t access_count = atomic_load(&entry->access_count);
    double access_factor = (double)access_count / 1000.0;
    if (access_factor > 1.0) access_factor = 1.0;
    
    /* Combined score: 70% time-based, 30% frequency-based */
    float hotness = 0.7f * time_factor + 0.3f * access_factor;
    
    return hotness > 1.0f ? 1.0f : hotness;
}

/* ============================================================================
 * FD Reference Implementation
 * ============================================================================ */

int fd_ref_acquire(fd_ref_t *fd_ref) {
    if (!fd_ref || !fd_ref->entry) return -1;
    
    index_entry_t *entry = fd_ref->entry;
    
    /* Fast path: FD already open */
    int fd = atomic_load(&entry->fd);
    if (fd >= 0) {
        /* Increment refcount */
        atomic_fetch_add(&entry->fd_refcount, 1);
        
        /* Verify FD is still valid */
        int current_fd = atomic_load(&entry->fd);
        if (current_fd >= 0) {
            fd_ref->fd = current_fd;
            fd_ref->generation = atomic_load(&entry->fd_generation);
            return current_fd;
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

void fd_ref_release(fd_ref_t *fd_ref) {
    if (!fd_ref || !fd_ref->entry) return;
    
    index_entry_t *entry = fd_ref->entry;
    
    /* Close FD if we have one */
    if (fd_ref->fd >= 0) {
        close(fd_ref->fd);
        /* Decrement FD refcount */
        atomic_fetch_sub(&entry->fd_refcount, 1);
    }
    
    /* Release entry reference */
    index_entry_put(entry);
    
    fd_ref->entry = NULL;
    fd_ref->fd = -1;
}

int fd_ref_dup(fd_ref_t *fd_ref) {
    if (!fd_ref || fd_ref->fd < 0) return -1;
    
    return dup(fd_ref->fd);
}

/* ============================================================================
 * Global Index Implementation
 * ============================================================================ */

global_index_t *global_index_create(size_t num_buckets, size_t max_open_fds) {
    global_index_t *idx = calloc(1, sizeof(*idx));
    if (!idx) return NULL;
    
    /* Round to power of 2 */
    idx->num_buckets = index_next_power_of_2(num_buckets);
    idx->max_open_fds = max_open_fds;
    
    /* Allocate buckets */
    idx->buckets = calloc(idx->num_buckets, sizeof(atomic_uintptr_t));
    if (!idx->buckets) {
        free(idx);
        return NULL;
    }
    
    /* Initialize atomics */
    for (size_t i = 0; i < idx->num_buckets; i++) {
        atomic_init(&idx->buckets[i], (uintptr_t)NULL);
    }
    
    atomic_init(&idx->num_entries, 0);
    atomic_init(&idx->num_open_fds, 0);
    atomic_init(&idx->stat_lookups, 0);
    atomic_init(&idx->stat_hits, 0);
    atomic_init(&idx->stat_misses, 0);
    atomic_init(&idx->stat_fd_cache_hits, 0);
    atomic_init(&idx->stat_fd_opens, 0);
    atomic_init(&idx->stat_fd_closes, 0);
    atomic_init(&idx->stat_fd_evictions, 0);
    
    pthread_mutex_init(&idx->write_lock, NULL);
    pthread_mutex_init(&idx->lru_lock, NULL);
    
    return idx;
}

void global_index_destroy(global_index_t *idx) {
    if (!idx) return;
    
    /* Close all FDs and free entries */
    for (size_t i = 0; i < idx->num_buckets; i++) {
        index_entry_t *entry = (index_entry_t *)atomic_load(&idx->buckets[i]);
        while (entry) {
            index_entry_t *next = (index_entry_t *)atomic_load(&entry->next);
            index_entry_put(entry);
            entry = next;
        }
    }
    
    free(idx->buckets);
    pthread_mutex_destroy(&idx->write_lock);
    pthread_mutex_destroy(&idx->lru_lock);
    free(idx);
}

int global_index_lookup(global_index_t *idx, const char *uri, fd_ref_t *fd_ref) {
    if (!idx || !uri || !fd_ref) return -1;
    
    atomic_fetch_add(&idx->stat_lookups, 1);
    
    uint64_t hash = index_hash_string(uri);
    size_t bucket = hash & (idx->num_buckets - 1);
    
    /* Lock-free read using atomic load */
    index_entry_t *entry = (index_entry_t *)atomic_load(&idx->buckets[bucket]);
    
    while (entry) {
        if (entry->uri_hash == hash && strcmp(entry->uri, uri) == 0) {
            /* Found! Acquire entry reference */
            index_entry_get(entry);
            
            /* Open FD if not already open */
            int fd = -1;
            if (entry->backend_path) {
                fd = open(entry->backend_path, O_RDWR);
                if (fd < 0) {
                    /* Try read-only if read-write fails */
                    fd = open(entry->backend_path, O_RDONLY);
                }
            }
            
            /* Prepare FD reference */
            fd_ref->entry = entry;
            fd_ref->fd = fd;
            fd_ref->generation = 0;
            
            /* Record access */
            index_entry_record_access(entry);
            
            /* Increment FD refcount if we opened it */
            if (fd >= 0) {
                atomic_fetch_add(&entry->fd_refcount, 1);
            }
            
            atomic_fetch_add(&idx->stat_hits, 1);
            return 0;
        }
        
        entry = (index_entry_t *)atomic_load(&entry->next);
    }
    
    atomic_fetch_add(&idx->stat_misses, 1);
    return -1;
}

int global_index_insert(global_index_t *idx, index_entry_t *entry) {
    if (!idx || !entry) return -1;
    
    /* Serialize writers */
    pthread_mutex_lock(&idx->write_lock);
    
    size_t bucket = entry->uri_hash & (idx->num_buckets - 1);
    
    /* Check for duplicate */
    index_entry_t *existing = (index_entry_t *)atomic_load(&idx->buckets[bucket]);
    while (existing) {
        if (existing->uri_hash == entry->uri_hash && 
            strcmp(existing->uri, entry->uri) == 0) {
            pthread_mutex_unlock(&idx->write_lock);
            return -1;  /* Duplicate */
        }
        existing = (index_entry_t *)atomic_load(&existing->next);
    }
    
    /* Insert at head of collision chain */
    index_entry_t *old_head = (index_entry_t *)atomic_load(&idx->buckets[bucket]);
    atomic_store(&entry->next, (uintptr_t)old_head);
    
    /* Atomic pointer update (becomes visible to readers) */
    atomic_store(&idx->buckets[bucket], (uintptr_t)entry);
    
    atomic_fetch_add(&idx->num_entries, 1);
    
    pthread_mutex_unlock(&idx->write_lock);
    return 0;
}

int global_index_remove(global_index_t *idx, const char *uri) {
    if (!idx || !uri) return -1;
    
    pthread_mutex_lock(&idx->write_lock);
    
    uint64_t hash = index_hash_string(uri);
    size_t bucket = hash & (idx->num_buckets - 1);
    
    atomic_uintptr_t *prev_next = &idx->buckets[bucket];
    index_entry_t *entry = (index_entry_t *)atomic_load(&idx->buckets[bucket]);
    
    while (entry) {
        if (entry->uri_hash == hash && strcmp(entry->uri, uri) == 0) {
            /* Unlink from chain */
            index_entry_t *next = (index_entry_t *)atomic_load(&entry->next);
            atomic_store(prev_next, (uintptr_t)next);
            
            /* Close FD if open */
            int fd = atomic_exchange(&entry->fd, -1);
            if (fd >= 0) {
                close(fd);
                atomic_fetch_sub(&idx->num_open_fds, 1);
            }
            
            /* Release entry (will be freed when refcount reaches 0) */
            index_entry_put(entry);
            
            atomic_fetch_sub(&idx->num_entries, 1);
            
            pthread_mutex_unlock(&idx->write_lock);
            return 0;
        }
        
        prev_next = &entry->next;
        entry = (index_entry_t *)atomic_load(&entry->next);
    }
    
    pthread_mutex_unlock(&idx->write_lock);
    return -1;
}

int global_index_update_backend(global_index_t *idx, const char *uri,
                                uint32_t backend_id, const char *backend_path) {
    if (!idx || !uri || !backend_path) return -1;
    
    /* Lookup entry */
    fd_ref_t ref;
    if (global_index_lookup(idx, uri, &ref) < 0) {
        return -1;
    }
    
    index_entry_t *entry = ref.entry;
    
    /* Close old FD */
    int fd = atomic_exchange(&entry->fd, -1);
    if (fd >= 0) {
        close(fd);
        atomic_fetch_sub(&idx->num_open_fds, 1);
        atomic_fetch_add(&entry->fd_generation, 1);
    }
    
    /* Update backend info */
    free(entry->backend_path);
    entry->backend_path = strdup(backend_path);
    entry->backend_id = backend_id;
    
    fd_ref_release(&ref);
    
    return 0;
}

void global_index_get_stats(global_index_t *idx, index_stats_t *stats) {
    if (!idx || !stats) return;
    
    stats->num_entries = atomic_load(&idx->num_entries);
    stats->num_open_fds = atomic_load(&idx->num_open_fds);
    stats->lookups = atomic_load(&idx->stat_lookups);
    stats->hits = atomic_load(&idx->stat_hits);
    stats->misses = atomic_load(&idx->stat_misses);
    stats->fd_cache_hits = atomic_load(&idx->stat_fd_cache_hits);
    stats->fd_opens = atomic_load(&idx->stat_fd_opens);
    stats->fd_closes = atomic_load(&idx->stat_fd_closes);
    stats->fd_evictions = atomic_load(&idx->stat_fd_evictions);
    
    stats->hit_rate = stats->lookups > 0 ? 
        (double)stats->hits / stats->lookups : 0.0;
    stats->fd_cache_rate = stats->fd_opens > 0 ?
        (double)stats->fd_cache_hits / stats->fd_opens : 0.0;
}

/* ============================================================================
 * Backend Index Implementation
 * ============================================================================ */

backend_index_t *backend_index_create(uint32_t backend_id,
                                      const char *index_file_path,
                                      size_t num_buckets) {
    backend_index_t *idx = calloc(1, sizeof(*idx));
    if (!idx) return NULL;
    
    idx->backend_id = backend_id;
    idx->num_buckets = index_next_power_of_2(num_buckets);
    
    if (index_file_path) {
        idx->index_file_path = strdup(index_file_path);
        idx->persist_enabled = 1;
    }
    
    /* Allocate buckets */
    idx->buckets = calloc(idx->num_buckets, sizeof(atomic_uintptr_t));
    if (!idx->buckets) {
        free(idx->index_file_path);
        free(idx);
        return NULL;
    }
    
    for (size_t i = 0; i < idx->num_buckets; i++) {
        atomic_init(&idx->buckets[i], (uintptr_t)NULL);
    }
    
    atomic_init(&idx->num_entries, 0);
    atomic_init(&idx->dirty, 0);
    atomic_init(&idx->stat_lookups, 0);
    atomic_init(&idx->stat_hits, 0);
    
    pthread_mutex_init(&idx->write_lock, NULL);
    
    return idx;
}

void backend_index_destroy(backend_index_t *idx) {
    if (!idx) return;
    
    /* Free all entries */
    for (size_t i = 0; i < idx->num_buckets; i++) {
        index_entry_t *entry = (index_entry_t *)atomic_load(&idx->buckets[i]);
        while (entry) {
            index_entry_t *next = (index_entry_t *)atomic_load(&entry->next);
            index_entry_put(entry);
            entry = next;
        }
    }
    
    free(idx->buckets);
    free(idx->index_file_path);
    pthread_mutex_destroy(&idx->write_lock);
    free(idx);
}

int backend_index_insert(backend_index_t *idx, index_entry_t *entry) {
    if (!idx || !entry) return -1;
    
    pthread_mutex_lock(&idx->write_lock);
    
    size_t bucket = entry->uri_hash & (idx->num_buckets - 1);
    
    /* Acquire reference for backend index */
    index_entry_get(entry);
    
    /* Insert at head */
    index_entry_t *old_head = (index_entry_t *)atomic_load(&idx->buckets[bucket]);
    atomic_store(&entry->next, (uintptr_t)old_head);
    atomic_store(&idx->buckets[bucket], (uintptr_t)entry);
    
    atomic_fetch_add(&idx->num_entries, 1);
    atomic_store(&idx->dirty, 1);
    
    pthread_mutex_unlock(&idx->write_lock);
    return 0;
}

index_entry_t *backend_index_lookup(backend_index_t *idx, const char *uri) {
    if (!idx || !uri) return NULL;
    
    atomic_fetch_add(&idx->stat_lookups, 1);
    
    uint64_t hash = index_hash_string(uri);
    size_t bucket = hash & (idx->num_buckets - 1);
    
    index_entry_t *entry = (index_entry_t *)atomic_load(&idx->buckets[bucket]);
    
    while (entry) {
        if (entry->uri_hash == hash && strcmp(entry->uri, uri) == 0) {
            atomic_fetch_add(&idx->stat_hits, 1);
            return entry;
        }
        entry = (index_entry_t *)atomic_load(&entry->next);
    }
    
    return NULL;
}

int backend_index_remove(backend_index_t *idx, const char *uri) {
    if (!idx || !uri) return -1;
    
    pthread_mutex_lock(&idx->write_lock);
    
    uint64_t hash = index_hash_string(uri);
    size_t bucket = hash & (idx->num_buckets - 1);
    
    atomic_uintptr_t *prev_next = &idx->buckets[bucket];
    index_entry_t *entry = (index_entry_t *)atomic_load(&idx->buckets[bucket]);
    
    while (entry) {
        if (entry->uri_hash == hash && strcmp(entry->uri, uri) == 0) {
            index_entry_t *next = (index_entry_t *)atomic_load(&entry->next);
            atomic_store(prev_next, (uintptr_t)next);
            
            index_entry_put(entry);
            atomic_fetch_sub(&idx->num_entries, 1);
            atomic_store(&idx->dirty, 1);
            
            pthread_mutex_unlock(&idx->write_lock);
            return 0;
        }
        
        prev_next = &entry->next;
        entry = (index_entry_t *)atomic_load(&entry->next);
    }
    
    pthread_mutex_unlock(&idx->write_lock);
    return -1;
}

/* Persistent index format helpers */

static uint32_t crc32(uint32_t crc, const void *buf, size_t len) {
    /* Simple CRC32 implementation */
    static const uint32_t table[256] = {
        /* CRC32 table - generated once */
        0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, /* ... truncated for brevity */
    };
    
    const uint8_t *p = buf;
    crc = ~crc;
    while (len--) {
        crc = (crc >> 8) ^ table[(crc ^ *p++) & 0xff];
    }
    return ~crc;
}

static ssize_t write_all(int fd, const void *buf, size_t count) {
    const uint8_t *p = buf;
    size_t written = 0;
    
    while (written < count) {
        ssize_t n = write(fd, p + written, count - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        written += n;
    }
    
    return written;
}

static ssize_t read_all(int fd, void *buf, size_t count) {
    uint8_t *p = buf;
    size_t total = 0;
    
    while (total < count) {
        ssize_t n = read(fd, p + total, count - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break;  /* EOF */
        total += n;
    }
    
    return total;
}

int backend_index_save(backend_index_t *idx) {
    if (!idx || !idx->persist_enabled) return -1;
    
    char tmp_path[PATH_MAX];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", idx->index_file_path);
    
    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    
    /* Write header */
    struct {
        char magic[6];
        uint16_t version;
        uint32_t backend_id;
        uint64_t num_entries;
        uint64_t num_buckets;
    } header;
    
    memcpy(header.magic, INDEX_MAGIC, 6);
    header.version = htole16(INDEX_VERSION);
    header.backend_id = htole32(idx->backend_id);
    header.num_entries = htole64(atomic_load(&idx->num_entries));
    header.num_buckets = htole64(idx->num_buckets);
    
    if (write_all(fd, &header, sizeof(header)) < 0) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }
    
    /* Write entries */
    for (size_t i = 0; i < idx->num_buckets; i++) {
        index_entry_t *entry = (index_entry_t *)atomic_load(&idx->buckets[i]);
        
        while (entry) {
            uint16_t uri_len = strlen(entry->uri);
            uint16_t path_len = strlen(entry->backend_path);
            
            uint16_t uri_len_le = htole16(uri_len);
            uint16_t path_len_le = htole16(path_len);
            uint64_t size_le = htole64(entry->size_bytes);
            uint64_t mtime_le = htole64(entry->mtime);
            uint32_t flags_le = htole32(entry->flags);
            
            write_all(fd, &uri_len_le, sizeof(uri_len_le));
            write_all(fd, entry->uri, uri_len);
            write_all(fd, &path_len_le, sizeof(path_len_le));
            write_all(fd, entry->backend_path, path_len);
            write_all(fd, &size_le, sizeof(size_le));
            write_all(fd, &mtime_le, sizeof(mtime_le));
            write_all(fd, &flags_le, sizeof(flags_le));
            
            entry = (index_entry_t *)atomic_load(&entry->next);
        }
    }
    
    close(fd);
    
    /* Atomic replace */
    if (rename(tmp_path, idx->index_file_path) < 0) {
        unlink(tmp_path);
        return -1;
    }
    
    atomic_store(&idx->dirty, 0);
    return 0;
}

int backend_index_load(backend_index_t *idx) {
    if (!idx || !idx->persist_enabled) return -1;
    
    int fd = open(idx->index_file_path, O_RDONLY);
    if (fd < 0) return -1;
    
    /* Read header */
    struct {
        char magic[6];
        uint16_t version;
        uint32_t backend_id;
        uint64_t num_entries;
        uint64_t num_buckets;
    } header;
    
    if (read_all(fd, &header, sizeof(header)) != sizeof(header)) {
        close(fd);
        return -1;
    }
    
    /* Validate */
    if (memcmp(header.magic, INDEX_MAGIC, 6) != 0) {
        close(fd);
        return -1;
    }
    
    if (le16toh(header.version) != INDEX_VERSION) {
        close(fd);
        return -1;
    }
    
    uint64_t num_entries = le64toh(header.num_entries);
    int loaded = 0;
    
    /* Read entries */
    for (uint64_t i = 0; i < num_entries; i++) {
        uint16_t uri_len, path_len;
        
        if (read_all(fd, &uri_len, sizeof(uri_len)) != sizeof(uri_len)) break;
        uri_len = le16toh(uri_len);
        
        char *uri = malloc(uri_len + 1);
        if (!uri) break;
        
        if (read_all(fd, uri, uri_len) != uri_len) {
            free(uri);
            break;
        }
        uri[uri_len] = '\0';
        
        if (read_all(fd, &path_len, sizeof(path_len)) != sizeof(path_len)) {
            free(uri);
            break;
        }
        path_len = le16toh(path_len);
        
        char *path = malloc(path_len + 1);
        if (!path) {
            free(uri);
            break;
        }
        
        if (read_all(fd, path, path_len) != path_len) {
            free(uri);
            free(path);
            break;
        }
        path[path_len] = '\0';
        
        /* Create entry */
        index_entry_t *entry = index_entry_create(uri, idx->backend_id, path);
        free(uri);
        free(path);
        
        if (!entry) break;
        
        /* Read metadata */
        uint64_t size, mtime;
        uint32_t flags;
        
        read_all(fd, &size, sizeof(size));
        read_all(fd, &mtime, sizeof(mtime));
        read_all(fd, &flags, sizeof(flags));
        
        entry->size_bytes = le64toh(size);
        entry->mtime = le64toh(mtime);
        entry->flags = le32toh(flags);
        
        /* Insert into index */
        backend_index_insert(idx, entry);
        loaded++;
    }
    
    close(fd);
    atomic_store(&idx->dirty, 0);
    
    return loaded;
}

int backend_index_scan(backend_index_t *idx, const char *mount_path,
                       void (*progress_cb)(size_t count, void *data),
                       void *user_data) {
    if (!idx || !mount_path) return -1;
    
    /* For this implementation, we'll do a simple recursive scan
     * In production, this would use nftw() or similar for efficiency */
    
    size_t count = 0;
    size_t mount_path_len = strlen(mount_path);
    
    /* Helper function to recursively scan directory */
    int scan_dir_recursive(const char *dir_path, size_t base_len) {
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
                scan_dir_recursive(full_path, base_len);
            } else if (S_ISREG(st.st_mode)) {
                /* Regular file - create index entry */
                /* URI is the path relative to mount point */
                const char *uri = full_path + base_len;
                
                /* Create index entry */
                index_entry_t *idx_entry = index_entry_create(uri, idx->backend_id, full_path);
                if (idx_entry) {
                    idx_entry->size_bytes = st.st_size;
                    idx_entry->flags = INDEX_FLAG_PERSISTENT;
                    
                    /* Insert into index */
                    backend_index_insert(idx, idx_entry);
                    index_entry_put(idx_entry);  /* Release our reference */
                    
                    count++;
                    
                    if (progress_cb && (count % 100 == 0)) {
                        progress_cb(count, user_data);
                    }
                }
            }
        }
        
        closedir(dir);
        return 0;
    }
    
    scan_dir_recursive(mount_path, mount_path_len);
    
    if (progress_cb) {
        progress_cb(count, user_data);
    }
    
    return (int)count;
}
