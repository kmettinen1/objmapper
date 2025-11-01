/**
 * @file protocol.h
 * @brief objmapper wire protocol implementation
 * 
 * Simple, efficient protocol library for persistent connections with optional
 * out-of-order (OOO) reply support.
 */

#ifndef OBJMAPPER_PROTOCOL_H
#define OBJMAPPER_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <objmapper/metadata.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

#define OBJM_MAGIC "OBJM"
#define OBJM_MAGIC_LEN 4

#define OBJM_VERSION_1 0x01
#define OBJM_VERSION_2 0x02

/* Capability flags */
#define OBJM_CAP_OOO_REPLIES    0x0001  /* Can handle out-of-order responses */
#define OBJM_CAP_PIPELINING     0x0002  /* Can send pipelined requests */
#define OBJM_CAP_COMPRESSION    0x0004  /* Reserved for future */
#define OBJM_CAP_MULTIPLEXING   0x0008  /* Reserved for future */
#define OBJM_CAP_SEGMENTED_DELIVERY 0x0010 /* Supports mixed segment responses */

/* Request flags */
#define OBJM_REQ_ORDERED   0x01  /* Force in-order response */
#define OBJM_REQ_PRIORITY  0x02  /* High priority request */

/* Message types */
#define OBJM_MSG_REQUEST    0x01
#define OBJM_MSG_RESPONSE   0x02
#define OBJM_MSG_CLOSE      0x03
#define OBJM_MSG_CLOSE_ACK  0x04
#define OBJM_MSG_SEGMENTED_RESPONSE 0x05

/* Status codes */
#define OBJM_STATUS_OK              0x00

/* Client errors (4xx equivalent) */
#define OBJM_STATUS_NOT_FOUND       0x01
#define OBJM_STATUS_INVALID_REQUEST 0x02
#define OBJM_STATUS_INVALID_MODE    0x03
#define OBJM_STATUS_URI_TOO_LONG    0x04
#define OBJM_STATUS_UNSUPPORTED_OP  0x05

/* Server errors (5xx equivalent) */
#define OBJM_STATUS_INTERNAL_ERROR  0x10
#define OBJM_STATUS_STORAGE_ERROR   0x11
#define OBJM_STATUS_OUT_OF_MEMORY   0x12
#define OBJM_STATUS_TIMEOUT         0x13
#define OBJM_STATUS_UNAVAILABLE     0x14

/* Protocol errors */
#define OBJM_STATUS_PROTOCOL_ERROR  0x20
#define OBJM_STATUS_VERSION_MISMATCH 0x21
#define OBJM_STATUS_CAPABILITY_ERROR 0x22

/* Operation modes */
#define OBJM_MODE_FDPASS '1'
#define OBJM_MODE_COPY   '2'
#define OBJM_MODE_SPLICE '3'
#define OBJM_MODE_SEGMENTED '4'

/* Metadata types */
#define OBJM_META_SIZE      0x01  /* File size (8 bytes) */
#define OBJM_META_MTIME     0x02  /* Modification time (8 bytes) */
#define OBJM_META_ETAG      0x03  /* ETag (variable string) */
#define OBJM_META_MIME      0x04  /* MIME type (variable string) */
#define OBJM_META_BACKEND   0x05  /* Backend path ID (1 byte) */
#define OBJM_META_LATENCY   0x06  /* Processing latency (4 bytes, μs) */
#define OBJM_META_PAYLOAD   0x07  /* Payload descriptor blob */
#define OBJM_META_SEGMENT_HINTS 0x08 /* Segment prefetch hints */

/* Close reasons */
#define OBJM_CLOSE_NORMAL    0x00
#define OBJM_CLOSE_TIMEOUT   0x01
#define OBJM_CLOSE_ERROR     0x02
#define OBJM_CLOSE_SHUTDOWN  0x03

/* Limits */
#define OBJM_MAX_URI_LEN     4096
#define OBJM_MAX_PIPELINE    1000
#define OBJM_MAX_METADATA    1024

/* ============================================================================
 * Types
 * ============================================================================ */

/**
 * Protocol version
 */
