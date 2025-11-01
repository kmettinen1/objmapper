/**
 * @file protocol.c
 * @brief objmapper wire protocol implementation
 */

#include "protocol.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <limits.h>
#include <endian.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <arpa/inet.h>

/* ============================================================================
 * Internal structures
 * ============================================================================ */

struct objm_connection {
    int fd;                     /* Socket file descriptor */
    objm_version_t version;     /* Protocol version */
    objm_params_t params;       /* Negotiated parameters */
    int is_server;              /* 1 if server, 0 if client */
    uint32_t next_request_id;   /* Next request ID (client only) */
    
    /* OOO support */
    objm_response_t **pending_responses;  /* Pending responses by ID */
    size_t pending_capacity;              /* Capacity of pending array */
    
    /* Callbacks */
    objm_callbacks_t callbacks;
    void *user_data;
    
    /* Error state */
    char error[256];
};

/* ============================================================================
 * Internal helpers
 * ============================================================================ */

static int send_all(int fd, const void *buf, size_t len) {
    const uint8_t *ptr = buf;
    size_t remaining = len;
    
    while (remaining > 0) {
        ssize_t sent = write(fd, ptr, remaining);
        if (sent < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        ptr += sent;
        remaining -= sent;
    }
    return 0;
}

static int recv_all(int fd, void *buf, size_t len) {
    uint8_t *ptr = buf;
    size_t remaining = len;
    
    while (remaining > 0) {
        ssize_t received = read(fd, ptr, remaining);
        if (received < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (received == 0) {
            /* Connection closed */
            return -1;
        }
        ptr += received;
        remaining -= received;
    }
    return 0;
}

static int send_fd(int sock, int fd_to_send) {
    struct msghdr msg = {0};
    struct iovec iov[1];
    char control[CMSG_SPACE(sizeof(int))];
    char dummy = 'X';
    
    iov[0].iov_base = &dummy;
    iov[0].iov_len = 1;
    
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd_to_send, sizeof(int));
    
    if (sendmsg(sock, &msg, 0) < 0) {
        return -1;
    }
    return 0;
}

static int recv_fd(int sock) {
    struct msghdr msg = {0};
    struct iovec iov[1];
    char control[CMSG_SPACE(sizeof(int))];
    char dummy;
    
    iov[0].iov_base = &dummy;
    iov[0].iov_len = 1;
    
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    
    if (recvmsg(sock, &msg, 0) < 0) {
        return -1;
    }
    
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_type != SCM_RIGHTS) {
        return -1;
    }
    
    int fd;
    memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
    return fd;
}

static void set_error(objm_connection_t *conn, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(conn->error, sizeof(conn->error), fmt, args);
    va_end(args);
}

static void free_segments(objm_segment_t *segments, uint16_t count) {
    if (!segments) {
        return;
    }

    for (uint16_t i = 0; i < count; i++) {
        if (segments[i].owns_fd && segments[i].fd >= 0) {
            close(segments[i].fd);
        }
        free(segments[i].inline_data);
        segments[i].inline_data = NULL;
    }

    free(segments);
}

