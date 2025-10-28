# objmapper Protocol Specification

## Overview

The objmapper wire protocol is designed for persistent connections with optional out-of-order (OOO) reply support. The protocol enables efficient request pipelining and concurrent backend processing while maintaining backward compatibility with simple ordered request/response flows.

**Design Goals:**
- Support persistent connections with keep-alive
- Enable request pipelining (send multiple requests without waiting)
- Allow out-of-order replies when backend supports it
- Maintain zero overhead for simple ordered flows
- Support multiple transfer modes (FD passing, copy, splice)
- Be transport-agnostic (Unix sockets, TCP, UDP)

## Protocol Versions

### Version 1: Basic Ordered Protocol
- Single request/response at a time
- No request IDs
- Simple mode byte + URI format
- Backward compatible with existing code

### Version 2: Pipelined with Optional OOO (Recommended)
- Request pipelining supported
- Optional request/response IDs for OOO
- Server capability negotiation
- Enables backend parallelism

## Connection Lifecycle

```
Client                          Server
  |                               |
  |-- TCP/Unix connect ---------->|
  |                               |
  |-- HELLO (optional) ---------->|
  |<- HELLO_ACK (capabilities) ---|
  |                               |
  |-- REQUEST_1 ----------------->|
  |-- REQUEST_2 (pipelined) ----->|
  |-- REQUEST_3 (pipelined) ----->|
  |                               |
  |<- RESPONSE_2 (OOO if enabled)-|
  |<- RESPONSE_1 (OOO if enabled)-|
  |<- RESPONSE_3 (OOO if enabled)-|
  |                               |
  |-- REQUEST_4 ----------------->|
  |<- RESPONSE_4 -----------------|
  |                               |
  ... (keep connection alive) ...
  |                               |
  |-- CLOSE (graceful) ---------->|
  |<- CLOSE_ACK ------------------|
  |-- TCP/Unix close ------------->|
  |                               |
```

## Message Format

### HELLO Message (Optional, Version 2+)

**Purpose:** Capability negotiation at connection start

```
┌─────────────────────────────────────────┐
│ Magic (4 bytes): "OBJM"                 │
├─────────────────────────────────────────┤
│ Protocol Version (1 byte): 0x02         │
├─────────────────────────────────────────┤
│ Client Capabilities (2 bytes):          │
│   Bit 0: Supports OOO replies           │
│   Bit 1: Supports request pipelining    │
│   Bit 2: Supports compression (future)  │
│   Bit 3: Supports multiplexing (future) │
│   Bits 4-15: Reserved                   │
├─────────────────────────────────────────┤
│ Max Pipeline Depth (2 bytes)            │
│   0 = unlimited, N = max concurrent     │
└─────────────────────────────────────────┘
Total: 9 bytes
```

**Client Capabilities Flags:**
```c
#define OBJM_CAP_OOO_REPLIES    0x0001  // Can handle out-of-order responses
#define OBJM_CAP_PIPELINING     0x0002  // Can send pipelined requests
#define OBJM_CAP_COMPRESSION    0x0004  // Reserved for future
#define OBJM_CAP_MULTIPLEXING   0x0008  // Reserved for future
```

**Example:**
```c
// Client supports OOO and pipelining, max 16 concurrent requests
uint8_t hello[9] = {
    'O', 'B', 'J', 'M',    // Magic
    0x02,                   // Version 2
    0x03, 0x00,            // Capabilities: OOO | PIPELINING
    0x10, 0x00             // Max pipeline depth: 16
};
```

### HELLO_ACK Message (Version 2+)

**Purpose:** Server confirms capabilities and limits

```
┌─────────────────────────────────────────┐
│ Status (1 byte):                        │
│   0x00 = OK                             │
│   0x01 = Version not supported          │
│   0x02 = Capabilities not supported     │
├─────────────────────────────────────────┤
│ Server Capabilities (2 bytes):          │
│   Same flags as client                  │
├─────────────────────────────────────────┤
│ Max Pipeline Depth (2 bytes)            │
│   Server's limit (may be < client's)    │
├─────────────────────────────────────────┤
│ Backend Parallelism (1 byte)            │
│   Number of parallel backend paths      │
│   0 = serialized, N = parallel          │
└─────────────────────────────────────────┘
Total: 6 bytes
```

