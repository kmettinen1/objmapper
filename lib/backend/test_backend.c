/**
 * @file test_backend.c
 * @brief Test suite for backend manager
 */

#include "backend.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* Test directory setup */
static void setup_test_dirs(void) {
    system("rm -rf /tmp/objmapper_test_*");
    mkdir("/tmp/objmapper_test_memory", 0755);
    mkdir("/tmp/objmapper_test_nvme", 0755);
    mkdir("/tmp/objmapper_test_ssd", 0755);
}

static void cleanup_test_dirs(void) {
    system("rm -rf /tmp/objmapper_test_*");
}

static void test_backend_creation(void) {
    printf("Testing backend manager creation...\n");
    
    backend_manager_t *mgr = backend_manager_create(1024, 100);
    assert(mgr != NULL);
    assert(mgr->global_index != NULL);
    assert(mgr->num_backends == 0);
    
    printf("  ✓ Backend manager created\n");
    
    backend_manager_destroy(mgr);
    printf("✓ Backend creation test passed\n\n");
}

static void test_backend_registration(void) {
    printf("Testing backend registration...\n");
    
    backend_manager_t *mgr = backend_manager_create(1024, 100);
    assert(mgr != NULL);
    
    /* Register memory backend */
    int mem_id = backend_manager_register(
        mgr,
        BACKEND_TYPE_MEMORY,
        "/tmp/objmapper_test_memory",
        "Test Memory",
        1024 * 1024 * 1024,  /* 1GB */
        BACKEND_FLAG_EPHEMERAL_ONLY | BACKEND_FLAG_MIGRATION_SRC | BACKEND_FLAG_MIGRATION_DST
    );
    assert(mem_id == 0);
    
    /* Register NVMe backend */
    int nvme_id = backend_manager_register(
        mgr,
        BACKEND_TYPE_NVME,
        "/tmp/objmapper_test_nvme",
        "Test NVMe",
        10ULL * 1024 * 1024 * 1024,  /* 10GB */
        BACKEND_FLAG_PERSISTENT | BACKEND_FLAG_MIGRATION_SRC | BACKEND_FLAG_MIGRATION_DST
    );
    assert(nvme_id == 1);
    
    /* Register SSD backend */
    int ssd_id = backend_manager_register(
        mgr,
        BACKEND_TYPE_SSD,
        "/tmp/objmapper_test_ssd",
        "Test SSD",
        100ULL * 1024 * 1024 * 1024,  /* 100GB */
        BACKEND_FLAG_PERSISTENT | BACKEND_FLAG_MIGRATION_SRC | BACKEND_FLAG_MIGRATION_DST
    );
    assert(ssd_id == 2);
    
    assert(mgr->num_backends == 3);
    
    printf("  ✓ Registered 3 backends\n");
    
    /* Get backend info */
    backend_info_t *mem = backend_manager_get_backend(mgr, mem_id);
    assert(mem != NULL);
    assert(mem->type == BACKEND_TYPE_MEMORY);
    assert(strcmp(mem->name, "Test Memory") == 0);
    assert(mem->perf_factor == 1.0);
    
    backend_info_t *nvme = backend_manager_get_backend(mgr, nvme_id);
    assert(nvme != NULL);
    assert(nvme->type == BACKEND_TYPE_NVME);
    assert(nvme->perf_factor == 3.0);
    
    printf("  ✓ Backend info correct\n");
    
    /* Set defaults */
    assert(backend_manager_set_ephemeral(mgr, mem_id) == 0);
    assert(backend_manager_set_default(mgr, nvme_id) == 0);
    assert(mgr->ephemeral_backend_id == mem_id);
    assert(mgr->default_backend_id == nvme_id);
    
    /* Security check: can't set ephemeral backend as default */
    assert(backend_manager_set_default(mgr, mem_id) < 0);
    
    printf("  ✓ Default backends set correctly\n");
    
    backend_manager_destroy(mgr);
    printf("✓ Backend registration test passed\n\n");
}