static int recv_segmented_payload(objm_connection_t *conn, objm_response_t *resp,
                                  uint16_t segment_count) {
    if (!conn || !resp) {
        return -1;
    }

    if (segment_count == 0 || segment_count > OBJM_MAX_SEGMENTS) {
        set_error(conn, "Invalid segment count: %u", segment_count);
        return -1;
    }

    size_t table_len = (size_t)segment_count * OBJM_SEGMENT_HEADER_WIRE_SIZE;
    uint8_t *table = malloc(table_len);
    if (!table) {
        set_error(conn, "Failed to allocate segment table");
        return -1;
    }

    if (recv_all(conn->fd, table, table_len) < 0) {
        free(table);
        set_error(conn, "Failed to receive segment table");
        return -1;
    }

    objm_segment_t *segments = calloc(segment_count, sizeof(*segments));
    if (!segments) {
        free(table);
        set_error(conn, "Failed to allocate segment array");
        return -1;
    }

    resp->segments = segments;
    resp->segment_count = segment_count;
    resp->content_len = 0;

    for (uint16_t i = 0; i < segment_count; i++) {
        const uint8_t *entry = table + (size_t)i * OBJM_SEGMENT_HEADER_WIRE_SIZE;
        objm_segment_t *seg = &segments[i];

        seg->type = entry[0];
        seg->flags = entry[1];
        seg->copy_length = ntohl(*(const uint32_t *)(entry + 4));
        seg->logical_length = be64toh(*(const uint64_t *)(entry + 8));
        seg->storage_offset = be64toh(*(const uint64_t *)(entry + 16));
        seg->storage_length = be64toh(*(const uint64_t *)(entry + 24));
        seg->fd = -1;
        seg->owns_fd = 0;

        switch (seg->type) {
            case OBJM_SEG_TYPE_INLINE:
                if (seg->copy_length != seg->logical_length) {
                    free(table);
                    set_error(conn, "Inline segment length mismatch");
                    goto error;
                }
                break;
            case OBJM_SEG_TYPE_FD:
            case OBJM_SEG_TYPE_SPLICE:
                if (seg->copy_length != 0) {
                    free(table);
                    set_error(conn, "Copy length must be zero for FD segments");
                    goto error;
                }
                if (seg->storage_length < seg->logical_length) {
                    free(table);
                    set_error(conn, "Storage length < logical length");
                    goto error;
                }
                break;
            default:
                free(table);
                set_error(conn, "Unknown segment type: %u", seg->type);
                goto error;
        }

        resp->content_len += seg->logical_length;
    }

    free(table);

    /* Receive inline payloads */
    for (uint16_t i = 0; i < segment_count; i++) {
        objm_segment_t *seg = &segments[i];
        if (seg->type != OBJM_SEG_TYPE_INLINE || seg->copy_length == 0) {
            continue;
        }

        seg->inline_data = malloc(seg->copy_length);
        if (!seg->inline_data) {
            set_error(conn, "Failed to allocate inline segment payload");
            goto error;
        }

        if (recv_all(conn->fd, seg->inline_data, seg->copy_length) < 0) {
            set_error(conn, "Failed to receive inline segment payload");
            goto error;
        }
    }

    /* Receive file descriptors */
    int last_fd = -1;
    for (uint16_t i = 0; i < segment_count; i++) {
        objm_segment_t *seg = &segments[i];

        if (seg->type != OBJM_SEG_TYPE_FD && seg->type != OBJM_SEG_TYPE_SPLICE) {
            continue;
        }

        if (seg->flags & OBJM_SEG_FLAG_REUSE_FD) {
            if (last_fd < 0) {
                set_error(conn, "FD reuse without prior descriptor");
                goto error;
            }
            seg->fd = last_fd;
            seg->owns_fd = 0;
            continue;
        }

        int fd = recv_fd(conn->fd);
        if (fd < 0) {
            set_error(conn, "Failed to receive file descriptor");
            goto error;
        }
        seg->fd = fd;
        seg->owns_fd = 1;
        last_fd = fd;
    }

    if (!(segments[segment_count - 1].flags & OBJM_SEG_FLAG_FIN)) {
        set_error(conn, "Final segment missing FIN flag");
        goto error;
    }

    return 0;

error:
    free_segments(segments, segment_count);
    resp->segments = NULL;
    resp->segment_count = 0;
    resp->content_len = 0;
    return -1;
}

static int send_segmented_payload(objm_connection_t *conn, const objm_response_t *resp) {
    if (!conn || !resp || resp->segment_count == 0 || !resp->segments) {
        set_error(conn, "Invalid segmented response");
        return -1;
    }

    if (resp->segment_count > OBJM_MAX_SEGMENTS) {
        set_error(conn, "Segment count exceeds limit");
        return -1;
    }

    size_t table_len = (size_t)resp->segment_count * OBJM_SEGMENT_HEADER_WIRE_SIZE;
    uint8_t *table = malloc(table_len);
    if (!table) {
        set_error(conn, "Failed to allocate segment table");
        return -1;
    }

    int last_fd = -1;

    for (uint16_t i = 0; i < resp->segment_count; i++) {
        const objm_segment_t *seg = &resp->segments[i];
        uint8_t *entry = table + (size_t)i * OBJM_SEGMENT_HEADER_WIRE_SIZE;

        entry[0] = seg->type;
        entry[1] = seg->flags;
        entry[2] = entry[3] = 0;
        *(uint32_t *)(entry + 4) = htonl(seg->copy_length);
        *(uint64_t *)(entry + 8) = htobe64(seg->logical_length);
        *(uint64_t *)(entry + 16) = htobe64(seg->storage_offset);
        *(uint64_t *)(entry + 24) = htobe64(seg->storage_length);

        switch (seg->type) {
            case OBJM_SEG_TYPE_INLINE:
                if (seg->copy_length != seg->logical_length) {
                    free(table);
                    set_error(conn, "Inline segment length mismatch");
                    return -1;
                }
                if (seg->copy_length && !seg->inline_data) {
                    free(table);
                    set_error(conn, "Inline segment missing payload");
                    return -1;
                }
                break;
            case OBJM_SEG_TYPE_FD:
            case OBJM_SEG_TYPE_SPLICE:
                if (!(seg->flags & OBJM_SEG_FLAG_REUSE_FD)) {
                    if (seg->fd < 0) {
                        free(table);
                        set_error(conn, "Segment missing file descriptor");
                        return -1;
                    }
                    last_fd = seg->fd;
                } else if (last_fd < 0) {
                    free(table);
                    set_error(conn, "FD reuse without prior descriptor");
                    return -1;
                }
                break;
            default:
                free(table);
                set_error(conn, "Unknown segment type: %u", seg->type);
                return -1;
        }
    }

    if (!(resp->segments[resp->segment_count - 1].flags & OBJM_SEG_FLAG_FIN)) {
        free(table);
        set_error(conn, "Final segment missing FIN flag");
        return -1;
    }

    if (send_all(conn->fd, table, table_len) < 0) {
        free(table);
        return -1;
    }
    free(table);

    /* Send inline payloads */
    for (uint16_t i = 0; i < resp->segment_count; i++) {
        const objm_segment_t *seg = &resp->segments[i];
        if (seg->type != OBJM_SEG_TYPE_INLINE || seg->copy_length == 0) {
            continue;
        }

        if (send_all(conn->fd, seg->inline_data, seg->copy_length) < 0) {
            return -1;
        }
    }

    /* Send file descriptors */
    last_fd = -1;
    for (uint16_t i = 0; i < resp->segment_count; i++) {
        const objm_segment_t *seg = &resp->segments[i];

        if (seg->type != OBJM_SEG_TYPE_FD && seg->type != OBJM_SEG_TYPE_SPLICE) {
            continue;
        }

        if (seg->flags & OBJM_SEG_FLAG_REUSE_FD) {
            if (last_fd < 0) {
                set_error(conn, "FD reuse without prior descriptor");
                return -1;
            }
            continue;
        }

        if (send_fd(conn->fd, seg->fd) < 0) {
            return -1;
        }
        last_fd = seg->fd;
    }

    return 0;
}