**Negotiated Capabilities:**
- Intersection of client and server capabilities
- If client doesn't support OOO but server does, OOO is disabled
- Pipeline depth = min(client_max, server_max)

### REQUEST Message

#### Version 1 (Simple, Ordered)

```
┌─────────────────────────────────────────┐
│ Mode (1 byte):                          │
│   '1' = OP_FDPASS                       │
│   '2' = OP_COPY                         │
│   '3' = OP_SPLICE                       │
├─────────────────────────────────────────┤
│ URI Length (2 bytes, network order)     │
├─────────────────────────────────────────┤
│ URI (variable, UTF-8)                   │
└─────────────────────────────────────────┘
Total: 3 + URI_length bytes
```

#### Version 2 (Pipelined with Optional OOO)

```
┌─────────────────────────────────────────┐
│ Message Type (1 byte): 0x01 (REQUEST)  │
├─────────────────────────────────────────┤
│ Request ID (4 bytes, network order)     │
│   Unique per connection, monotonic      │
│   Used to match responses in OOO mode   │
├─────────────────────────────────────────┤
│ Flags (1 byte):                         │
│   Bit 0: Require in-order response      │
│   Bit 1: Priority (high=1, normal=0)    │
│   Bits 2-7: Reserved                    │
├─────────────────────────────────────────┤
│ Mode (1 byte):                          │
│   '1' = OP_FDPASS                       │
│   '2' = OP_COPY                         │
│   '3' = OP_SPLICE                       │
├─────────────────────────────────────────┤
│ URI Length (2 bytes, network order)     │
├─────────────────────────────────────────┤
│ URI (variable, UTF-8)                   │
└─────────────────────────────────────────┘
Total: 9 + URI_length bytes
```

**Request ID Rules:**
- MUST be unique within the connection
- SHOULD be monotonically increasing (but not required)
- Can wrap around at 2^32
- Server MUST echo back in response

**Flags:**
```c
#define OBJM_REQ_ORDERED   0x01  // Force in-order response
#define OBJM_REQ_PRIORITY  0x02  // High priority request
```

### RESPONSE Message

#### Version 1 (Simple, Ordered)

**Success (FD Passing):**
```
┌─────────────────────────────────────────┐
│ Status (1 byte): 0x00 (OK)              │
├─────────────────────────────────────────┤
│ File Descriptor (via SCM_RIGHTS)        │
│   Sent via sendmsg() with SCM_RIGHTS    │
│   Unix sockets only                     │
└─────────────────────────────────────────┘
Total: 1 byte + FD in ancillary data
```

**Success (Copy/Splice):**
```
┌─────────────────────────────────────────┐
│ Status (1 byte): 0x00 (OK)              │
├─────────────────────────────────────────┤
│ Content Length (8 bytes, network order) │
├─────────────────────────────────────────┤
│ Content (variable bytes)                │
│   Streamed directly or via splice()     │
└─────────────────────────────────────────┘
Total: 9 + content_length bytes
```

**Error:**
```
┌─────────────────────────────────────────┐
│ Status (1 byte): 0x01-0xFF (error code) │
├─────────────────────────────────────────┤
│ Error Message Length (2 bytes)          │
├─────────────────────────────────────────┤
│ Error Message (variable, UTF-8)         │
└─────────────────────────────────────────┘
Total: 3 + error_message_length bytes
```

#### Version 2 (With Request ID for OOO)

**Success (FD Passing):**
```
┌─────────────────────────────────────────┐
│ Message Type (1 byte): 0x02 (RESPONSE) │
├─────────────────────────────────────────┤
│ Request ID (4 bytes, network order)     │
│   Echo of the request ID from REQUEST   │
├─────────────────────────────────────────┤
│ Status (1 byte): 0x00 (OK)              │
├─────────────────────────────────────────┤
│ Metadata Length (2 bytes)               │
│   Optional metadata (size, mtime, etc)  │
├─────────────────────────────────────────┤
│ Metadata (variable, optional)           │
├─────────────────────────────────────────┤
│ File Descriptor (via SCM_RIGHTS)        │
└─────────────────────────────────────────┘
Total: 8 + metadata_length bytes + FD
```

