/**
 * @file test_index.c
 * @brief Basic tests for index library
 */

#include "index.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

static void test_basic_operations(void) {
    printf("Testing basic operations...\n");
    
    /* Create index */
    global_index_t *idx = global_index_create(1024, 100);
    assert(idx != NULL);
    
    /* Create entry */
    index_entry_t *entry = index_entry_create("/test/object1", 1, "/mnt/backend1/test/object1");
    assert(entry != NULL);
    
    /* Insert */
    int ret = global_index_insert(idx, entry);
    assert(ret == 0);
    
    /* Lookup */
    fd_ref_t ref;
    ret = global_index_lookup(idx, "/test/object1", &ref);
    assert(ret == 0);
    assert(ref.entry == entry);
    
    fd_ref_release(&ref);
    
    /* Check stats */
    index_stats_t stats;
    global_index_get_stats(idx, &stats);
    assert(stats.num_entries == 1);
    assert(stats.lookups == 1);
    assert(stats.hits == 1);
    assert(stats.misses == 0);
    
    printf("  ✓ Insert and lookup work\n");
    
    /* Remove */
    ret = global_index_remove(idx, "/test/object1");
    assert(ret == 0);
    
    /* Verify removed */
    ret = global_index_lookup(idx, "/test/object1", &ref);
    assert(ret == -1);
    
    global_index_get_stats(idx, &stats);
    assert(stats.num_entries == 0);
    
    printf("  ✓ Remove works\n");
    
    global_index_destroy(idx);
    printf("✓ Basic operations passed\n\n");
}

static void test_collisions(void) {
    printf("Testing hash collisions...\n");
    
    global_index_t *idx = global_index_create(16, 100);  /* Small table = more collisions */
    assert(idx != NULL);
    
    /* Insert multiple entries */
    for (int i = 0; i < 100; i++) {
        char uri[64];
        char path[128];
        snprintf(uri, sizeof(uri), "/object%d", i);
        snprintf(path, sizeof(path), "/mnt/backend/object%d", i);
        
        index_entry_t *entry = index_entry_create(uri, 1, path);
        assert(entry != NULL);
        
        int ret = global_index_insert(idx, entry);
        assert(ret == 0);
    }
    
    /* Lookup all entries */
    for (int i = 0; i < 100; i++) {
        char uri[64];
        snprintf(uri, sizeof(uri), "/object%d", i);
        
        fd_ref_t ref;
        int ret = global_index_lookup(idx, uri, &ref);
        assert(ret == 0);
        assert(strcmp(ref.entry->uri, uri) == 0);
        fd_ref_release(&ref);
    }
    
    index_stats_t stats;
    global_index_get_stats(idx, &stats);
    assert(stats.num_entries == 100);
    assert(stats.hits == 100);
    
    printf("  ✓ All 100 entries inserted and retrieved correctly\n");
    
    global_index_destroy(idx);
    printf("✓ Collision handling passed\n\n");
}

static void test_fd_lifecycle(void) {
    printf("Testing FD lifecycle...\n");
    
    /* Create test file */
    const char *test_file = "/tmp/objmapper_test_fd.txt";
    int fd = open(test_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    assert(fd >= 0);
    write(fd, "test data", 9);
    close(fd);
    
    global_index_t *idx = global_index_create(1024, 10);
    assert(idx != NULL);
    
    /* Create entry pointing to test file */
    index_entry_t *entry = index_entry_create("/test/file", 1, test_file);
    assert(entry != NULL);
    
    global_index_insert(idx, entry);
    
    /* Lookup and acquire FD */
    fd_ref_t ref;
    int ret = global_index_lookup(idx, "/test/file", &ref);
    assert(ret == 0);
    
    int acquired_fd = fd_ref_acquire(&ref);
    assert(acquired_fd >= 0);
    
    /* Read from FD */
    char buf[16];
    ssize_t n = read(acquired_fd, buf, sizeof(buf));
    assert(n == 9);
    assert(memcmp(buf, "test data", 9) == 0);
    
    printf("  ✓ FD acquisition and read work\n");
    
    /* Release FD */
    fd_ref_release(&ref);
    
    /* Cleanup */
    global_index_destroy(idx);
    unlink(test_file);
    
    printf("✓ FD lifecycle passed\n\n");
}

static void test_backend_index(void) {
    printf("Testing backend index...\n");
    
    backend_index_t *idx = backend_index_create(1, "/tmp/objmapper_test.idx", 256);
    assert(idx != NULL);
    
    /* Insert entries */
    for (int i = 0; i < 10; i++) {
        char uri[64];
        char path[128];
        snprintf(uri, sizeof(uri), "/obj%d", i);
        snprintf(path, sizeof(path), "/mnt/backend/obj%d", i);
        
        index_entry_t *entry = index_entry_create(uri, 1, path);
        assert(entry != NULL);
        entry->size_bytes = 1024 * i;
        
        int ret = backend_index_insert(idx, entry);
        assert(ret == 0);
    }
    
    /* Lookup */
    index_entry_t *found = backend_index_lookup(idx, "/obj5");
    assert(found != NULL);
    assert(found->size_bytes == 1024 * 5);
    
    printf("  ✓ Backend index insert and lookup work\n");
    
    /* Save to disk */
    int ret = backend_index_save(idx);
    assert(ret == 0);
    
    printf("  ✓ Persistent save works\n");
    
    /* Destroy and recreate */
    backend_index_destroy(idx);
    
    idx = backend_index_create(1, "/tmp/objmapper_test.idx", 256);
    assert(idx != NULL);
    
    /* Load from disk */
    int loaded = backend_index_load(idx);
    assert(loaded == 10);
    
    /* Verify entries */
    found = backend_index_lookup(idx, "/obj5");
    assert(found != NULL);
    assert(found->size_bytes == 1024 * 5);
    
    printf("  ✓ Persistent load works\n");
    
    backend_index_destroy(idx);
    unlink("/tmp/objmapper_test.idx");
    
    printf("✓ Backend index passed\n\n");
}

static void test_concurrent_lookup(void) {
    printf("Testing concurrent lookups (simulated)...\n");
    
    global_index_t *idx = global_index_create(1024, 100);
    assert(idx != NULL);
    
    /* Insert test entry */
    index_entry_t *entry = index_entry_create("/shared", 1, "/tmp/shared");
    global_index_insert(idx, entry);
    
    /* Simulate multiple concurrent readers */
    fd_ref_t refs[10];
    
    for (int i = 0; i < 10; i++) {
        int ret = global_index_lookup(idx, "/shared", &refs[i]);
        assert(ret == 0);
        assert(refs[i].entry == entry);
    }
    
    /* All references point to same entry */
    int refcount = atomic_load(&entry->entry_refcount);
    assert(refcount == 11);  /* 1 from index + 10 from refs */
    
    printf("  ✓ Multiple concurrent lookups work (refcount = %d)\n", refcount);
    
    /* Release all */
    for (int i = 0; i < 10; i++) {
        fd_ref_release(&refs[i]);
    }
    
    refcount = atomic_load(&entry->entry_refcount);
    assert(refcount == 1);  /* Back to just index reference */
    
    printf("  ✓ Reference counting works correctly\n");
    
    global_index_destroy(idx);
    printf("✓ Concurrent lookup test passed\n\n");
}

int main(void) {
    printf("=== objmapper Index Tests ===\n\n");
    
    test_basic_operations();
    test_collisions();
    test_fd_lifecycle();
    test_backend_index();
    test_concurrent_lookup();
    
    printf("=== All tests passed! ===\n");
    return 0;
}