/* ============================================================================
 * Client API
 * ============================================================================ */

objm_connection_t *objm_client_create(int fd, objm_version_t version) {
    objm_connection_t *conn = calloc(1, sizeof(*conn));
    if (!conn) return NULL;
    
    conn->fd = fd;
    conn->version = version;
    conn->is_server = 0;
    conn->next_request_id = 1;
    
    /* Default params */
    conn->params.version = version;
    conn->params.capabilities = 0;
    conn->params.max_pipeline = 1;
    
    return conn;
}

int objm_client_hello(objm_connection_t *conn, const objm_hello_t *hello,
                      objm_params_t *params) {
    if (!conn || conn->version != OBJM_PROTO_V2) {
        return -1;
    }
    
    /* Send HELLO: magic(4) + version(1) + caps(2) + max_pipeline(2) */
    uint8_t hello_msg[9];
    memcpy(hello_msg, OBJM_MAGIC, OBJM_MAGIC_LEN);
    hello_msg[4] = OBJM_VERSION_2;
    *(uint16_t *)(hello_msg + 5) = htons(hello->capabilities);
    *(uint16_t *)(hello_msg + 7) = htons(hello->max_pipeline);
    
    if (send_all(conn->fd, hello_msg, sizeof(hello_msg)) < 0) {
        set_error(conn, "Failed to send HELLO");
        return -1;
    }
    
    /* Receive HELLO_ACK: magic(4) + version(1) + caps(2) + max_pipeline(2) + backend_parallelism(1) */
    uint8_t ack_msg[10];
    if (recv_all(conn->fd, ack_msg, sizeof(ack_msg)) < 0) {
        set_error(conn, "Failed to receive HELLO_ACK");
        return -1;
    }
    
    /* Validate magic and version */
    if (memcmp(ack_msg, OBJM_MAGIC, OBJM_MAGIC_LEN) != 0) {
        set_error(conn, "Invalid HELLO_ACK magic");
        return -1;
    }
    
    if (ack_msg[4] != OBJM_VERSION_2) {
        set_error(conn, "Version mismatch");
        return -1;
    }
    
    /* Parse negotiated parameters */
    conn->params.version = OBJM_PROTO_V2;
    conn->params.capabilities = ntohs(*(uint16_t *)(ack_msg + 5));
    conn->params.max_pipeline = ntohs(*(uint16_t *)(ack_msg + 7));
    conn->params.backend_parallelism = ack_msg[9];
    
    /* Allocate pending response array for OOO */
    if (conn->params.capabilities & OBJM_CAP_OOO_REPLIES) {
        conn->pending_capacity = conn->params.max_pipeline;
        conn->pending_responses = calloc(conn->pending_capacity, sizeof(objm_response_t *));
        if (!conn->pending_responses) {
            set_error(conn, "Failed to allocate pending response buffer");
            return -1;
        }
    }
    
    if (params) {
        *params = conn->params;
    }
    
    return 0;
}

int objm_client_send_request(objm_connection_t *conn, const objm_request_t *req) {
    if (!conn || !req) return -1;
    
    if (conn->version == OBJM_PROTO_V1) {
        /* V1: mode(1) + uri_len(2) + uri */
        uint8_t header[3];
        header[0] = req->mode;
        *(uint16_t *)(header + 1) = htons(req->uri_len);
        
        if (send_all(conn->fd, header, sizeof(header)) < 0 ||
            send_all(conn->fd, req->uri, req->uri_len) < 0) {
            set_error(conn, "Failed to send V1 request");
            return -1;
        }
    } else {
        /* V2: msg_type(1) + request_id(4) + flags(1) + mode(1) + uri_len(2) + uri */
        uint8_t header[9];
        header[0] = OBJM_MSG_REQUEST;
        *(uint32_t *)(header + 1) = htonl(req->id);
        header[5] = req->flags;
        header[6] = req->mode;
        *(uint16_t *)(header + 7) = htons(req->uri_len);
        
        if (send_all(conn->fd, header, sizeof(header)) < 0 ||
            send_all(conn->fd, req->uri, req->uri_len) < 0) {
            set_error(conn, "Failed to send V2 request");
            return -1;
        }
    }
    
    return 0;
}