**Success (Copy/Splice):**
```
┌─────────────────────────────────────────┐
│ Message Type (1 byte): 0x02 (RESPONSE) │
├─────────────────────────────────────────┤
│ Request ID (4 bytes, network order)     │
├─────────────────────────────────────────┤
│ Status (1 byte): 0x00 (OK)              │
├─────────────────────────────────────────┤
│ Content Length (8 bytes, network order) │
├─────────────────────────────────────────┤
│ Metadata Length (2 bytes)               │
├─────────────────────────────────────────┤
│ Metadata (variable, optional)           │
├─────────────────────────────────────────┤
│ Content (variable bytes)                │
└─────────────────────────────────────────┘
Total: 16 + metadata_length + content_length bytes
```

**Error:**
```
┌─────────────────────────────────────────┐
│ Message Type (1 byte): 0x02 (RESPONSE) │
├─────────────────────────────────────────┤
│ Request ID (4 bytes, network order)     │
├─────────────────────────────────────────┤
│ Status (1 byte): 0x01-0xFF (error code) │
├─────────────────────────────────────────┤
│ Error Message Length (2 bytes)          │
├─────────────────────────────────────────┤
│ Error Message (variable, UTF-8)         │
└─────────────────────────────────────────┘
Total: 8 + error_message_length bytes
```

### Status Codes

```c
// Success
#define OBJM_STATUS_OK              0x00

// Client errors (4xx equivalent)
#define OBJM_STATUS_NOT_FOUND       0x01
#define OBJM_STATUS_INVALID_REQUEST 0x02
#define OBJM_STATUS_INVALID_MODE    0x03
#define OBJM_STATUS_URI_TOO_LONG    0x04
#define OBJM_STATUS_UNSUPPORTED_OP  0x05

// Server errors (5xx equivalent)
#define OBJM_STATUS_INTERNAL_ERROR  0x10
#define OBJM_STATUS_STORAGE_ERROR   0x11
#define OBJM_STATUS_OUT_OF_MEMORY   0x12
#define OBJM_STATUS_TIMEOUT         0x13
#define OBJM_STATUS_UNAVAILABLE     0x14

// Protocol errors
#define OBJM_STATUS_PROTOCOL_ERROR  0x20
#define OBJM_STATUS_VERSION_MISMATCH 0x21
#define OBJM_STATUS_CAPABILITY_ERROR 0x22
```

## Out-of-Order Reply Semantics

### When OOO is Enabled

**Server behavior:**
- Can process requests in parallel across multiple backend paths
- Can send responses as soon as ready (not waiting for earlier requests)
- MUST include request ID in every response
- MUST respect per-request ORDERED flag

**Client behavior:**
- MUST track outstanding requests by ID
- MUST handle responses in any order
- Can pipeline multiple requests without waiting
- Matches responses to requests via request ID

**Example OOO scenario:**
```
Time  Client                 Server (3 backend paths)
----  ------                 ----------------------
T0    REQ_1 (10GB file) --->  [Path A starts, slow disk I/O]
T1    REQ_2 (1KB file)  --->  [Path B starts, cache hit]
T2    REQ_3 (1MB file)  --->  [Path C starts, cache hit]
T3                      <---  RESP_2 (fast, from Path B)
T4                      <---  RESP_3 (medium, from Path C)
T8                      <---  RESP_1 (slow, from Path A)
```

Without OOO, client would wait until T8 for REQ_2 and REQ_3 responses.

### When OOO is Disabled (Ordered Mode)

**Server behavior:**
- Processes requests in parallel but MUST queue responses
- Sends responses in request order (FIFO)
- Request IDs still included for debugging/monitoring
- Head-of-line blocking if early request is slow

**Client behavior:**
- Can still pipeline requests
- Receives responses in order
- Simpler client implementation (no request tracking needed)

**Example ordered scenario:**
```
Time  Client                 Server (3 backend paths)
----  ------                 ----------------------
T0    REQ_1 (10GB file) --->  [Path A starts, slow]
T1    REQ_2 (1KB file)  --->  [Path B starts, fast, WAITS]
T2    REQ_3 (1MB file)  --->  [Path C starts, fast, WAITS]
T8                      <---  RESP_1 (slow completes)
T8                      <---  RESP_2 (queued, now sent)
T8                      <---  RESP_3 (queued, now sent)
```

