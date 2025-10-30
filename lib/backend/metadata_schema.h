/*
 * @file metadata_schema.h
 * @brief Object payload metadata schema definitions for objmapper ↔ Varnish integration
 */

#ifndef OBJMAPPER_METADATA_SCHEMA_H
#define OBJMAPPER_METADATA_SCHEMA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OBJM_PAYLOAD_DESCRIPTOR_VERSION 1
#define OBJM_MAX_VARIANTS 8
#define OBJM_VARIANT_ID_MAX 32

/* Content encoding identifiers */
typedef enum {
    OBJM_ENCODING_IDENTITY = 0,
    OBJM_ENCODING_GZIP = 1,
    OBJM_ENCODING_BROTLI = 2,
    OBJM_ENCODING_ZSTD = 3,
    OBJM_ENCODING_CUSTOM = 255
} objm_content_encoding_t;

/* Capability bitmask for delivery features */
#define OBJM_CAP_IDENTITY        (1u << 0)
#define OBJM_CAP_GZIP            (1u << 1)
#define OBJM_CAP_ESI_FLATTENED   (1u << 2)
#define OBJM_CAP_RANGE_READY     (1u << 3)
#define OBJM_CAP_ZERO_COPY       (1u << 4)
#define OBJM_CAP_TLS_OFFLOAD     (1u << 5)

/* Payload manifest level flags */
#define OBJM_PAYLOAD_FLAG_HAS_VARIANTS  (1u << 0)
#define OBJM_PAYLOAD_FLAG_LEGACY_FALLBACK (1u << 1)

/* Variant descriptor describing a single deliverable body */
typedef struct objm_variant_descriptor {
    char variant_id[OBJM_VARIANT_ID_MAX];
    uint32_t capabilities;           /* Capability bitmask */
    uint32_t encoding;               /* objm_content_encoding_t */
    uint64_t logical_length;         /* Bytes exposed to clients */
    uint64_t storage_length;         /* Bytes stored on disk */
    uint64_t range_granularity;      /* Chunk size for range-ready variants */
    uint8_t is_primary;              /* Non-zero if primary/default variant */
    uint8_t reserved[7];             /* Future use/padding */
} objm_variant_descriptor_t;

/* Payload descriptor aggregating up to OBJM_MAX_VARIANTS */
typedef struct objm_payload_descriptor {
    uint32_t version;                /* Schema version */
    uint32_t variant_count;          /* Active variants */
    uint32_t manifest_flags;         /* OBJM_PAYLOAD_FLAG_* */
    uint32_t reserved;               /* Alignment/padding */
    objm_variant_descriptor_t variants[OBJM_MAX_VARIANTS];
} objm_payload_descriptor_t;

void objm_payload_descriptor_init(objm_payload_descriptor_t *descriptor);
void objm_payload_descriptor_copy(objm_payload_descriptor_t *dst,
                                  const objm_payload_descriptor_t *src);
int objm_payload_descriptor_validate(const objm_payload_descriptor_t *descriptor,
                                     char *error_buf,
                                     size_t error_buf_len);

#ifdef __cplusplus
}
#endif

#endif /* OBJMAPPER_METADATA_SCHEMA_H */
