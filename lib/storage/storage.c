/**
 * @file storage.c
 * @brief Implementation of object storage with URI dictionary
 */

#define _GNU_SOURCE
#include "storage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <errno.h>
#include <dirent.h>
#include <pthread.h>

#ifdef DEBUG
#define dprintf(...) fprintf(stderr, __VA_ARGS__)
#else
#define dprintf(...) ((void)0)
#endif

/* Hash table entry */
typedef struct hash_entry {
    uint64_t hash;              /* Full hash of URI */
    size_t slot_index;          /* Index into object array */
    struct hash_entry *next;    /* Collision chain */
} hash_entry_t;

/* Object slot */
typedef struct object_slot {
    char uri[1024];             /* Object URI */
    char backing_path[1024];    /* Path to backing file */
    char cache_path[1024];      /* Path to cached file */
    int backing_fd;             /* File descriptor for backing file */
    void *cache_mmap;           /* Memory mapped cache */
    size_t size;                /* Object size */
    uint64_t hits;              /* Access count */
    int in_use;                 /* Slot is active */
} object_slot_t;

/* Storage structure */
struct object_storage {
    char backing_dir[256];
    char cache_dir[256];
    size_t cache_limit;
    size_t cached_bytes;
    
    object_slot_t *objects;
    size_t capacity;
    size_t count;
    
    hash_entry_t **hash_table;
    size_t hash_size;
    
    pthread_rwlock_t lock;
};

/* Hash function for URIs */
static uint64_t hash_uri(const char *uri)
{
    uint64_t hash = 5381;
    int c;
    
    while ((c = *uri++)) {
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
    }
    
    return hash;
}

/* Find object slot by URI */
static object_slot_t *find_slot(object_storage_t *storage, const char *uri,
                                uint64_t *out_hash)
{
    uint64_t hash = hash_uri(uri);
    size_t idx = hash % storage->hash_size;
    
    if (out_hash) *out_hash = hash;
    
    hash_entry_t *entry = storage->hash_table[idx];
    while (entry != NULL) {
        if (entry->hash == hash) {
            object_slot_t *slot = &storage->objects[entry->slot_index];
            if (slot->in_use && strcmp(slot->uri, uri) == 0) {
                return slot;
            }
        }
        entry = entry->next;
    }
    
    return NULL;
}

/* Add hash entry */
static int add_hash_entry(object_storage_t *storage, const char *uri,
                         size_t slot_index)
{
    uint64_t hash = hash_uri(uri);
    size_t idx = hash % storage->hash_size;
    
    hash_entry_t *entry = malloc(sizeof(hash_entry_t));
    if (!entry) return -1;
    
    entry->hash = hash;
    entry->slot_index = slot_index;
    entry->next = storage->hash_table[idx];
    storage->hash_table[idx] = entry;
    
    return 0;
}

object_storage_t *storage_init(const storage_config_t *config)
{
    if (!config || !config->backing_dir) {
        errno = EINVAL;
        return NULL;
    }
    
    object_storage_t *storage = calloc(1, sizeof(object_storage_t));
    if (!storage) return NULL;
    
    /* Initialize configuration */
    strncpy(storage->backing_dir, config->backing_dir, 
            sizeof(storage->backing_dir) - 1);
    if (config->cache_dir) {
        strncpy(storage->cache_dir, config->cache_dir,
                sizeof(storage->cache_dir) - 1);
    }
    storage->cache_limit = config->cache_limit;
    
    /* Initialize capacity */
    storage->capacity = 10000; /* Default capacity */
    storage->objects = calloc(storage->capacity, sizeof(object_slot_t));
    if (!storage->objects) {
        free(storage);
        return NULL;
    }
    
    /* Initialize hash table */
    storage->hash_size = config->hash_size > 0 ? config->hash_size : 16384;
    storage->hash_table = calloc(storage->hash_size, sizeof(hash_entry_t *));
    if (!storage->hash_table) {
        free(storage->objects);
        free(storage);
        return NULL;
    }
    
    pthread_rwlock_init(&storage->lock, NULL);
    
    /* Create directories if needed */
    mkdir(storage->backing_dir, 0755);
    if (storage->cache_dir[0]) {
        mkdir(storage->cache_dir, 0755);
    }
    
    dprintf("storage_init: backing='%s', cache='%s', limit=%zu\n",
            storage->backing_dir, storage->cache_dir, storage->cache_limit);
    
    return storage;
}