Responses 2 and 3 wait for response 1 (head-of-line blocking).

### Mixed Mode (Per-Request Ordering)

Client can request in-order response for specific requests:

```c
// Request 1: Large file, allow OOO
REQUEST(id=1, flags=0, uri="/large.dat")

// Request 2: Critical, must be in order after request 1
REQUEST(id=2, flags=OBJM_REQ_ORDERED, uri="/critical.dat")

// Request 3: Can be OOO
REQUEST(id=3, flags=0, uri="/other.dat")
```

Server MUST NOT send RESP_2 until RESP_1 has been sent, even if REQ_2 completes first.

## Request Pipelining

### Pipeline Depth

**Definition:** Maximum number of outstanding (sent but not responded) requests.

**Negotiation:**
```
Client: I can handle up to 64 concurrent requests
Server: I can process up to 16 in parallel
Agreed: min(64, 16) = 16 pipeline depth
```

**Flow control:**
```c
int outstanding = 0;
int max_pipeline = negotiated_depth;

while (has_more_requests()) {
    if (outstanding < max_pipeline) {
        send_request();
        outstanding++;
    }
    
    // Process any incoming responses
    while (poll_response()) {
        handle_response();
        outstanding--;
    }
}
```

### Pipeline Stalls

**Situation:** Pipeline full, must wait for response before sending more.

**Mitigation strategies:**
1. **Priority flag:** Mark urgent requests for faster processing
2. **Adaptive depth:** Reduce pipeline depth if latency increases
3. **Timeout:** Cancel slow requests after threshold
4. **Alternative backend:** Route to different provider if available

## Metadata Format (Optional)

Metadata provides additional information about the object without requiring separate requests.

```
┌─────────────────────────────────────────┐
│ Metadata Entry Type (1 byte)            │
├─────────────────────────────────────────┤
│ Entry Length (2 bytes)                  │
├─────────────────────────────────────────┤
│ Entry Data (variable)                   │
└─────────────────────────────────────────┘
```

**Metadata Types:**
```c
#define OBJM_META_SIZE      0x01  // File size (8 bytes)
#define OBJM_META_MTIME     0x02  // Modification time (8 bytes, Unix timestamp)
#define OBJM_META_ETAG      0x03  // ETag (variable, string)
#define OBJM_META_MIME      0x04  // MIME type (variable, string)
#define OBJM_META_BACKEND   0x05  // Backend path ID (1 byte, for debugging)
#define OBJM_META_LATENCY   0x06  // Processing latency (4 bytes, microseconds)
```

**Example:**
```c
// Metadata: size=1234567, mtime=1698450000, backend_id=2
uint8_t metadata[] = {
    0x01, 0x00, 0x08,           // SIZE, length=8
    0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0xD6, 0x87,  // 1234567
    
    0x02, 0x00, 0x08,           // MTIME, length=8
    0x00, 0x00, 0x00, 0x00, 0x65, 0x48, 0x08, 0x70,  // 1698450000
    
    0x05, 0x00, 0x01,           // BACKEND, length=1
    0x02                        // backend path 2
};
```

## Connection Management

### Keep-Alive

**Purpose:** Reuse connection for multiple requests, avoiding connection overhead.

**Client responsibility:**
- Keep connection open between requests
- Send periodic heartbeat if idle (optional)
- Handle server-initiated close gracefully

**Server responsibility:**
- Keep connection open until client closes or timeout
- Configure idle timeout (default: 60 seconds)
- Send CLOSE message before timeout if needed

### Graceful Shutdown

```
Client                          Server
  |                               |
  |-- REQUEST_N ----------------->|
  |<- RESPONSE_N -----------------|
  |                               |
  |-- CLOSE ---------------------->|
  |   (no more requests)          |
  |                               |
  |   (wait for outstanding) -----|
  |<- RESPONSE_(pending) ---------|
  |                               |
  |<- CLOSE_ACK ------------------|
  |                               |
  |-- TCP/Unix FIN --------------->|
```