typedef enum {
    OBJM_PROTO_V1 = OBJM_VERSION_1,  /* Simple ordered */
    OBJM_PROTO_V2 = OBJM_VERSION_2   /* Pipelined with OOO */
} objm_version_t;

/**
 * Connection handle (opaque)
 */
typedef struct objm_connection objm_connection_t;

/**
 * Request handle
 */
typedef struct {
    uint32_t id;           /* Request ID (V2 only) */
    uint8_t flags;         /* Request flags */
    char mode;             /* Operation mode ('1', '2', '3') */
    char *uri;             /* URI string (caller owns) */
    size_t uri_len;        /* URI length */
} objm_request_t;

/* Segment encoding ------------------------------------------------------- */

#define OBJM_SEGMENT_HEADER_WIRE_SIZE 32U
#define OBJM_MAX_SEGMENTS 64U

#define OBJM_SEG_TYPE_INLINE 0U
#define OBJM_SEG_TYPE_FD     1U
#define OBJM_SEG_TYPE_SPLICE 2U

#define OBJM_SEG_FLAG_FIN       0x01
#define OBJM_SEG_FLAG_REUSE_FD  0x02
#define OBJM_SEG_FLAG_OPTIONAL  0x04

typedef struct {
    uint8_t type;            /* OBJM_SEG_TYPE_* */
    uint8_t flags;           /* OBJM_SEG_FLAG_* */
    uint32_t copy_length;    /* Bytes of inline payload */
    uint64_t logical_length; /* Bytes contributed to client body */
    uint64_t storage_offset; /* Offset within referenced FD */
    uint64_t storage_length; /* Bytes available from FD */
    uint8_t *inline_data;    /* Inline bytes (type INLINE) */
    int fd;                  /* FD/SPLICE segment handle */
    int owns_fd;             /* Close FD during response_free */
} objm_segment_t;

/**
 * Response handle
 */
typedef struct {
    uint32_t request_id;   /* Matching request ID */
    uint8_t status;        /* Status code */
    int fd;                /* File descriptor (for FD pass mode, -1 otherwise) */
    objm_segment_t *segments; /* Segments for segmented delivery */
    uint16_t segment_count;   /* Number of active segments */
    size_t content_len;    /* Content length (for copy/splice modes) */
    uint8_t *metadata;     /* Optional metadata (caller must free) */
    size_t metadata_len;   /* Metadata length */
    char *error_msg;       /* Error message if status != OK (caller must free) */
} objm_response_t;

/**
 * Hello configuration (V2)
 */
typedef struct {
    uint16_t capabilities;      /* Client/server capabilities */
    uint16_t max_pipeline;      /* Max concurrent requests */
    uint8_t backend_parallelism; /* Server only: backend paths */
} objm_hello_t;

/**
 * Negotiated connection parameters
 */
typedef struct {
    objm_version_t version;     /* Negotiated version */
    uint16_t capabilities;      /* Negotiated capabilities */
    uint16_t max_pipeline;      /* Negotiated pipeline depth */
    uint8_t backend_parallelism; /* Server backend paths */
} objm_params_t;

/**
 * Metadata entry
 */
typedef struct {
    uint8_t type;          /* Metadata type */
    uint16_t length;       /* Data length */
    uint8_t *data;         /* Data bytes (caller must free) */
} objm_metadata_entry_t;

/**
 * Connection callbacks (for async operation)
 */
typedef struct {
    /* Called when request is ready to send */
    void (*on_send_ready)(objm_connection_t *conn, void *user_data);
    
    /* Called when response arrives */
    void (*on_response)(objm_connection_t *conn, objm_response_t *resp, void *user_data);
    
    /* Called on error */
    void (*on_error)(objm_connection_t *conn, const char *error, void *user_data);
    
    /* Called on connection close */
    void (*on_close)(objm_connection_t *conn, uint8_t reason, void *user_data);
} objm_callbacks_t;

/* ============================================================================
 * Client API
 * ============================================================================ */

/**
 * Create a new client connection
 * 
 * @param fd Socket file descriptor (caller manages ownership)
 * @param version Protocol version to use (OBJM_PROTO_V1 or V2)
 * @return Connection handle, or NULL on error
 */
