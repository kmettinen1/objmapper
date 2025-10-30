/*-
 * Copyright (c) 2025 The objmapper Authors
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef OBJMAPPER_METADATA_H
#define OBJMAPPER_METADATA_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <endian.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OBJM_PAYLOAD_DESCRIPTOR_VERSION 1U
#define OBJM_MAX_VARIANTS 8U
#define OBJM_VARIANT_ID_MAX 32U

/* Content encoding identifiers shared with objmapper */
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

/* Payload manifest flags */
#define OBJM_PAYLOAD_FLAG_HAS_VARIANTS    (1u << 0)
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

/* Payload descriptor aggregating variants */
typedef struct objm_payload_descriptor {
    uint32_t version;                /* Schema version */
    uint32_t variant_count;          /* Active variants */
    uint32_t manifest_flags;         /* OBJM_PAYLOAD_FLAG_* */
    uint32_t reserved;               /* Alignment/padding */
    objm_variant_descriptor_t variants[OBJM_MAX_VARIANTS];
} objm_payload_descriptor_t;

#define OBJM_VARIANT_DESCRIPTOR_WIRE_SIZE \
    (OBJM_VARIANT_ID_MAX + sizeof(uint32_t) * 2 + sizeof(uint64_t) * 3 + \
     sizeof(uint8_t) + sizeof(((objm_variant_descriptor_t *)0)->reserved))

#define OBJM_PAYLOAD_DESCRIPTOR_WIRE_SIZE \
    (sizeof(uint32_t) * 4 + OBJM_MAX_VARIANTS * OBJM_VARIANT_DESCRIPTOR_WIRE_SIZE)

void objm_payload_descriptor_init(objm_payload_descriptor_t *descriptor);
void objm_payload_descriptor_copy(objm_payload_descriptor_t *dst,
                                  const objm_payload_descriptor_t *src);
int objm_payload_descriptor_validate(const objm_payload_descriptor_t *descriptor,
                                     char *error_buf,
                                     size_t error_buf_len);

static inline size_t objm_payload_descriptor_wire_size(void) {
    return OBJM_PAYLOAD_DESCRIPTOR_WIRE_SIZE;
}

static inline int objm_payload_descriptor_encode(const objm_payload_descriptor_t *descriptor,
                                                 uint8_t *buffer,
                                                 size_t buffer_len) {
    if (!descriptor || !buffer || buffer_len < OBJM_PAYLOAD_DESCRIPTOR_WIRE_SIZE) {
        return -1;
    }

    if (descriptor->variant_count > OBJM_MAX_VARIANTS) {
        return -1;
    }

    memset(buffer, 0, OBJM_PAYLOAD_DESCRIPTOR_WIRE_SIZE);

    uint8_t *p = buffer;
    uint32_t field32;
    uint64_t field64;

    field32 = htole32(descriptor->version);
    memcpy(p, &field32, sizeof(field32));
    p += sizeof(field32);

    field32 = htole32(descriptor->variant_count);
    memcpy(p, &field32, sizeof(field32));
    p += sizeof(field32);

    field32 = htole32(descriptor->manifest_flags);
    memcpy(p, &field32, sizeof(field32));
    p += sizeof(field32);

    field32 = htole32(descriptor->reserved);
    memcpy(p, &field32, sizeof(field32));
    p += sizeof(field32);

    for (size_t i = 0; i < OBJM_MAX_VARIANTS; i++) {
        const objm_variant_descriptor_t *variant = &descriptor->variants[i];

        memcpy(p, variant->variant_id, OBJM_VARIANT_ID_MAX);
        p += OBJM_VARIANT_ID_MAX;

        field32 = htole32(variant->capabilities);
        memcpy(p, &field32, sizeof(field32));
        p += sizeof(field32);

        field32 = htole32(variant->encoding);
        memcpy(p, &field32, sizeof(field32));
        p += sizeof(field32);

        field64 = htole64(variant->logical_length);
        memcpy(p, &field64, sizeof(field64));
        p += sizeof(field64);

        field64 = htole64(variant->storage_length);
        memcpy(p, &field64, sizeof(field64));
        p += sizeof(field64);

        field64 = htole64(variant->range_granularity);
        memcpy(p, &field64, sizeof(field64));
        p += sizeof(field64);

        *p++ = variant->is_primary;
        memcpy(p, variant->reserved, sizeof(variant->reserved));
        p += sizeof(variant->reserved);
    }

    return 0;
}

static inline int objm_payload_descriptor_decode(const uint8_t *buffer,
                                                 size_t buffer_len,
                                                 objm_payload_descriptor_t *descriptor) {
    if (!buffer || !descriptor || buffer_len < OBJM_PAYLOAD_DESCRIPTOR_WIRE_SIZE) {
        return -1;
    }

    const uint8_t *p = buffer;
    uint32_t field32;
    uint64_t field64;

    objm_payload_descriptor_init(descriptor);

    memcpy(&field32, p, sizeof(field32));
    descriptor->version = le32toh(field32);
    p += sizeof(field32);

    memcpy(&field32, p, sizeof(field32));
    descriptor->variant_count = le32toh(field32);
    p += sizeof(field32);

    if (descriptor->variant_count > OBJM_MAX_VARIANTS) {
        return -1;
    }

    memcpy(&field32, p, sizeof(field32));
    descriptor->manifest_flags = le32toh(field32);
    p += sizeof(field32);

    memcpy(&field32, p, sizeof(field32));
    descriptor->reserved = le32toh(field32);
    p += sizeof(field32);

    for (size_t i = 0; i < OBJM_MAX_VARIANTS; i++) {
        objm_variant_descriptor_t *variant = &descriptor->variants[i];

        memcpy(variant->variant_id, p, OBJM_VARIANT_ID_MAX);
        variant->variant_id[OBJM_VARIANT_ID_MAX - 1] = '\0';
        p += OBJM_VARIANT_ID_MAX;

        memcpy(&field32, p, sizeof(field32));
        variant->capabilities = le32toh(field32);
        p += sizeof(field32);

        memcpy(&field32, p, sizeof(field32));
        variant->encoding = le32toh(field32);
        p += sizeof(field32);

        memcpy(&field64, p, sizeof(field64));
        variant->logical_length = le64toh(field64);
        p += sizeof(field64);

        memcpy(&field64, p, sizeof(field64));
        variant->storage_length = le64toh(field64);
        p += sizeof(field64);

        memcpy(&field64, p, sizeof(field64));
        variant->range_granularity = le64toh(field64);
        p += sizeof(field64);

        variant->is_primary = *p++;
        memcpy(variant->reserved, p, sizeof(variant->reserved));
        p += sizeof(variant->reserved);
    }

    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* OBJMAPPER_METADATA_H */