**CLOSE Message:**
```
┌─────────────────────────────────────────┐
│ Message Type (1 byte): 0x03 (CLOSE)    │
├─────────────────────────────────────────┤
│ Reason (1 byte):                        │
│   0x00 = Normal close                   │
│   0x01 = Idle timeout                   │
│   0x02 = Protocol error                 │
│   0x03 = Server shutting down           │
└─────────────────────────────────────────┘
```

**CLOSE_ACK Message:**
```
┌─────────────────────────────────────────┐
│ Message Type (1 byte): 0x04 (CLOSE_ACK)│
├─────────────────────────────────────────┤
│ Outstanding Requests (4 bytes)          │
│   Number of responses still to send     │
└─────────────────────────────────────────┘
```

### Connection Timeout

**Idle timeout:** Close connection after N seconds of inactivity.

**Request timeout:** Fail request if not completed within N seconds.

**Configuration:**
```c
typedef struct {
    int idle_timeout_sec;      // Close if idle (default: 60)
    int request_timeout_sec;   // Fail slow request (default: 30)
    int shutdown_grace_sec;    // Wait for pending on shutdown (default: 5)
} timeout_config_t;
```

## Protocol State Machine

### Client State Machine

```
┌─────────────┐
│ DISCONNECTED│
└──────┬──────┘
       │ connect()
       v
┌─────────────┐
│  CONNECTED  │
└──────┬──────┘
       │ send HELLO (v2)
       v
┌─────────────┐     ┌──────────────┐
│  HANDSHAKE  │────>│ READY (v1)   │ (no HELLO, assume v1)
└──────┬──────┘     └──────┬───────┘
       │ recv HELLO_ACK        │
       v                       │
┌─────────────┐                │
│    READY    │<───────────────┘
└──────┬──────┘
       │ send/recv requests/responses
       │<──────────────┐
       │               │
       │ send CLOSE    │
       v               │
┌─────────────┐        │
│   CLOSING   │        │
└──────┬──────┘        │
       │ recv CLOSE_ACK│
       v               │
┌─────────────┐        │
│    CLOSED   │────────┘ (can reconnect)
└─────────────┘
```

### Server State Machine (per connection)

```
┌─────────────┐
│   ACCEPT    │
└──────┬──────┘
       │ accept()
       v
┌─────────────┐
│  CONNECTED  │
└──────┬──────┘
       │ peek first byte
       ├─> 'O' (HELLO magic)
       │   └──> HANDSHAKE
       │
       └─> '1','2','3' (mode byte)
           └──> READY_V1
           
┌─────────────┐
│  HANDSHAKE  │
└──────┬──────┘
       │ send HELLO_ACK
       v
┌─────────────┐    ┌──────────────┐
│  READY_V2   │    │  READY_V1    │
└──────┬──────┘    └──────┬───────┘
       │                  │
       │ process requests │
       │<─────────────────┘
       │
       │ recv CLOSE
       v
┌─────────────┐
│   CLOSING   │
└──────┬──────┘
       │ send pending responses
       │ send CLOSE_ACK
       v
┌─────────────┐
│    CLOSED   │
└─────────────┘
```

## Implementation Considerations

### Backward Compatibility

**Version 1 clients to Version 2 servers:**
- Server detects no HELLO message (first byte is mode '1','2','3')
- Server assumes Version 1 protocol
- Server does NOT send request IDs in responses
- Server processes requests in order

**Version 2 clients to Version 1 servers:**
- Client sends HELLO
- Server doesn't recognize 'OBJM' magic (treats as invalid mode)
- Client receives error or connection close
- Client SHOULD fall back to Version 1 (no HELLO)

**Detection strategy:**
```c
// Server: peek first 4 bytes without consuming
uint8_t peek[4];
if (peek(sock, peek, 4) == 4 && memcmp(peek, "OBJM", 4) == 0) {
    // Version 2 client
    handle_hello();
} else if (peek[0] >= '1' && peek[0] <= '3') {
    // Version 1 client
    handle_v1_request();
} else {
    // Invalid protocol
    send_error();
}
```

### Thread Pool Integration

