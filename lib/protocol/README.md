# objmapper Protocol Library

Simple, efficient C library implementing the objmapper wire protocol for persistent connections with optional out-of-order (OOO) reply support.

## Features

- **Dual Protocol Support**: Version 1 (simple ordered) and Version 2 (pipelined with OOO)
- **Zero Overhead**: OOO support has no cost when disabled
- **Persistent Connections**: Designed for long-lived connections with request pipelining
- **Simple API**: Clean client and server interfaces
- **FD Passing**: Efficient file descriptor passing over Unix sockets
- **Metadata Support**: Extensible metadata system
- **Capability Negotiation**: Automatic feature detection and negotiation
- **Segmented Delivery**: Mix inline bytes and zero-copy file descriptors in one response

## Quick Start

### Building

```bash
cd lib/protocol
make
make examples
```

This creates:
- `libobmprotocol.a` - Static library
- `libobmprotocol.so` - Shared library
- `example_client` - Example client
- `example_server` - Example server

### Running the Example

Terminal 1 (server):
```bash
./example_server /tmp/objmapper.sock
```

Terminal 2 (client):
```bash
./example_client /tmp/objmapper.sock /etc/passwd 1
```

## API Overview

### Client API

```c
/* Create connection */
objm_connection_t *conn = objm_client_create(socket_fd, OBJM_PROTO_V2);

/* Handshake (V2 only) */
objm_hello_t hello = {
    .capabilities = OBJM_CAP_OOO_REPLIES | OBJM_CAP_PIPELINING |
                    OBJM_CAP_SEGMENTED_DELIVERY,
    .max_pipeline = 100
};
objm_client_hello(conn, &hello, NULL);

/* Send request */
objm_request_t req = {
    .id = 1,
    .mode = OBJM_MODE_FDPASS,
    .uri = "/path/to/file",
    .uri_len = strlen("/path/to/file")
};
objm_client_send_request(conn, &req);

/* Receive response */
objm_response_t *resp;
objm_client_recv_response(conn, &resp);

if (resp->status == OBJM_STATUS_OK && resp->fd >= 0) {
    /* Use file descriptor */
    read(resp->fd, buffer, size);
}

objm_response_free(resp);

/* Close */
objm_client_close(conn, OBJM_CLOSE_NORMAL);
objm_client_destroy(conn);
```

### Server API

```c
/* Create connection */
objm_connection_t *conn = objm_server_create(client_fd);

/* Handshake (auto-detects V1 or V2) */
objm_hello_t hello = {
    .capabilities = OBJM_CAP_OOO_REPLIES | OBJM_CAP_PIPELINING |
                    OBJM_CAP_SEGMENTED_DELIVERY,
    .max_pipeline = 100,
    .backend_parallelism = 3
};
objm_server_handshake(conn, &hello, NULL);

/* Receive request */
objm_request_t *req;
int ret = objm_server_recv_request(conn, &req);

if (ret == 0) {
    /* Handle request */
    int fd = open(req->uri, O_RDONLY);
    
    /* Build response with metadata */
    uint8_t *metadata = objm_metadata_create(100);
    size_t meta_len = objm_metadata_add_size(metadata, 0, file_size);
    
    objm_response_t resp = {
        .request_id = req->id,
        .status = OBJM_STATUS_OK,
        .fd = fd,
        .metadata = metadata,
        .metadata_len = meta_len
    };
    
    objm_server_send_response(conn, &resp);
    free(metadata);
}

objm_request_free(req);
objm_server_destroy(conn);
```

## Protocol Versions

### Version 1 (Simple Ordered)

- Minimal overhead: 3-byte request, 11-byte response header
- No handshake required
- Single request at a time
- Backward compatible

### Version 2 (Pipelined with OOO)

- HELLO/HELLO_ACK handshake with capability negotiation
- Request IDs for correlation
- Optional out-of-order responses
- Request pipelining support
- Per-request ordering control via `OBJM_REQ_ORDERED` flag

## Operation Modes

- `OBJM_MODE_FDPASS` ('1'): Pass file descriptor (zero-copy)
- `OBJM_MODE_COPY` ('2'): Copy data through socket
- `OBJM_MODE_SPLICE` ('3'): Splice data (kernel-assisted copy)
- `OBJM_MODE_SEGMENTED` ('4'): Stream ordered segments (inline + FD ranges)

### Segmented Responses

When both endpoints negotiate `OBJM_CAP_SEGMENTED_DELIVERY`, responses can carry a descriptor array describing inline blocks and zero-copy ranges. Each `objm_segment_t` entry contains the logical length, optional inline payload, and (for FD segments) the file descriptor and byte range to consume. The client API exposes parsed segments under `objm_response_t::segments`, and `objm_response_free` automatically releases any received descriptors and buffers.

## Metadata

The library supports extensible metadata:

```c
/* Add metadata entries */
uint8_t *metadata = objm_metadata_create(100);
size_t len = 0;

len = objm_metadata_add_size(metadata, len, file_size);
len = objm_metadata_add_mtime(metadata, len, mtime);
len = objm_metadata_add_backend(metadata, len, backend_id);

/* Parse metadata */
objm_metadata_entry_t *entries;
size_t num_entries;
objm_metadata_parse(metadata, len, &entries, &num_entries);

/* Find specific metadata */
const objm_metadata_entry_t *size_entry = 
    objm_metadata_get(entries, num_entries, OBJM_META_SIZE);

objm_metadata_free_entries(entries, num_entries);
```

## Error Handling

All functions return 0 on success, -1 on error. Error details can be retrieved via connection state.

Status codes:
- `OBJM_STATUS_OK` - Success
- `OBJM_STATUS_NOT_FOUND` - File/object not found
- `OBJM_STATUS_INVALID_REQUEST` - Malformed request
- `OBJM_STATUS_INTERNAL_ERROR` - Server error
- See `protocol.h` for complete list

## Out-of-Order Support

When OOO capability is negotiated, responses may arrive in any order:

```c
/* Send multiple requests */
for (int i = 0; i < 10; i++) {
    req.id = i;
    objm_client_send_request(conn, &req);
}

/* Wait for specific response */
objm_response_t *resp;
objm_client_recv_response_for(conn, 5, &resp);  /* Wait for request ID 5 */

/* Or receive in arrival order */
objm_client_recv_response(conn, &resp);  /* Get next available */
```

Force in-order response for specific requests:
```c
req.flags = OBJM_REQ_ORDERED;  /* This response must be in-order */
```

## Performance Characteristics

- **FD Passing**: O(1) regardless of file size (~8μs per request)
- **Request Overhead**: 9 bytes (V2) or 3 bytes (V1) + URI length
- **Response Overhead**: 16 bytes (V2) or 11 bytes (V1) + metadata
- **Zero-Copy**: FD pass mode never touches file contents
- **Pipelining**: Send multiple requests without waiting for responses

## Thread Safety

The library is **not thread-safe**. Each connection should be used by a single thread, or protected by external locking.

For multi-threaded servers, use one connection per thread or implement a connection pool.

## Integration

Link against the library:

```makefile
CFLAGS += -I/usr/local/include/objmapper
LDFLAGS += -L/usr/local/lib -lobmprotocol
```

Or use the static library directly:
```makefile
LDFLAGS += lib/protocol/libobmprotocol.a
```

## License

See LICENSE file in the repository root.
