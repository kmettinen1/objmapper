# Transport Abstraction Layer

## Overview

The transport abstraction layer (`lib/transport`) provides a unified API for multiple network transport types while maintaining Unix domain sockets as the primary interface for zero-copy file descriptor passing.

## Supported Transports

### Unix Domain Sockets (Primary)
- **Type:** `TRANSPORT_UNIX`
- **Features:** Connection-oriented, stream-based, supports FD passing
- **Use Cases:** Local IPC with zero-copy FD passing (primary mode)
- **Default:** `/tmp/objmapper.sock`

### TCP Sockets
- **Type:** `TRANSPORT_TCP`
- **Features:** Connection-oriented, stream-based, no FD passing
- **Use Cases:** Network-based object delivery with copy/splice modes
- **Default Port:** 9999

### UDP Sockets
- **Type:** `TRANSPORT_UDP`
- **Features:** Connectionless, datagram-based, no FD passing
- **Use Cases:** Lightweight object delivery (experimental)
- **Default Port:** 9998

## Architecture

### Core Components

```c
// Transport types
typedef enum {
    TRANSPORT_UNIX = 0,
    TRANSPORT_TCP = 1,
    TRANSPORT_UDP = 2
} transport_type_t;

// Opaque transport handle
typedef struct transport transport_t;

// Capability detection
typedef struct {
    int supports_fdpass;        // Can pass file descriptors
    int is_stream;              // Stream vs datagram
    int is_connection_oriented; // Connection vs connectionless
} transport_caps_t;
```

### Configuration

Transport configuration uses a union to support different socket types:

```c
typedef struct {
    transport_type_t type;
    union {
        struct {
            const char *path;
        } unix_cfg;
        struct {
            const char *host;
            uint16_t port;
        } tcp_cfg;
        struct {
            const char *host;
            uint16_t port;
        } udp_cfg;
    };
} transport_config_t;
```

### API Functions

#### Server Functions
- `transport_server_create()` - Create server socket
- `transport_accept()` - Accept client connection (stream transports)

#### Client Functions
- `transport_client_connect()` - Connect to server

#### I/O Functions
- `transport_send()` / `transport_recv()` - Data transfer
- `transport_send_fd()` / `transport_recv_fd()` - FD passing (Unix only)

#### Utility Functions
- `transport_get_caps()` - Get transport capabilities
- `transport_close()` - Close connection

## Usage Examples

### Server with Unix Sockets (FD Passing)

```bash
./build/objmapper-server -t unix -s /tmp/obj.sock -b /data
```

```c
server_config_t config = {
    .transport = OBJMAPPER_TRANSPORT_UNIX,
    .socket_path = "/tmp/obj.sock",
    .backing_dir = "/data"
};
objmapper_server_start(&config);
```

### Server with TCP

```bash
./build/objmapper-server -t tcp -H 0.0.0.0 -p 8080 -b /data
```

```c
server_config_t config = {
    .transport = OBJMAPPER_TRANSPORT_TCP,
    .net = { .host = "0.0.0.0", .port = 8080 },
    .backing_dir = "/data"
};
objmapper_server_start(&config);
```

### Client with Unix Sockets (FD Passing Mode)

```bash
./build/objmapper-test-client -t unix -m 1 /path/to/object
```

```c
client_config_t config = {
    .transport = OBJMAPPER_TRANSPORT_UNIX,
    .socket_path = "/tmp/objmapper.sock",
    .operation_mode = OP_FDPASS  // '1'
};
int sock = objmapper_client_connect(&config);
int fd = objmapper_client_request(sock, "/path/to/object", OP_FDPASS);
```

### Client with TCP (Copy Mode)

```bash
./build/objmapper-test-client -t tcp -H localhost -p 8080 -m 2 /path/to/object
```

```c
client_config_t config = {
    .transport = OBJMAPPER_TRANSPORT_TCP,
    .net = { .host = "localhost", .port = 8080 },
    .operation_mode = OP_COPY  // '2'
};
int sock = objmapper_client_connect(&config);
int fd = objmapper_client_request(sock, "/path/to/object", OP_COPY);
```

## Operation Modes by Transport

| Mode | Unix | TCP | UDP | Description |
|------|------|-----|-----|-------------|
| FD Pass (1) | ✅ | ❌ | ❌ | Zero-copy file descriptor passing |
| Copy (2) | ✅ | ✅ | ✅ | Data copied over socket |
| Splice (3) | ✅ | ✅ | ❌ | Zero-copy pipe-based transfer (streams only) |

The server automatically validates operation modes against transport capabilities and returns errors for unsupported combinations.

## Implementation Details

### Capability Detection

The transport layer provides runtime capability detection:

```c
transport_caps_t caps = transport_get_caps(transport);
if (caps.supports_fdpass) {
    // Can use FD passing
}
if (caps.is_stream) {
    // Can use splice
}
```

### Internal Architecture

Each transport type has helper functions:
- `create_unix_server()` - Unix domain socket setup with `SO_REUSEADDR`
- `create_tcp_server()` - TCP socket with `bind()` and `listen()`
- `create_udp_server()` - UDP socket with `bind()` only

The `transport_t` structure internally tracks:
- Socket file descriptor
- Transport type
- Address information (for datagrams)
- Capabilities

### Error Handling

- FD passing on non-Unix transports returns `-1` with `errno = ENOTSUP`
- Invalid modes return error in response packet
- Network resolution failures propagate system errors

## Design Decisions

1. **Unix Sockets as Primary:** Optimized for the common case of local FD passing
2. **Opaque Type:** `transport_t` keeps implementation details hidden
3. **Capability-Based:** Runtime detection prevents invalid operations
4. **Union Config:** Type-safe configuration per transport type
5. **Backward Compatible:** Existing FD passing code still works via `lib/fdpass`

## Future Enhancements

- [ ] Full UDP datagram support (currently experimental)
- [ ] TLS/SSL support for encrypted TCP transport
- [ ] Unix socket credentials passing
- [ ] IPv6 support
- [ ] Connection pooling for TCP clients
- [ ] Asynchronous I/O with epoll/kqueue
- [ ] QUIC transport for low-latency UDP

## Build Integration

The transport library is built as part of `libobjmapper.a`:

```makefile
TRANSPORT_OBJ = $(BUILD_DIR)/transport.o
$(TRANSPORT_OBJ): $(LIB_TRANSPORT)/transport.c $(LIB_TRANSPORT)/transport.h
    $(CC) $(CFLAGS) -c $< -o $@
```

## Testing

Run the integration tests with different transports:

```bash
# Test Unix socket with FD passing
./test.sh

# Test TCP mode
./build/objmapper-server -t tcp -p 8080 -b /tmp/test_objects &
./build/objmapper-test-client -t tcp -p 8080 -m 2 /test/file.txt

# Test with network debugging
strace -e network ./build/objmapper-server -t tcp -p 8080 -b /data
```

## References

- [ARCHITECTURE.md](ARCHITECTURE.md) - Overall system architecture
- [REFACTORING.md](REFACTORING.md) - Modular refactoring details
- [lib/transport/transport.h](../lib/transport/transport.h) - API header
- [lib/transport/transport.c](../lib/transport/transport.c) - Implementation
