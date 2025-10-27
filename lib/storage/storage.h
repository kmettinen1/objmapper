/**
 * @file storage.h
 * @brief Object storage with URI/object dictionary and caching
 *
 * Provides an interface for storing and retrieving objects by URI,
 * with support for memory-mapped caching and backing file storage.
 */

#ifndef STORAGE_H
#define STORAGE_H

#include <sys/types.h>
#include <stdint.h>

/* Opaque storage handle */
typedef struct object_storage object_storage_t;

/* Storage configuration */
typedef struct {
    const char *backing_dir;    /* Directory for persistent storage */
    const char *cache_dir;      /* Directory for cached objects */
    size_t cache_limit;         /* Maximum cache size in bytes */
    size_t hash_size;           /* Hash table size (0 for auto) */
} storage_config_t;

/* Object metadata */
typedef struct {
    const char *uri;            /* Object URI/name */
    size_t size;                /* Object size in bytes */
    uint64_t hits;              /* Access count */
    int is_cached;              /* Whether object is in cache */
} object_info_t;

/**
 * Initialize object storage
 * 
 * @param config Storage configuration
 * @return Storage handle on success, NULL on failure
 */
object_storage_t *storage_init(const storage_config_t *config);

/**
 * Add or update an object in storage
 * 
 * @param storage Storage handle
 * @param uri Object URI/identifier
 * @param data Object data
 * @param size Object size in bytes
 * @return 0 on success, -1 on failure
 */
int storage_put(object_storage_t *storage, const char *uri, 
                const void *data, size_t size);

/**
 * Get file descriptor for an object
 * 
 * @param storage Storage handle
 * @param uri Object URI/identifier
 * @param info Optional pointer to receive object info
 * @return File descriptor on success, -1 on failure
 */
int storage_get_fd(object_storage_t *storage, const char *uri, 
                   object_info_t *info);

/**
 * Get object data via memory map
 * 
 * @param storage Storage handle
 * @param uri Object URI/identifier
 * @param size Pointer to receive object size
 * @return Pointer to mapped memory on success, NULL on failure
 */
void *storage_get_mmap(object_storage_t *storage, const char *uri, 
                       size_t *size);

/**
 * Get object information
 * 
 * @param storage Storage handle
 * @param uri Object URI/identifier
 * @param info Pointer to receive object info
 * @return 0 on success, -1 on failure
 */
int storage_get_info(object_storage_t *storage, const char *uri,
                     object_info_t *info);

/**
 * Remove an object from storage
 * 
 * @param storage Storage handle
 * @param uri Object URI/identifier
 * @return 0 on success, -1 on failure
 */
int storage_remove(object_storage_t *storage, const char *uri);

/**
 * Get storage statistics
 * 
 * @param storage Storage handle
 * @param total_objects Pointer to receive total object count
 * @param cached_bytes Pointer to receive cached bytes
 * @param total_hits Pointer to receive total hit count
 * @return 0 on success, -1 on failure
 */
int storage_get_stats(object_storage_t *storage, size_t *total_objects,
                      size_t *cached_bytes, uint64_t *total_hits);

/**
 * Cleanup and free storage
 * 
 * @param storage Storage handle
 */
void storage_cleanup(object_storage_t *storage);

#endif /* STORAGE_H */