int objm_client_recv_response(objm_connection_t *conn, objm_response_t **resp) {
    if (!conn || !resp) {
        return -1;
    }

    objm_response_t *r = calloc(1, sizeof(*r));
    if (!r) {
        return -1;
    }

    r->fd = -1;

    int is_segmented = 0;
    uint16_t segment_count = 0;

    if (conn->version == OBJM_PROTO_V1) {
        uint8_t header[11];
        if (recv_all(conn->fd, header, sizeof(header)) < 0) {
            free(r);
            set_error(conn, "Failed to receive V1 response header");
            return -1;
        }

        r->status = header[0];
        r->content_len = be64toh(*(uint64_t *)(header + 1));
        r->metadata_len = ntohs(*(uint16_t *)(header + 9));
    } else {
        uint8_t base[6];
        if (recv_all(conn->fd, base, sizeof(base)) < 0) {
            free(r);
            set_error(conn, "Failed to receive V2 response header");
            return -1;
        }

        uint8_t msg_type = base[0];
        r->request_id = ntohl(*(uint32_t *)(base + 1));
        r->status = base[5];

        if (msg_type == OBJM_MSG_RESPONSE) {
            uint8_t tail[10];
            if (recv_all(conn->fd, tail, sizeof(tail)) < 0) {
                free(r);
                set_error(conn, "Failed to receive response detail");
                return -1;
            }
            r->content_len = be64toh(*(uint64_t *)(tail));
            r->metadata_len = ntohs(*(uint16_t *)(tail + 8));
        } else if (msg_type == OBJM_MSG_SEGMENTED_RESPONSE) {
            if (!(conn->params.capabilities & OBJM_CAP_SEGMENTED_DELIVERY)) {
                free(r);
                set_error(conn, "Segmented response without capability");
                return -1;
            }

            uint8_t tail[4];
            if (recv_all(conn->fd, tail, sizeof(tail)) < 0) {
                free(r);
                set_error(conn, "Failed to receive segmented response detail");
                return -1;
            }
            segment_count = ntohs(*(uint16_t *)(tail));
            r->metadata_len = ntohs(*(uint16_t *)(tail + 2));
            is_segmented = 1;
        } else {
            free(r);
            set_error(conn, "Unexpected message type: %d", msg_type);
            return -1;
        }
    }

    if (r->metadata_len > 0) {
        r->metadata = malloc(r->metadata_len);
        if (!r->metadata) {
            free(r);
            return -1;
        }
        if (recv_all(conn->fd, r->metadata, r->metadata_len) < 0) {
            free(r->metadata);
            free(r);
            set_error(conn, "Failed to receive metadata");
            return -1;
        }
    }

    if (is_segmented) {
        if (recv_segmented_payload(conn, r, segment_count) < 0) {
            free(r->metadata);
            free(r);
            return -1;
        }
    } else if (r->status == OBJM_STATUS_OK && r->content_len == 0) {
        r->fd = recv_fd(conn->fd);
        if (r->fd < 0) {
            free(r->metadata);
            free(r);
            set_error(conn, "Failed to receive file descriptor");
            return -1;
        }
    }

    *resp = r;
    return 0;
}

int objm_client_recv_response_for(objm_connection_t *conn, uint32_t request_id,
                                   objm_response_t **resp) {
    if (!conn || !resp || conn->version != OBJM_PROTO_V2) {
        return -1;
    }
    
    /* Check if already received and pending */
    if (request_id < conn->pending_capacity && conn->pending_responses[request_id]) {
        *resp = conn->pending_responses[request_id];
        conn->pending_responses[request_id] = NULL;
        return 0;
    }
    
    /* Keep receiving until we get our response */
    while (1) {
        objm_response_t *r;
        if (objm_client_recv_response(conn, &r) < 0) {
            return -1;
        }
        
        if (r->request_id == request_id) {
            *resp = r;
            return 0;
        }
        
        /* Store for later */
        if (r->request_id < conn->pending_capacity) {
            conn->pending_responses[r->request_id] = r;
        } else {
            /* ID out of range, discard */
            objm_response_free(r);
        }
    }
}

int objm_client_close(objm_connection_t *conn, uint8_t reason) {
    if (!conn) return -1;
    
    if (conn->version == OBJM_PROTO_V2) {
        uint8_t close_msg[2] = {OBJM_MSG_CLOSE, reason};
        if (send_all(conn->fd, close_msg, sizeof(close_msg)) < 0) {
            return -1;
        }
        
        /* Wait for CLOSE_ACK */
        uint8_t ack[6];
        if (recv_all(conn->fd, ack, sizeof(ack)) < 0) {
            return -1;
        }
        
        if (ack[0] != OBJM_MSG_CLOSE_ACK) {
            return -1;
        }
    }
    
    return 0;
}

void objm_client_destroy(objm_connection_t *conn) {
    if (!conn) return;
    
    if (conn->pending_responses) {
        for (size_t i = 0; i < conn->pending_capacity; i++) {
            if (conn->pending_responses[i]) {
                objm_response_free(conn->pending_responses[i]);
            }
        }
        free(conn->pending_responses);
    }
    
    free(conn);
}