int storage_put(object_storage_t *storage, const char *uri,
                const void *data, size_t size)
{
    if (!storage || !uri || !data) {
        errno = EINVAL;
        return -1;
    }
    
    pthread_rwlock_wrlock(&storage->lock);
    
    /* Check if object exists */
    object_slot_t *slot = find_slot(storage, uri, NULL);
    
    if (!slot) {
        /* Find free slot */
        if (storage->count >= storage->capacity) {
            pthread_rwlock_unlock(&storage->lock);
            errno = ENOMEM;
            return -1;
        }
        
        for (size_t i = 0; i < storage->capacity; i++) {
            if (!storage->objects[i].in_use) {
                slot = &storage->objects[i];
                slot->in_use = 1;
                storage->count++;
                strncpy(slot->uri, uri, sizeof(slot->uri) - 1);
                add_hash_entry(storage, uri, i);
                break;
            }
        }
    }
    
    if (!slot) {
        pthread_rwlock_unlock(&storage->lock);
        return -1;
    }
    
    /* Write to backing file */
    snprintf(slot->backing_path, sizeof(slot->backing_path),
             "%s/%s", storage->backing_dir, uri);
    
    int fd = open(slot->backing_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        pthread_rwlock_unlock(&storage->lock);
        return -1;
    }
    
    ssize_t written = write(fd, data, size);
    close(fd);
    
    if (written != (ssize_t)size) {
        pthread_rwlock_unlock(&storage->lock);
        return -1;
    }
    
    slot->size = size;
    slot->backing_fd = -1; /* Will open on demand */
    
    pthread_rwlock_unlock(&storage->lock);
    
    dprintf("storage_put: uri='%s', size=%zu\n", uri, size);
    return 0;
}

int storage_get_fd(object_storage_t *storage, const char *uri,
                   object_info_t *info)
{
    if (!storage || !uri) {
        errno = EINVAL;
        return -1;
    }
    
    pthread_rwlock_rdlock(&storage->lock);
    
    object_slot_t *slot = find_slot(storage, uri, NULL);
    if (!slot) {
        pthread_rwlock_unlock(&storage->lock);
        errno = ENOENT;
        return -1;
    }
    
    /* Open backing file if not already open */
    if (slot->backing_fd < 0) {
        slot->backing_fd = open(slot->backing_path, O_RDONLY);
        if (slot->backing_fd < 0) {
            pthread_rwlock_unlock(&storage->lock);
            return -1;
        }
    }
    
    slot->hits++;
    
    if (info) {
        info->uri = slot->uri;
        info->size = slot->size;
        info->hits = slot->hits;
        info->is_cached = (slot->cache_mmap != NULL);
    }
    
    int fd = dup(slot->backing_fd);
    
    pthread_rwlock_unlock(&storage->lock);
    
    dprintf("storage_get_fd: uri='%s', fd=%d, hits=%lu\n", 
            uri, fd, slot->hits);
    
    return fd;
}

void *storage_get_mmap(object_storage_t *storage, const char *uri,
                       size_t *size)
{
    if (!storage || !uri) {
        errno = EINVAL;
        return NULL;
    }
    
    pthread_rwlock_rdlock(&storage->lock);
    
    object_slot_t *slot = find_slot(storage, uri, NULL);
    if (!slot) {
        pthread_rwlock_unlock(&storage->lock);
        errno = ENOENT;
        return NULL;
    }
    
    /* Create cache mmap if not exists and within limit */
    if (!slot->cache_mmap && storage->cache_dir[0] &&
        storage->cached_bytes + slot->size <= storage->cache_limit) {
        
        int fd = open(slot->backing_path, O_RDONLY);
        if (fd >= 0) {
            slot->cache_mmap = mmap(NULL, slot->size, PROT_READ,
                                   MAP_PRIVATE, fd, 0);
            if (slot->cache_mmap != MAP_FAILED) {
                storage->cached_bytes += slot->size;
            } else {
                slot->cache_mmap = NULL;
            }
            close(fd);
        }
    }
    
    slot->hits++;
    
    if (size) *size = slot->size;
    
    void *ptr = slot->cache_mmap;
    pthread_rwlock_unlock(&storage->lock);
    
    return ptr;
}