objm_connection_t *objm_client_create(int fd, objm_version_t version);

/**
 * Perform handshake (V2 only)
 * 
 * @param conn Connection handle
 * @param hello Client hello parameters
 * @param params Output: negotiated parameters (can be NULL)
 * @return 0 on success, -1 on error
 */
int objm_client_hello(objm_connection_t *conn, const objm_hello_t *hello, 
                      objm_params_t *params);

/**
 * Send a request
 * 
 * @param conn Connection handle
 * @param req Request to send
 * @return 0 on success, -1 on error
 */
int objm_client_send_request(objm_connection_t *conn, const objm_request_t *req);

/**
 * Receive a response (blocking)
 * 
 * @param conn Connection handle
 * @param resp Output: response (caller must free with objm_response_free)
 * @return 0 on success, -1 on error
 */
int objm_client_recv_response(objm_connection_t *conn, objm_response_t **resp);

/**
 * Receive a specific response by request ID (V2 OOO mode)
 * 
 * @param conn Connection handle
 * @param request_id Request ID to wait for
 * @param resp Output: response (caller must free)
 * @return 0 on success, -1 on error
 */
int objm_client_recv_response_for(objm_connection_t *conn, uint32_t request_id,
                                   objm_response_t **resp);

/**
 * Close connection gracefully
 * 
 * @param conn Connection handle
 * @param reason Close reason
 * @return 0 on success, -1 on error
 */
int objm_client_close(objm_connection_t *conn, uint8_t reason);

/**
 * Destroy connection (frees resources, does NOT close socket)
 * 
 * @param conn Connection handle
 */
void objm_client_destroy(objm_connection_t *conn);

/* ============================================================================
 * Server API
 * ============================================================================ */

/**
 * Create a new server connection
 * 
 * @param fd Socket file descriptor (caller manages ownership)
 * @return Connection handle, or NULL on error
 */
objm_connection_t *objm_server_create(int fd);

/**
 * Detect protocol version and perform handshake if needed
 * 
 * @param conn Connection handle
 * @param hello Server hello parameters (for V2)
 * @param params Output: negotiated parameters (can be NULL)
 * @return 0 on success, -1 on error
 */
int objm_server_handshake(objm_connection_t *conn, const objm_hello_t *hello,
                          objm_params_t *params);

/**
 * Receive a request (blocking)
 * 
 * @param conn Connection handle
 * @param req Output: request (caller must free with objm_request_free)
 * @return 0 on success, -1 on error, 1 on connection close
 */
int objm_server_recv_request(objm_connection_t *conn, objm_request_t **req);

/**
 * Send a response
 * 
 * @param conn Connection handle
 * @param resp Response to send
 * @return 0 on success, -1 on error
 */
int objm_server_send_response(objm_connection_t *conn, const objm_response_t *resp);

/**
 * Send an error response
 * 
 * @param conn Connection handle
 * @param request_id Request ID (V2 only, use 0 for V1)
 * @param status Error status code
 * @param error_msg Error message (can be NULL)
 * @return 0 on success, -1 on error
 */
int objm_server_send_error(objm_connection_t *conn, uint32_t request_id,
                           uint8_t status, const char *error_msg);

/**
 * Send close acknowledgment
 * 
 * @param conn Connection handle
 * @param outstanding Number of pending responses
 * @return 0 on success, -1 on error
 */
int objm_server_send_close_ack(objm_connection_t *conn, uint32_t outstanding);

/**
 * Destroy server connection (frees resources, does NOT close socket)
 * 
 * @param conn Connection handle
 */
void objm_server_destroy(objm_connection_t *conn);

/* ============================================================================
 * Common Utilities
 * ============================================================================ */

/**
 * Get negotiated connection parameters
 * 
 * @param conn Connection handle
 * @param params Output: parameters
 * @return 0 on success, -1 on error
 */
int objm_get_params(objm_connection_t *conn, objm_params_t *params);

/**
 * Check if capability is enabled
 * 
 * @param conn Connection handle
 * @param capability Capability flag to check
 * @return 1 if enabled, 0 if not
 */
int objm_has_capability(objm_connection_t *conn, uint16_t capability);