/* ============================================================================
 * Server API
 * ============================================================================ */

objm_connection_t *objm_server_create(int fd) {
    objm_connection_t *conn = calloc(1, sizeof(*conn));
    if (!conn) return NULL;
    
    conn->fd = fd;
    conn->is_server = 1;
    
    return conn;
}

int objm_server_handshake(objm_connection_t *conn, const objm_hello_t *hello,
                          objm_params_t *params) {
    if (!conn || !conn->is_server) return -1;
    
    /* Peek first byte to detect version */
    uint8_t first_byte;
    ssize_t n = recv(conn->fd, &first_byte, 1, MSG_PEEK);
    if (n != 1) {
        set_error(conn, "Failed to peek first byte");
        return -1;
    }
    
    /* Check for V2 HELLO (starts with 'O' from "OBJM") */
    if (first_byte == 'O') {
        /* V2 handshake */
        uint8_t hello_msg[9];
        if (recv_all(conn->fd, hello_msg, sizeof(hello_msg)) < 0) {
            set_error(conn, "Failed to receive HELLO");
            return -1;
        }
        
        /* Validate */
        if (memcmp(hello_msg, OBJM_MAGIC, OBJM_MAGIC_LEN) != 0 ||
            hello_msg[4] != OBJM_VERSION_2) {
            set_error(conn, "Invalid HELLO message");
            return -1;
        }
        
        uint16_t client_caps = ntohs(*(uint16_t *)(hello_msg + 5));
        uint16_t client_pipeline = ntohs(*(uint16_t *)(hello_msg + 7));
        
        /* Negotiate capabilities */
        conn->params.version = OBJM_PROTO_V2;
        conn->params.capabilities = client_caps & hello->capabilities;
        conn->params.max_pipeline = (client_pipeline < hello->max_pipeline) ?
                                     client_pipeline : hello->max_pipeline;
        conn->params.backend_parallelism = hello->backend_parallelism;
        
        /* Send HELLO_ACK */
        uint8_t ack_msg[10];
        memcpy(ack_msg, OBJM_MAGIC, OBJM_MAGIC_LEN);
        ack_msg[4] = OBJM_VERSION_2;
        *(uint16_t *)(ack_msg + 5) = htons(conn->params.capabilities);
        *(uint16_t *)(ack_msg + 7) = htons(conn->params.max_pipeline);
        ack_msg[9] = conn->params.backend_parallelism;
        
        if (send_all(conn->fd, ack_msg, sizeof(ack_msg)) < 0) {
            set_error(conn, "Failed to send HELLO_ACK");
            return -1;
        }
        
        conn->version = OBJM_PROTO_V2;
    } else {
        /* V1 - no handshake */
        conn->version = OBJM_PROTO_V1;
        conn->params.version = OBJM_PROTO_V1;
        conn->params.capabilities = 0;
        conn->params.max_pipeline = 1;
        conn->params.backend_parallelism = 1;
    }
    
    if (params) {
        *params = conn->params;
    }
    
    return 0;
}

int objm_server_recv_request(objm_connection_t *conn, objm_request_t **req) {
    if (!conn || !req) return -1;
    
    objm_request_t *r = calloc(1, sizeof(*r));
    if (!r) return -1;
    
    if (conn->version == OBJM_PROTO_V1) {
        /* V1: mode(1) + uri_len(2) + uri */
        uint8_t header[3];
        if (recv_all(conn->fd, header, sizeof(header)) < 0) {
            free(r);
            return -1;
        }
        
        r->mode = header[0];
        r->uri_len = ntohs(*(uint16_t *)(header + 1));
        
        if (r->uri_len > OBJM_MAX_URI_LEN) {
            free(r);
            set_error(conn, "URI too long");
            return -1;
        }
        
        r->uri = malloc(r->uri_len + 1);
        if (!r->uri) {
            free(r);
            return -1;
        }
        
        if (recv_all(conn->fd, r->uri, r->uri_len) < 0) {
            free(r->uri);
            free(r);
            return -1;
        }
        r->uri[r->uri_len] = '\0';
        
    } else {
        /* V2: msg_type + request header */
        uint8_t msg_type;
        if (recv_all(conn->fd, &msg_type, sizeof(msg_type)) < 0) {
            free(r);
            return -1;
        }

        if (msg_type == OBJM_MSG_CLOSE) {
            /* Consume reason byte to keep stream in sync */
            uint8_t close_reason;
            if (recv_all(conn->fd, &close_reason, sizeof(close_reason)) < 0) {
                free(r);
                return -1;
            }

            (void)close_reason;

            free(r);
            return 1;  /* Connection closing */
        }

        if (msg_type != OBJM_MSG_REQUEST) {
            free(r);
            set_error(conn, "Unexpected message type: %d", msg_type);
            return -1;
        }

        /* request_id(4) + flags(1) + mode(1) + uri_len(2) */
        uint8_t header[8];
        if (recv_all(conn->fd, header, sizeof(header)) < 0) {
            free(r);
            return -1;
        }

        r->id = ntohl(*(uint32_t *)(header + 0));
        r->flags = header[4];
        r->mode = header[5];
        r->uri_len = ntohs(*(uint16_t *)(header + 6));
        
        if (r->uri_len > OBJM_MAX_URI_LEN) {
            free(r);
            set_error(conn, "URI too long");
            return -1;
        }
        
        r->uri = malloc(r->uri_len + 1);
        if (!r->uri) {
            free(r);
            return -1;
        }
        
        if (recv_all(conn->fd, r->uri, r->uri_len) < 0) {
            free(r->uri);
            free(r);
            return -1;
        }
        r->uri[r->uri_len] = '\0';
    }
    
    *req = r;
    return 0;
}

