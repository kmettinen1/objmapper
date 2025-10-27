# Objmapper Architecture

## Overview

Objmapper is a high-performance object storage system optimized for efficient delivery, designed as a stevedore backend for Varnish and other HTTP caching systems.

## Architecture

The system is built with clean, modular components:

```
objmapper/
├── lib/                      # Reusable library components
│   ├── fdpass/              # File descriptor passing over Unix sockets
│   │   ├── fdpass.h
│   │   └── fdpass.c
│   └── storage/             # Object storage with URI dictionary
│       ├── storage.h
│       └── storage.c
│
├── objmapper/               # Main server and client
│   ├── include/
│   │   └── objmapper.h      # Public API
│   ├── src/
│   │   ├── server.c         # Server implementation
│   │   ├── client.c         # Client library
│   │   ├── main.c           # Server entry point
│   │   └── test_client.c    # Test client
│   └── tests/               # Unit tests
│
├── datapass/                # Legacy implementation (to be cleaned)
└── datastore/               # Rust storage backend (future)
```

## Core Components

### 1. FD Passing Library (`lib/fdpass`)

Provides a clean interface for passing file descriptors between processes using Unix domain sockets and `SCM_RIGHTS`.

**API:**
- `fdpass_send()` - Send file descriptor with operation type
- `fdpass_recv()` - Receive file descriptor

**Key Features:**
- Zero-copy file descriptor transfer
- Support for both connected and unconnected sockets
- Operation type metadata

### 2. Storage Library (`lib/storage`)

Implements object storage with URI-based dictionary and optional memory-mapped caching.

**API:**
- `storage_init()` - Initialize storage with configuration
- `storage_put()` - Store object by URI
- `storage_get_fd()` - Get file descriptor for object
- `storage_get_mmap()` - Get memory-mapped object
- `storage_get_info()` - Query object metadata
- `storage_remove()` - Remove object
- `storage_get_stats()` - Get storage statistics
- `storage_cleanup()` - Cleanup resources

**Key Features:**
- Hash table based URI → object mapping
- Configurable cache with size limits
- Memory-mapped I/O for hot objects
- Thread-safe with reader-writer locks
- Automatic backing file management

**Data Structures:**
- Hash table with chaining for collision resolution
- Object slots with metadata (URI, size, hits, paths)
- Separate backing and cache file management

### 3. Objmapper Server (`objmapper/src/server.c`)

Main server process that accepts client connections and serves objects.

**Features:**
- Multi-threaded request handling
- Three operation modes:
  - `OP_FDPASS` - Zero-copy file descriptor passing
  - `OP_COPY` - Traditional data copying
  - `OP_SPLICE` - Kernel-level splice for efficiency
- Per-session operation mode
- Unix domain socket interface

**Flow:**
1. Initialize storage backend
2. Create and bind Unix socket
3. Accept client connections
4. Spawn thread per client
5. Read requests (URIs)
6. Lookup object in storage
7. Deliver via selected mode (fdpass/copy/splice)

### 4. Objmapper Client (`objmapper/src/client.c`)

Client library for connecting to objmapper server.

**API:**
- `objmapper_client_connect()` - Connect and negotiate mode
- `objmapper_client_request()` - Request object by URI
- `objmapper_client_close()` - Close connection

**Features:**
- Automatic mode negotiation
- Handles all three transfer modes
- Returns file descriptor for zero-copy access

## Data Flow

### FD Passing Mode (Zero-Copy)
```
Client                  Server                  Storage
  |                       |                        |
  |--URI request--------->|                        |
  |                       |--lookup URI----------->|
  |                       |<-return fd-------------|
  |<--FD via SCM_RIGHTS---|                        |
  |                       |                        |
  read from FD directly
```

### Copy Mode (Traditional)
```
Client                  Server                  Storage
  |                       |                        |
  |--URI request--------->|                        |
  |                       |--lookup URI----------->|
  |                       |<-return fd-------------|
  |<--size----------------|                        |
  |<--data chunks---------|                        |
  |                       |                        |
```

### Splice Mode (Kernel-Level)
```
Client                  Server                  Storage
  |                       |                        |
  |--URI request--------->|                        |
  |                       |--lookup URI----------->|
  |                       |<-return fd-------------|
  |<--size----------------|                        |
  |<--splice data---------|                        |
  |                       | (zero user-space copy) |
```

## Thread Safety

- **Storage**: Protected by `pthread_rwlock_t`
  - Read operations acquire read lock (concurrent)
  - Write operations acquire write lock (exclusive)
- **Server**: One thread per client connection
- **Client**: Not thread-safe (use separate connections)

## Memory Management

- **Storage slots**: Pre-allocated array with configurable capacity
- **Hash table**: Separate chaining for collision resolution
- **Cache**: Optional memory-mapped files with size limit
- **File descriptors**: 
  - Backing FDs kept open and dup'd for clients
  - Cache mmaps created on-demand

## Performance Optimizations

1. **Zero-Copy Transfers**: FD passing avoids data copying
2. **Memory Mapping**: Hot objects cached in memory
3. **Hash-Based Lookup**: O(1) average URI resolution
4. **Splice Support**: Kernel-level data transfer
5. **Read-Write Locks**: Concurrent read access
6. **Pre-allocation**: Avoid allocation overhead during requests

## Configuration

### Server
- Socket path
- Backing directory (persistent storage)
- Cache directory (optional)
- Cache size limit
- Max connections

### Client
- Socket path
- Operation mode (fdpass/copy/splice)

## Error Handling

- Return codes: 0 for success, -1 for failure
- `errno` set appropriately
- Debug logging with `DEBUG` compile flag
- Graceful degradation (cache miss → backing file)

## Future Enhancements

1. **Rust Storage Backend**: Replace C storage with Rust implementation
2. **Eviction Policies**: LRU, LFU for cache management
3. **Compression**: On-the-fly compression for large objects
4. **Replication**: Multi-node object storage
5. **Monitoring**: Metrics and statistics endpoints
6. **Hot Reload**: Dynamic configuration updates