static void test_object_operations(void) {
    printf("Testing object operations...\n");
    
    backend_manager_t *mgr = backend_manager_create(1024, 100);
    assert(mgr != NULL);
    
    /* Register backends */
    int mem_id = backend_manager_register(
        mgr, BACKEND_TYPE_MEMORY, "/tmp/objmapper_test_memory",
        "Memory", 1024 * 1024 * 1024,
        BACKEND_FLAG_EPHEMERAL_ONLY | BACKEND_FLAG_MIGRATION_SRC | BACKEND_FLAG_MIGRATION_DST
    );
    
    int nvme_id = backend_manager_register(
        mgr, BACKEND_TYPE_NVME, "/tmp/objmapper_test_nvme",
        "NVMe", 10ULL * 1024 * 1024 * 1024,
        BACKEND_FLAG_PERSISTENT | BACKEND_FLAG_MIGRATION_SRC | BACKEND_FLAG_MIGRATION_DST
    );
    
    backend_manager_set_ephemeral(mgr, mem_id);
    backend_manager_set_default(mgr, nvme_id);
    
    /* Create persistent object */
    object_create_req_t req = {
        .uri = "/test/object1.txt",
        .backend_id = -1,  /* Auto-select */
        .ephemeral = false,
        .size_hint = 1024,
        .flags = 0
    };
    
    fd_ref_t ref;
    int ret = backend_create_object(mgr, &req, &ref);
    assert(ret == 0);
    assert(ref.entry != NULL);
    assert(ref.entry->backend_id == nvme_id);
    
    /* Write some data */
    int fd = fd_ref_acquire(&ref);
    assert(fd >= 0);
    
    const char *data = "Hello, objmapper!";
    ssize_t written = write(fd, data, strlen(data));
    assert(written == (ssize_t)strlen(data));
    
    backend_update_size(mgr, "/test/object1.txt", strlen(data));
    
    fd_ref_release(&ref);
    
    printf("  ✓ Created persistent object\n");
    
    /* Get object */
    fd_ref_t ref2;
    ret = backend_get_object(mgr, "/test/object1.txt", &ref2);
    assert(ret == 0);
    assert(ref2.entry != NULL);
    
    /* Read data */
    fd = fd_ref_acquire(&ref2);
    assert(fd >= 0);
    
    char buf[64];
    lseek(fd, 0, SEEK_SET);
    ssize_t nread = read(fd, buf, sizeof(buf));
    assert(nread == (ssize_t)strlen(data));
    assert(memcmp(buf, data, strlen(data)) == 0);
    
    fd_ref_release(&ref2);
    
    printf("  ✓ Retrieved and read object\n");
    
    /* Get metadata */
    object_metadata_t metadata;
    ret = backend_get_metadata(mgr, "/test/object1.txt", &metadata);
    assert(ret == 0);
    assert(strcmp(metadata.uri, "/test/object1.txt") == 0);
    assert(metadata.backend_id == nvme_id);
    assert(metadata.size_bytes == strlen(data));
    assert(!metadata.has_payload);
    object_metadata_free(&metadata);
    
    printf("  ✓ Metadata retrieval works\n");

    objm_payload_descriptor_t payload;
    objm_payload_descriptor_init(&payload);
    payload.variant_count = 1;
    payload.manifest_flags = OBJM_PAYLOAD_FLAG_HAS_VARIANTS;
    objm_variant_descriptor_t *variant = &payload.variants[0];
    snprintf(variant->variant_id, OBJM_VARIANT_ID_MAX, "%s", "identity");
    variant->logical_length = strlen(data);
    variant->storage_length = strlen(data);
    variant->encoding = OBJM_ENCODING_IDENTITY;
    variant->capabilities = OBJM_CAP_IDENTITY | OBJM_CAP_ZERO_COPY;
    variant->is_primary = 1;

    ret = backend_set_payload_metadata(mgr, "/test/object1.txt", &payload);
    assert(ret == 0);

    objm_payload_descriptor_t payload_out;
    objm_payload_descriptor_init(&payload_out);
    ret = backend_get_payload_metadata(mgr, "/test/object1.txt", &payload_out);
    assert(ret == 0);
    assert(payload_out.variant_count == 1);
    assert(strcmp(payload_out.variants[0].variant_id, "identity") == 0);
    assert(payload_out.variants[0].capabilities & OBJM_CAP_ZERO_COPY);

    ret = backend_get_metadata(mgr, "/test/object1.txt", &metadata);
    assert(ret == 0);
    assert(metadata.has_payload);
    assert(metadata.payload.variant_count == 1);
    object_metadata_free(&metadata);
    
    printf("  ✓ Payload metadata lifecycle works\n");
    
    /* Create ephemeral object */
    object_create_req_t eph_req = {
        .uri = "/tmp/ephemeral.dat",
        .backend_id = -1,
        .ephemeral = true,
        .size_hint = 512,
        .flags = 0
    };
    
    fd_ref_t eph_ref;
    ret = backend_create_object(mgr, &eph_req, &eph_ref);
    assert(ret == 0);
    assert(eph_ref.entry->backend_id == mem_id);
    assert(eph_ref.entry->flags & INDEX_FLAG_EPHEMERAL);
    
    fd_ref_release(&eph_ref);
    
    printf("  ✓ Created ephemeral object\n");
    
    /* Delete object */
    ret = backend_delete_object(mgr, "/test/object1.txt");
    assert(ret == 0);
    
    /* Verify deleted */
    ret = backend_get_object(mgr, "/test/object1.txt", &ref2);
    assert(ret < 0);
    
    printf("  ✓ Object deletion works\n");
    
    backend_manager_destroy(mgr);
    printf("✓ Object operations test passed\n\n");
}