int objm_server_send_response(objm_connection_t *conn, const objm_response_t *resp) {
    if (!conn || !resp) return -1;
    
    if (conn->version == OBJM_PROTO_V1) {
        /* V1: status(1) + content_len(8) + metadata_len(2) + metadata */
        uint8_t header[11];
        header[0] = resp->status;
        *(uint64_t *)(header + 1) = htobe64(resp->content_len);
        *(uint16_t *)(header + 9) = htons(resp->metadata_len);
        
        if (send_all(conn->fd, header, sizeof(header)) < 0) {
            return -1;
        }
        
        if (resp->metadata_len > 0 && resp->metadata) {
            if (send_all(conn->fd, resp->metadata, resp->metadata_len) < 0) {
                return -1;
            }
        }
        
        /* Send FD if FD pass mode */
        if (resp->status == OBJM_STATUS_OK && resp->fd >= 0) {
            if (send_fd(conn->fd, resp->fd) < 0) {
                return -1;
            }
        }
        
    } else {
        if (resp->segment_count > 0 && resp->segments) {
            if (!(conn->params.capabilities & OBJM_CAP_SEGMENTED_DELIVERY)) {
                set_error(conn, "Peer lacks segmented delivery capability");
                return -1;
            }

            uint8_t header[10];
            header[0] = OBJM_MSG_SEGMENTED_RESPONSE;
            *(uint32_t *)(header + 1) = htonl(resp->request_id);
            header[5] = resp->status;
            *(uint16_t *)(header + 6) = htons(resp->segment_count);
            *(uint16_t *)(header + 8) = htons(resp->metadata_len);

            if (send_all(conn->fd, header, sizeof(header)) < 0) {
                return -1;
            }

            if (resp->metadata_len > 0 && resp->metadata) {
                if (send_all(conn->fd, resp->metadata, resp->metadata_len) < 0) {
                    return -1;
                }
            }

            if (send_segmented_payload(conn, resp) < 0) {
                return -1;
            }
        } else {
            uint8_t header[16];
            header[0] = OBJM_MSG_RESPONSE;
            *(uint32_t *)(header + 1) = htonl(resp->request_id);
            header[5] = resp->status;
            *(uint64_t *)(header + 6) = htobe64(resp->content_len);
            *(uint16_t *)(header + 14) = htons(resp->metadata_len);

            if (send_all(conn->fd, header, sizeof(header)) < 0) {
                return -1;
            }

            if (resp->metadata_len > 0 && resp->metadata) {
                if (send_all(conn->fd, resp->metadata, resp->metadata_len) < 0) {
                    return -1;
                }
            }

            if (resp->status == OBJM_STATUS_OK && resp->fd >= 0) {
                if (send_fd(conn->fd, resp->fd) < 0) {
                    return -1;
                }
            }
        }
    }
    
    return 0;
}

int objm_server_send_error(objm_connection_t *conn, uint32_t request_id,
                           uint8_t status, const char *error_msg) {
    objm_response_t resp = {0};
    resp.request_id = request_id;
    resp.status = status;
    resp.fd = -1;
    
    /* Add error message as metadata if provided */
    if (error_msg) {
        size_t msg_len = strlen(error_msg);
        resp.metadata = objm_metadata_create(msg_len + 10);
        if (resp.metadata) {
            resp.metadata_len = objm_metadata_add(resp.metadata, 0, 0xFF,
                                                   error_msg, msg_len);
        }
    }
    
    int ret = objm_server_send_response(conn, &resp);
    
    if (resp.metadata) {
        free(resp.metadata);
    }
    
    return ret;
}

int objm_server_send_close_ack(objm_connection_t *conn, uint32_t outstanding) {
    if (!conn || conn->version != OBJM_PROTO_V2) {
        return -1;
    }
    
    uint8_t ack[6];
    ack[0] = OBJM_MSG_CLOSE_ACK;
    ack[1] = 0;  /* Reserved */
    *(uint32_t *)(ack + 2) = htonl(outstanding);
    
    return send_all(conn->fd, ack, sizeof(ack));
}

void objm_server_destroy(objm_connection_t *conn) {
    if (!conn) return;
    free(conn);
}

/* ============================================================================
 * Common utilities
 * ============================================================================ */

int objm_get_params(objm_connection_t *conn, objm_params_t *params) {
    if (!conn || !params) return -1;
    *params = conn->params;
    return 0;
}