**With OOO replies:**
```c
void worker_thread(void *arg) {
    thread_pool_t *pool = arg;
    
    while (!pool->shutdown) {
        // Dequeue work from ANY connection (work-stealing)
        work_item_t *work = deque_work(pool);
        
        if (work) {
            // Process request
            response_t *resp = process_request(work->request);
            
            // Send response immediately (OOO)
            send_response(work->conn, resp);
        }
    }
}
```

**Without OOO replies (ordered):**
```c
void worker_thread(void *arg) {
    thread_pool_t *pool = arg;
    
    while (!pool->shutdown) {
        // Dequeue entire CONNECTION
        connection_t *conn = dequeue_connection(pool);
        
        while (connection_alive(conn)) {
            // Process requests in order
            request_t *req = recv_request(conn);
            response_t *resp = process_request(req);
            
            // Send response immediately (already in order)
            send_response(conn, resp);
        }
    }
}
```

### Multiple Backend Paths

**Scenario:** Server has multiple storage backends (local SSD, network NFS, S3)

**With OOO:**
```c
typedef struct {
    backend_type_t type;  // LOCAL, NFS, S3
    thread_pool_t *pool;  // Dedicated pool per backend
} backend_t;

void route_request(request_t *req) {
    backend_t *backend = select_backend(req->uri);
    
    // Enqueue to backend-specific pool
    enqueue_work(backend->pool, req);
    
    // Response sent when ready (OOO)
}
```

**Without OOO:**
```c
void handle_connection_ordered(connection_t *conn) {
    while (connection_alive(conn)) {
        request_t *req = recv_request(conn);
        
        // Select backend
        backend_t *backend = select_backend(req->uri);
        
        // Process synchronously (blocks until done)
        response_t *resp = backend_process(backend, req);
        
        // Send immediately (in order by definition)
        send_response(conn, resp);
    }
}
```

### Performance Impact

**OOO disabled (ordered mode):**
- ✅ Zero overhead - same as simple request/response
- ✅ No request tracking needed
- ✅ No response queueing overhead
- ❌ Head-of-line blocking if one request is slow

**OOO enabled:**
- ✅ No head-of-line blocking
- ✅ Can saturate multiple backends
- ✅ Better latency for fast requests
- ❌ Small overhead: request ID tracking (~8 bytes per request)
- ❌ Client complexity: must track outstanding requests

**Recommendation:**
- Default: OOO disabled for simplicity
- Enable OOO when:
  - Multiple backend paths with different latencies
  - Large variance in request processing times
  - High pipeline depth (>10 concurrent requests)

## Example Flows

### Example 1: Simple Version 1 Flow (Ordered)

```
Client -> Server: '1' (FD pass mode)
                  0x00 0x0E (URI length = 14)
                  "/tmp/test.dat"

Server -> Client: 0x00 (OK status)
                  [FD via SCM_RIGHTS]

Client -> Server: '2' (Copy mode)
                  0x00 0x0F (URI length = 15)
                  "/tmp/large.bin"

Server -> Client: 0x00 (OK status)
                  0x00 0x00 0x00 0x00 0x00 0x10 0x00 0x00 (size = 1048576)
                  [1MB of data...]
```

### Example 2: Version 2 with OOO

```
Client -> Server: "OBJM" (magic)
                  0x02 (version)
                  0x03 0x00 (caps: OOO | PIPELINING)
                  0x10 0x00 (max pipeline: 16)

Server -> Client: 0x00 (OK)
                  0x03 0x00 (server caps: OOO | PIPELINING)
                  0x08 0x00 (server max: 8)
                  0x04 (4 backend paths)

# Pipeline 3 requests
Client -> Server: 0x01 (REQUEST) 0x00 0x00 0x00 0x01 (id=1)
                  0x00 (no flags) '1' (FD pass)
                  0x00 0x09 "/slow.dat" (10GB file, slow backend)

Client -> Server: 0x01 (REQUEST) 0x00 0x00 0x00 0x02 (id=2)
                  0x00 (no flags) '1' (FD pass)
                  0x00 0x09 "/fast.dat" (1KB file, cache hit)

Client -> Server: 0x01 (REQUEST) 0x00 0x00 0x00 0x03 (id=3)
                  0x00 (no flags) '1' (FD pass)
                  0x00 0x0A "/medium.dat" (1MB file, fast backend)

# Responses arrive out-of-order
Server -> Client: 0x02 (RESPONSE) 0x00 0x00 0x00 0x02 (id=2, FAST)
                  0x00 (OK) 0x00 0x00 (no metadata)
                  [FD via SCM_RIGHTS]

Server -> Client: 0x02 (RESPONSE) 0x00 0x00 0x00 0x03 (id=3, MEDIUM)
                  0x00 (OK) 0x00 0x00 (no metadata)
                  [FD via SCM_RIGHTS]

# ... wait for slow backend ...

Server -> Client: 0x02 (RESPONSE) 0x00 0x00 0x00 0x01 (id=1, SLOW)
                  0x00 (OK) 0x00 0x00 (no metadata)
                  [FD via SCM_RIGHTS]
```

