/*
 * @file metadata_schema.c
 * @brief Validation helpers for objmapper payload metadata descriptors
 */

#include "metadata_schema.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void set_error(char *buf, size_t len, const char *fmt, ...) {
    if (!buf || len == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, len, fmt, ap);
    va_end(ap);
}

void objm_payload_descriptor_init(objm_payload_descriptor_t *descriptor) {
    if (!descriptor) {
        return;
    }
    memset(descriptor, 0, sizeof(*descriptor));
    descriptor->version = OBJM_PAYLOAD_DESCRIPTOR_VERSION;
}

void objm_payload_descriptor_copy(objm_payload_descriptor_t *dst,
                                  const objm_payload_descriptor_t *src) {
    if (!dst || !src) {
        return;
    }
    memcpy(dst, src, sizeof(*dst));
}

static int validate_variant(const objm_variant_descriptor_t *variant,
                            char *error_buf,
                            size_t error_buf_len,
                            size_t index,
                            int *primary_count) {
    if (!variant) {
        set_error(error_buf, error_buf_len, "variant[%zu]: descriptor is NULL", index);
        return -1;
    }

    if (variant->variant_id[0] == '\0') {
        set_error(error_buf, error_buf_len, "variant[%zu]: variant_id missing", index);
        return -1;
    }

    if (variant->logical_length == 0) {
        set_error(error_buf, error_buf_len, "variant[%zu]: logical_length must be > 0", index);
        return -1;
    }

    if (variant->storage_length == 0) {
        set_error(error_buf, error_buf_len, "variant[%zu]: storage_length must be > 0", index);
        return -1;
    }

    if (variant->encoding == OBJM_ENCODING_IDENTITY &&
        variant->storage_length < variant->logical_length) {
        set_error(error_buf, error_buf_len,
                  "variant[%zu]: storage_length (%llu) < logical_length (%llu) for identity encoding",
                  index,
                  (unsigned long long)variant->storage_length,
                  (unsigned long long)variant->logical_length);
        return -1;
    }

    if (variant->encoding != OBJM_ENCODING_IDENTITY &&
        variant->encoding != OBJM_ENCODING_GZIP &&
        variant->encoding != OBJM_ENCODING_BROTLI &&
        variant->encoding != OBJM_ENCODING_ZSTD &&
        variant->encoding != OBJM_ENCODING_CUSTOM) {
        set_error(error_buf, error_buf_len, "variant[%zu]: unsupported encoding %u", index, variant->encoding);
        return -1;
    }

    if ((variant->capabilities & OBJM_CAP_IDENTITY) && variant->encoding != OBJM_ENCODING_IDENTITY) {
        set_error(error_buf, error_buf_len,
                  "variant[%zu]: OBJM_CAP_IDENTITY requires identity encoding", index);
        return -1;
    }

    if ((variant->capabilities & OBJM_CAP_GZIP) && variant->encoding != OBJM_ENCODING_GZIP) {
        set_error(error_buf, error_buf_len,
                  "variant[%zu]: OBJM_CAP_GZIP requires gzip encoding", index);
        return -1;
    }

    if ((variant->capabilities & OBJM_CAP_RANGE_READY) && variant->range_granularity == 0) {
        set_error(error_buf, error_buf_len,
                  "variant[%zu]: range-ready capability requires range_granularity", index);
        return -1;
    }

    if (variant->is_primary) {
        (*primary_count)++;
    }

    return 0;
}

int objm_payload_descriptor_validate(const objm_payload_descriptor_t *descriptor,
                                     char *error_buf,
                                     size_t error_buf_len) {
    if (!descriptor) {
        set_error(error_buf, error_buf_len, "descriptor is NULL");
        return -1;
    }

    if (descriptor->version != OBJM_PAYLOAD_DESCRIPTOR_VERSION) {
        set_error(error_buf, error_buf_len,
                  "unexpected version %u (expected %u)",
                  descriptor->version,
                  OBJM_PAYLOAD_DESCRIPTOR_VERSION);
        return -1;
    }

    if (descriptor->variant_count == 0) {
        set_error(error_buf, error_buf_len, "variant_count must be > 0");
        return -1;
    }

    if (descriptor->variant_count > OBJM_MAX_VARIANTS) {
        set_error(error_buf, error_buf_len,
                  "variant_count (%u) exceeds OBJM_MAX_VARIANTS (%u)",
                  descriptor->variant_count,
                  OBJM_MAX_VARIANTS);
        return -1;
    }

    int primary_count = 0;

    for (uint32_t i = 0; i < descriptor->variant_count; i++) {
        if (validate_variant(&descriptor->variants[i], error_buf, error_buf_len, i, &primary_count) < 0) {
            return -1;
        }
    }

    if (primary_count == 0) {
        set_error(error_buf, error_buf_len, "no primary variant defined");
        return -1;
    }

    if (primary_count > 1) {
        set_error(error_buf, error_buf_len, "multiple primary variants defined (%d)", primary_count);
        return -1;
    }

    return 0;
}