int objm_has_capability(objm_connection_t *conn, uint16_t capability) {
    if (!conn) return 0;
    return (conn->params.capabilities & capability) != 0;
}

int objm_get_fd(objm_connection_t *conn) {
    return conn ? conn->fd : -1;
}

void objm_set_callbacks(objm_connection_t *conn, const objm_callbacks_t *callbacks,
                        void *user_data) {
    if (!conn) return;
    if (callbacks) {
        conn->callbacks = *callbacks;
    } else {
        memset(&conn->callbacks, 0, sizeof(conn->callbacks));
    }
    conn->user_data = user_data;
}

void objm_request_free(objm_request_t *req) {
    if (!req) return;
    free(req->uri);
    free(req);
}

void objm_response_free(objm_response_t *resp) {
    if (!resp) return;
    if (resp->segments) {
        free_segments(resp->segments, resp->segment_count);
    }
    if (resp->fd >= 0) {
        close(resp->fd);
    }
    free(resp->metadata);
    free(resp->error_msg);
    free(resp);
}

/* ============================================================================
 * Metadata utilities
 * ============================================================================ */

uint8_t *objm_metadata_create(size_t estimated_size) {
    return malloc(estimated_size > 0 ? estimated_size : 128);
}

size_t objm_metadata_add(uint8_t *metadata, size_t current_len,
                         uint8_t type, const void *data, size_t len) {
    if (!metadata || len > 65535) return 0;
    
    metadata[current_len] = type;
    *(uint16_t *)(metadata + current_len + 1) = htons(len);
    memcpy(metadata + current_len + 3, data, len);
    
    return current_len + 3 + len;
}

size_t objm_metadata_add_size(uint8_t *metadata, size_t current_len, uint64_t size) {
    uint64_t size_be = htobe64(size);
    return objm_metadata_add(metadata, current_len, OBJM_META_SIZE, &size_be, 8);
}

size_t objm_metadata_add_mtime(uint8_t *metadata, size_t current_len, uint64_t mtime) {
    uint64_t mtime_be = htobe64(mtime);
    return objm_metadata_add(metadata, current_len, OBJM_META_MTIME, &mtime_be, 8);
}

size_t objm_metadata_add_backend(uint8_t *metadata, size_t current_len, uint8_t backend_id) {
    return objm_metadata_add(metadata, current_len, OBJM_META_BACKEND, &backend_id, 1);
}

size_t objm_metadata_add_payload(uint8_t *metadata, size_t current_len,
                                 const objm_payload_descriptor_t *payload) {
    if (!metadata || !payload) {
        return current_len;
    }

    uint8_t encoded[OBJM_PAYLOAD_DESCRIPTOR_WIRE_SIZE];
    if (objm_payload_descriptor_encode(payload, encoded, sizeof(encoded)) < 0) {
        return current_len;
    }

    return objm_metadata_add(metadata, current_len, OBJM_META_PAYLOAD,
                             encoded, sizeof(encoded));
}

int objm_metadata_parse(const uint8_t *metadata, size_t metadata_len,
                        objm_metadata_entry_t **entries, size_t *num_entries) {
    if (!metadata || !entries || !num_entries) return -1;
    
    /* Count entries */
    size_t count = 0;
    size_t offset = 0;
    while (offset + 3 <= metadata_len) {
        uint16_t len = ntohs(*(uint16_t *)(metadata + offset + 1));
        if (offset + 3 + len > metadata_len) break;
        count++;
        offset += 3 + len;
    }
    
    /* Allocate array */
    objm_metadata_entry_t *arr = calloc(count, sizeof(*arr));
    if (!arr) return -1;
    
    /* Parse entries */
    offset = 0;
    size_t idx = 0;
    while (offset + 3 <= metadata_len && idx < count) {
        arr[idx].type = metadata[offset];
        arr[idx].length = ntohs(*(uint16_t *)(metadata + offset + 1));
        arr[idx].data = malloc(arr[idx].length);
        if (!arr[idx].data) {
            objm_metadata_free_entries(arr, idx);
            return -1;
        }
        memcpy(arr[idx].data, metadata + offset + 3, arr[idx].length);
        offset += 3 + arr[idx].length;
        idx++;
    }
    
    *entries = arr;
    *num_entries = count;
    return 0;
}

const objm_metadata_entry_t *objm_metadata_get(const objm_metadata_entry_t *entries,
                                                size_t num_entries, uint8_t type) {
    for (size_t i = 0; i < num_entries; i++) {
        if (entries[i].type == type) {
            return &entries[i];
        }
    }
    return NULL;
}