static void test_backend_stats(void) {
    printf("Testing backend statistics...\n");
    
    backend_manager_t *mgr = backend_manager_create(1024, 100);
    assert(mgr != NULL);
    
    int nvme_id = backend_manager_register(
        mgr, BACKEND_TYPE_NVME, "/tmp/objmapper_test_nvme",
        "NVMe", 10ULL * 1024 * 1024 * 1024,
        BACKEND_FLAG_PERSISTENT | BACKEND_FLAG_MIGRATION_SRC | BACKEND_FLAG_MIGRATION_DST
    );
    
    backend_manager_set_default(mgr, nvme_id);
    
    /* Create object */
    object_create_req_t req = {
        .uri = "/stats/test.dat",
        .backend_id = -1,
        .ephemeral = false,
        .size_hint = 100,
        .flags = 0
    };
    
    fd_ref_t ref;
    backend_create_object(mgr, &req, &ref);
    fd_ref_release(&ref);
    
    /* Get object (triggers read stat) */
    backend_get_object(mgr, "/stats/test.dat", &ref);
    fd_ref_release(&ref);
    
    /* Check stats */
    size_t reads, writes, mig_in, mig_out;
    backend_get_stats(mgr, nvme_id, &reads, &writes, &mig_in, &mig_out);
    
    assert(reads == 1);    /* One get_object call */
    assert(writes == 1);   /* One create_object call */
    assert(mig_in == 0);
    assert(mig_out == 0);
    
    printf("  ✓ Statistics tracking works (reads=%zu, writes=%zu)\n", reads, writes);
    
    backend_manager_destroy(mgr);
    printf("✓ Backend statistics test passed\n\n");
}

static void test_backend_management(void) {
    printf("Testing backend management...\n");
    
    backend_manager_t *mgr = backend_manager_create(1024, 100);
    assert(mgr != NULL);
    
    int backend_id = backend_manager_register(
        mgr, BACKEND_TYPE_SSD, "/tmp/objmapper_test_ssd",
        "SSD", 100ULL * 1024 * 1024 * 1024,
        BACKEND_FLAG_PERSISTENT | BACKEND_FLAG_MIGRATION_SRC | BACKEND_FLAG_MIGRATION_DST
    );
    
    /* Test enable/disable */
    backend_info_t *backend = backend_manager_get_backend(mgr, backend_id);
    assert(backend->flags & BACKEND_FLAG_ENABLED);
    
    backend_set_enabled(mgr, backend_id, false);
    assert(!(backend->flags & BACKEND_FLAG_ENABLED));
    
    backend_set_enabled(mgr, backend_id, true);
    assert(backend->flags & BACKEND_FLAG_ENABLED);
    
    printf("  ✓ Enable/disable works\n");
    
    /* Test watermarks */
    assert(backend->high_watermark == 0.85);
    assert(backend->low_watermark == 0.70);
    
    backend_set_watermarks(mgr, backend_id, 0.90, 0.75);
    assert(backend->high_watermark == 0.90);
    assert(backend->low_watermark == 0.75);
    
    /* Invalid watermarks */
    assert(backend_set_watermarks(mgr, backend_id, 0.5, 0.8) < 0);  /* low > high */
    
    printf("  ✓ Watermark configuration works\n");
    
    /* Test migration policy */
    assert(backend->migration_policy == MIGRATION_POLICY_HYBRID);
    
    backend_set_migration_policy(mgr, backend_id, MIGRATION_POLICY_HOTNESS, 0.7);
    assert(backend->migration_policy == MIGRATION_POLICY_HOTNESS);
    assert(backend->hotness_threshold == 0.7);
    
    printf("  ✓ Migration policy configuration works\n");
    
    backend_manager_destroy(mgr);
    printf("✓ Backend management test passed\n\n");
}

int main(void) {
    printf("=== objmapper Backend Tests ===\n\n");
    
    setup_test_dirs();
    
    test_backend_creation();
    test_backend_registration();
    test_object_operations();
    test_backend_stats();
    test_backend_management();
    
    cleanup_test_dirs();
    
    printf("=== All tests passed! ===\n");
    return 0;
}