### Example 3: Mixed Ordered/OOO

```
Client -> Server: 0x01 (REQUEST) 0x00 0x00 0x00 0x01 (id=1)
                  0x00 (no flags) '1' "/file1.dat"

Client -> Server: 0x01 (REQUEST) 0x00 0x00 0x00 0x02 (id=2)
                  0x01 (ORDERED flag!) '1' "/critical.dat"
                  # This MUST be delivered after id=1

Client -> Server: 0x01 (REQUEST) 0x00 0x00 0x00 0x03 (id=3)
                  0x00 (no flags) '1' "/file3.dat"

# If id=2 completes before id=1, server MUST queue it
# If id=3 completes before id=2, it CAN be sent (no ORDERED flag)

Server -> Client: 0x02 (RESPONSE) 0x00 0x00 0x00 0x03 (id=3, can go first)
Server -> Client: 0x02 (RESPONSE) 0x00 0x00 0x00 0x01 (id=1)
Server -> Client: 0x02 (RESPONSE) 0x00 0x00 0x00 0x02 (id=2, after id=1)
```

## Security Considerations

### Request ID Prediction
- Request IDs SHOULD NOT be predictable across connections
- Use per-connection random starting offset
- Prevents request injection attacks

### Pipeline Depth Limits
- Server MUST enforce maximum pipeline depth
- Prevents resource exhaustion (OOM via too many pending requests)
- Recommended max: 1000 requests

### URI Length Limits
- URI length SHOULD be limited (recommended: 4KB max)
- Prevents buffer overflow attacks
- Return OBJM_STATUS_URI_TOO_LONG if exceeded

### Connection Limits
- Server SHOULD limit total concurrent connections
- Prevents DoS via connection exhaustion
- Recommended: 1000 connections per server

### Timeout Enforcement
- All timeouts MUST be enforced server-side
- Prevents resource leaks from hung connections
- Zombie connections reaped after idle timeout

## Future Extensions

### Compression (Reserved Capability Bit 2)
- Negotiate compression algorithm (gzip, zstd, lz4)
- Apply to URI strings and response data
- Metadata includes uncompressed size

### Multiplexing (Reserved Capability Bit 3)
- Multiple logical streams over one TCP connection
- Stream ID in addition to request ID
- Reduces connection overhead for multiple clients

### Push Notifications
- Server-initiated messages (invalidation, updates)
- Push message type (0x05)
- Client ACK required

### Batch Requests
- Send multiple URIs in one request
- Server responds with multiple FDs or batch data
- Reduces round-trips for multi-get operations

## Reference Implementation

See implementation files:
- `lib/protocol/protocol.h` - Protocol constants and structures
- `lib/protocol/protocol.c` - Encoding/decoding functions
- `objmapper/src/server_protocol.c` - Server-side protocol handling
- `objmapper/src/client_protocol.c` - Client-side protocol handling

## Testing

### Protocol Compliance Tests
- Version negotiation (v1, v2, mixed)
- Capability negotiation (OOO, pipelining)
- Request/response matching
- Error handling
- Graceful shutdown

### Performance Tests
- Pipeline saturation (measure throughput vs depth)
- OOO benefit (measure latency reduction)
- Overhead measurement (protocol bytes vs data bytes)

### Interoperability Tests
- V1 client to V2 server
- V2 client to V1 server (with fallback)
- Different capability combinations