int objm_metadata_get_payload(const objm_metadata_entry_t *entries,
                              size_t num_entries,
                              objm_payload_descriptor_t *out_payload) {
    if (!entries || !out_payload) {
        return -1;
    }

    const objm_metadata_entry_t *entry =
        objm_metadata_get(entries, num_entries, OBJM_META_PAYLOAD);
    if (!entry) {
        return 1;  /* Not found */
    }

    if (entry->length != OBJM_PAYLOAD_DESCRIPTOR_WIRE_SIZE) {
        return -1;
    }

    memset(out_payload, 0, sizeof(*out_payload));
    out_payload->version = OBJM_PAYLOAD_DESCRIPTOR_VERSION;

    const uint8_t *p = entry->data;
    uint32_t field32;
    uint64_t field64;

    memcpy(&field32, p, sizeof(field32));
    out_payload->version = le32toh(field32);
    p += sizeof(field32);

    memcpy(&field32, p, sizeof(field32));
    out_payload->variant_count = le32toh(field32);
    p += sizeof(field32);

    if (out_payload->variant_count > OBJM_MAX_VARIANTS) {
        return -1;
    }

    memcpy(&field32, p, sizeof(field32));
    out_payload->manifest_flags = le32toh(field32);
    p += sizeof(field32);

    memcpy(&field32, p, sizeof(field32));
    out_payload->reserved = le32toh(field32);
    p += sizeof(field32);

    for (size_t i = 0; i < OBJM_MAX_VARIANTS; i++) {
        objm_variant_descriptor_t *variant = &out_payload->variants[i];

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

void objm_metadata_free_entries(objm_metadata_entry_t *entries, size_t num_entries) {
    if (!entries) return;
    for (size_t i = 0; i < num_entries; i++) {
        free(entries[i].data);
    }
    free(entries);
}

/* ============================================================================
 * Helper functions
 * ============================================================================ */

const char *objm_status_name(uint8_t status) {
    switch (status) {
        case OBJM_STATUS_OK: return "OK";
        case OBJM_STATUS_NOT_FOUND: return "NOT_FOUND";
        case OBJM_STATUS_INVALID_REQUEST: return "INVALID_REQUEST";
        case OBJM_STATUS_INVALID_MODE: return "INVALID_MODE";
        case OBJM_STATUS_URI_TOO_LONG: return "URI_TOO_LONG";
        case OBJM_STATUS_UNSUPPORTED_OP: return "UNSUPPORTED_OP";
        case OBJM_STATUS_INTERNAL_ERROR: return "INTERNAL_ERROR";
        case OBJM_STATUS_STORAGE_ERROR: return "STORAGE_ERROR";
        case OBJM_STATUS_OUT_OF_MEMORY: return "OUT_OF_MEMORY";
        case OBJM_STATUS_TIMEOUT: return "TIMEOUT";
        case OBJM_STATUS_UNAVAILABLE: return "UNAVAILABLE";
        case OBJM_STATUS_PROTOCOL_ERROR: return "PROTOCOL_ERROR";
        case OBJM_STATUS_VERSION_MISMATCH: return "VERSION_MISMATCH";
        case OBJM_STATUS_CAPABILITY_ERROR: return "CAPABILITY_ERROR";
        default: return "UNKNOWN";
    }
}

const char *objm_mode_name(char mode) {
    switch (mode) {
        case OBJM_MODE_FDPASS: return "FD_PASS";
        case OBJM_MODE_COPY: return "COPY";
        case OBJM_MODE_SPLICE: return "SPLICE";
        case OBJM_MODE_SEGMENTED: return "SEGMENTED";
        default: return "UNKNOWN";
    }
}

const char *objm_last_error(const objm_connection_t *conn) {
    if (!conn || conn->error[0] == '\0') {
        return NULL;
    }
    return conn->error;
}

int objm_capability_names(uint16_t capabilities, char *buffer, size_t size) {
    static const struct {
        uint16_t bit;
        const char *name;
    } cap_table[] = {
        {OBJM_CAP_OOO_REPLIES, "OOO_REPLIES"},
        {OBJM_CAP_PIPELINING, "PIPELINING"},
        {OBJM_CAP_COMPRESSION, "COMPRESSION"},
        {OBJM_CAP_MULTIPLEXING, "MULTIPLEXING"},
        {OBJM_CAP_SEGMENTED_DELIVERY, "SEGMENTED"},
    };

    size_t written = 0;
    int first = 1;

    for (size_t i = 0; i < sizeof(cap_table) / sizeof(cap_table[0]); i++) {
        if (!(capabilities & cap_table[i].bit)) {
            continue;
        }

        size_t remaining = (size > written) ? (size - written) : 0;
        int ret = snprintf(buffer + written, remaining,
                           "%s%s", first ? "" : "|", cap_table[i].name);
        if (ret < 0) {
            return (int)written;
        }
        if ((size_t)ret >= remaining) {
            written = size;
        } else {
            written += (size_t)ret;
        }
        first = 0;
    }

    if ((capabilities & OBJM_CAP_SEGMENTED_DELIVERY) && size > 0) {
        if (!strstr(buffer, "SEGMENTED")) {
            size_t len = (written < size) ? written : size - 1;
            size_t remaining = (size > len) ? (size - len) : 0;
            int ret = snprintf(buffer + len, remaining, "%sSEGMENTED",
                               (len > 0) ? "|" : "");
            if (ret > 0) {
                written = len + (size_t)ret;
            }
        }
    }

    if (first && size > 0) {
        buffer[0] = '\0';
    }

    return (int)written;
}