/**
 * Get socket file descriptor
 * 
 * @param conn Connection handle
 * @return Socket FD, or -1 on error
 */
int objm_get_fd(objm_connection_t *conn);

/**
 * Set connection callbacks (for async operation)
 * 
 * @param conn Connection handle
 * @param callbacks Callback structure
 * @param user_data User data passed to callbacks
 */
void objm_set_callbacks(objm_connection_t *conn, const objm_callbacks_t *callbacks,
                        void *user_data);

/**
 * Free request structure
 * 
 * @param req Request to free
 */
void objm_request_free(objm_request_t *req);

/**
 * Free response structure
 * 
 * @param resp Response to free
 */
void objm_response_free(objm_response_t *resp);

/* ============================================================================
 * Metadata Utilities
 * ============================================================================ */

/**
 * Create metadata buffer
 * 
 * @param estimated_size Estimated total size (for allocation)
 * @return Metadata buffer, or NULL on error
 */
uint8_t *objm_metadata_create(size_t estimated_size);

/**
 * Add metadata entry
 * 
 * @param metadata Metadata buffer
 * @param current_len Current metadata length
 * @param type Metadata type
 * @param data Entry data
 * @param len Entry data length
 * @return New metadata length, or 0 on error
 */
size_t objm_metadata_add(uint8_t *metadata, size_t current_len,
                         uint8_t type, const void *data, size_t len);

/**
 * Add size metadata
 */
size_t objm_metadata_add_size(uint8_t *metadata, size_t current_len, uint64_t size);

/**
 * Add mtime metadata
 */
size_t objm_metadata_add_mtime(uint8_t *metadata, size_t current_len, uint64_t mtime);

/**
 * Add backend ID metadata
 */
size_t objm_metadata_add_backend(uint8_t *metadata, size_t current_len, uint8_t backend_id);

/**
 * Add payload descriptor metadata
 */
size_t objm_metadata_add_payload(uint8_t *metadata, size_t current_len,
                                 const objm_payload_descriptor_t *payload);

/**
 * Parse metadata buffer into entries
 * 
 * @param metadata Metadata buffer
 * @param metadata_len Metadata length
 * @param entries Output: array of entries (caller must free)
 * @param num_entries Output: number of entries
 * @return 0 on success, -1 on error
 */
int objm_metadata_parse(const uint8_t *metadata, size_t metadata_len,
                        objm_metadata_entry_t **entries, size_t *num_entries);

/**
 * Get metadata entry by type
 * 
 * @param entries Entry array
 * @param num_entries Number of entries
 * @param type Metadata type to find
 * @return Entry pointer, or NULL if not found
 */
const objm_metadata_entry_t *objm_metadata_get(const objm_metadata_entry_t *entries,
                                                size_t num_entries, uint8_t type);

/**
 * Extract payload descriptor if present.
 *
 * @param entries Metadata entry array
 * @param num_entries Entry count
 * @param out_payload Output descriptor (must be non-NULL)
 * @return 0 on success, 1 if not found, -1 on error
 */
int objm_metadata_get_payload(const objm_metadata_entry_t *entries,
                              size_t num_entries,
                              objm_payload_descriptor_t *out_payload);

/**
 * Free metadata entries
 * 
 * @param entries Entry array
 * @param num_entries Number of entries
 */
void objm_metadata_free_entries(objm_metadata_entry_t *entries, size_t num_entries);

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/**
 * Get status code name
 * 
 * @param status Status code
 * @return Status name string
 */
const char *objm_status_name(uint8_t status);

/**
 * Get mode name
 * 
 * @param mode Mode character
 * @return Mode name string
 */
const char *objm_mode_name(char mode);

/**
 * Get capability names
 * 
 * @param capabilities Capability flags
 * @param buffer Output buffer
 * @param size Buffer size
 * @return Number of bytes written
 */
int objm_capability_names(uint16_t capabilities, char *buffer, size_t size);

/**
 * Retrieve last error message
 *
 * @param conn Connection handle
 * @return Error string or NULL
 */
const char *objm_last_error(const objm_connection_t *conn);

#ifdef __cplusplus
}
#endif

#endif /* OBJMAPPER_PROTOCOL_H */