int storage_get_info(object_storage_t *storage, const char *uri,
                     object_info_t *info)
{
    if (!storage || !uri || !info) {
        errno = EINVAL;
        return -1;
    }
    
    pthread_rwlock_rdlock(&storage->lock);
    
    object_slot_t *slot = find_slot(storage, uri, NULL);
    if (!slot) {
        pthread_rwlock_unlock(&storage->lock);
        errno = ENOENT;
        return -1;
    }
    
    info->uri = slot->uri;
    info->size = slot->size;
    info->hits = slot->hits;
    info->is_cached = (slot->cache_mmap != NULL);
    
    pthread_rwlock_unlock(&storage->lock);
    return 0;
}

int storage_remove(object_storage_t *storage, const char *uri)
{
    if (!storage || !uri) {
        errno = EINVAL;
        return -1;
    }
    
    pthread_rwlock_wrlock(&storage->lock);
    
    object_slot_t *slot = find_slot(storage, uri, NULL);
    if (!slot) {
        pthread_rwlock_unlock(&storage->lock);
        errno = ENOENT;
        return -1;
    }
    
    /* Clean up resources */
    if (slot->backing_fd >= 0) {
        close(slot->backing_fd);
    }
    if (slot->cache_mmap) {
        munmap(slot->cache_mmap, slot->size);
        storage->cached_bytes -= slot->size;
    }
    
    unlink(slot->backing_path);
    if (slot->cache_path[0]) {
        unlink(slot->cache_path);
    }
    
    slot->in_use = 0;
    storage->count--;
    
    pthread_rwlock_unlock(&storage->lock);
    
    dprintf("storage_remove: uri='%s'\n", uri);
    return 0;
}

int storage_get_stats(object_storage_t *storage, size_t *total_objects,
                      size_t *cached_bytes, uint64_t *total_hits)
{
    if (!storage) {
        errno = EINVAL;
        return -1;
    }
    
    pthread_rwlock_rdlock(&storage->lock);
    
    if (total_objects) *total_objects = storage->count;
    if (cached_bytes) *cached_bytes = storage->cached_bytes;
    
    if (total_hits) {
        uint64_t hits = 0;
        for (size_t i = 0; i < storage->capacity; i++) {
            if (storage->objects[i].in_use) {
                hits += storage->objects[i].hits;
            }
        }
        *total_hits = hits;
    }
    
    pthread_rwlock_unlock(&storage->lock);
    return 0;
}

void storage_cleanup(object_storage_t *storage)
{
    if (!storage) return;
    
    pthread_rwlock_wrlock(&storage->lock);
    
    /* Close all file descriptors and unmap memory */
    for (size_t i = 0; i < storage->capacity; i++) {
        if (storage->objects[i].in_use) {
            if (storage->objects[i].backing_fd >= 0) {
                close(storage->objects[i].backing_fd);
            }
            if (storage->objects[i].cache_mmap) {
                munmap(storage->objects[i].cache_mmap,
                      storage->objects[i].size);
            }
        }
    }
    
    /* Free hash table */
    for (size_t i = 0; i < storage->hash_size; i++) {
        hash_entry_t *entry = storage->hash_table[i];
        while (entry) {
            hash_entry_t *next = entry->next;
            free(entry);
            entry = next;
        }
    }
    
    free(storage->hash_table);
    free(storage->objects);
    
    pthread_rwlock_unlock(&storage->lock);
    pthread_rwlock_destroy(&storage->lock);
    
    free(storage);
    
    dprintf("storage_cleanup: done\n");
}
