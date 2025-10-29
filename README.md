# objmapper# objmapper



High-performance object storage with zero-copy file descriptor passing over Unix domain sockets.Generic object storage optimized for efficient delivery



**Version:** 0.1  ## Overview

**Status:** Production-ready core functionality

objmapper is a high-performance object storage system designed to serve as a stevedore backend for Varnish and other HTTP caching systems. It provides efficient object storage with a URI-based dictionary and multiple delivery mechanisms optimized for zero-copy transfers.

## Overview

## Features

objmapper is a specialized object storage system optimized for zero-copy object delivery. It uses file descriptor passing over Unix domain sockets to eliminate data copying, achieving throughput of 495K ops/sec with sub-millisecond latency.

- **Multiple Transport Types**: Unix sockets (primary), TCP, and UDP

**Primary Use Case:** Backend for HTTP caching systems like Varnish Cache- **Zero-Copy Delivery**: File descriptor passing eliminates data copying (Unix sockets)

- **Multiple Transfer Modes**: FD passing, traditional copy, and kernel splice

## Key Features- **URI-Based Dictionary**: Fast hash-based object lookup

- **Memory-Mapped Caching**: Configurable cache with automatic management

- **Zero-Copy Delivery**: File descriptor passing via `SCM_RIGHTS` eliminates data copying- **Thread-Safe**: Concurrent access with reader-writer locks

- **High Performance**: 495K ops/sec with 16 concurrent clients, 1.9 GB/s throughput- **Clean Modular Design**: Well-defined internal APIs and interfaces

- **Low Latency**: 0.01ms average response time for small objects- **Capability Detection**: Automatic validation of transport-mode compatibility

- **Two-Tier Storage**: Memory-based cache with persistent disk backend- **Varnish Integration**: Designed as a stevedore backend for Varnish Cache

- **Automatic Management**: LRU eviction, automatic tier promotion/demotion

- **Thread-Safe**: Bucket-level RW locks for high concurrency## Architecture

- **100% Reliability**: Zero errors under sustained load

```

## Quick Startlib/

├── fdpass/      - File descriptor passing over Unix sockets

### Build├── storage/     - Object storage with URI dictionary and caching

└── transport/   - Multi-transport abstraction (Unix/TCP/UDP)

```bash

make clean && makeobjmapper/

```├── include/     - Public API headers

└── src/         - Server and client implementations

### Run Server```



```bashSee documentation:

./server- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) - Overall system architecture

```- [docs/TRANSPORT.md](docs/TRANSPORT.md) - Transport abstraction layer

- [docs/REFACTORING.md](docs/REFACTORING.md) - Modular refactoring details

Server listens on `/tmp/objmapper.sock` by default.

## Building

### Test with Client

```bash

```bashmake                  # Build all components

# Upload a filemake debug            # Build with debug symbols

./client put /data/test.txt myfile.txtmake libs             # Build libraries only

make clean            # Remove build artifacts

# Download it back```

./client get /data/test.txt output.txt

Built artifacts are placed in `build/`:

# Delete it- `objmapper-server` - Main server executable

./client delete /data/test.txt- `objmapper-test-client` - Test client

```- `libobjmapper.a` - Static library



## Architecture## Usage



```### Starting the Server

┌─────────────────────────────────────────────────────────────┐

│                      Client Application                     │#### Unix Socket Mode (Primary - supports FD passing)

└────────────────────┬────────────────────────────────────────┘```bash

                     │ Unix Socket + SCM_RIGHTS./build/objmapper-server -t unix -s /tmp/objmapper.sock -b /data

┌────────────────────▼────────────────────────────────────────┐```

│                     objmapper Server                         │

│  ┌──────────────────────────────────────────────────────┐   │#### TCP Mode (Network - copy/splice only)

│  │  Protocol Layer  → Request/Response + FD Passing     │   │```bash

│  └────────────┬─────────────────────────────────────────┘   │./build/objmapper-server -t tcp -H 0.0.0.0 -p 9999 -b /data

│  ┌────────────▼─────────────────────────────────────────┐   │```

│  │  Global Index    → URI → Object Mapping             │   │

│  └────────────┬─────────────────────────────────────────┘   │#### UDP Mode (Experimental)

│  ┌────────────▼─────────────────────────────────────────┐   │```bash

│  │  Backend Manager → Memory + Persistent Storage       │   │./build/objmapper-server -t udp -H 0.0.0.0 -p 9998 -b /data

│  └──────────────────────────────────────────────────────┘   │```

└─────────────────────────────────────────────────────────────┘

```Server Options:

- `-t TYPE` - Transport type: unix (default), tcp, udp

### Components- `-s PATH` - Unix socket path (default: /tmp/objmapper.sock)

- `-H HOST` - Host for TCP/UDP (default: *)

- **Protocol Layer** (`lib/protocol/`): Wire protocol with FD passing support- `-p PORT` - Port for TCP/UDP (default: 9999/9998)

- **Global Index** (`lib/index/`): URI-based dictionary with FD lifecycle management  - `-b DIR` - Backing directory for persistent storage (required)

- **Backend Manager** (`lib/backend/`): Two-tier storage with automatic eviction- `-c DIR` - Cache directory for hot objects (optional)

- **Server** (`server.c`): Multi-threaded server with per-connection handlers- `-l SIZE` - Cache size limit in bytes (default: 1GB)

- **Client** (`client.c`): Command-line client for testing- `-m NUM` - Max concurrent connections (default: 10)



See [ARCHITECTURE.md](ARCHITECTURE.md) for detailed design documentation.### Using the Test Client



## Performance#### FD Passing Mode (Unix sockets only)

```bash

**Benchmark Results (v0.1)**:./build/objmapper-test-client -t unix -m 1 /path/to/object -o output.dat

```

| Test | Throughput | Latency | Notes |

|------|------------|---------|-------|#### Copy Mode (all transports)

| Single-threaded (1KB) | 75K ops/sec | 0.01ms | Long-lived connection |```bash

| 16 threads (4KB) | 495K ops/sec | 0.03ms | 1.9 GB/s throughput |# Unix socket

| Large objects (1MB) | 12 GB/s | 0.6ms | Memory-to-memory |./build/objmapper-test-client -t unix -m 2 /path/to/object -o output.dat

| Reconnect vs long-lived | 6x difference | - | Connection reuse critical |

# TCP

**Test Environment**:./build/objmapper-test-client -t tcp -H localhost -p 9999 -m 2 /path/to/object

- 5-second sustained load per test

- Mixed 50% read / 50% write workload# UDP

- Memory backend (tmpfs)./build/objmapper-test-client -t udp -H localhost -p 9998 -m 2 /path/to/object

- Error rate: 0%```



Run benchmarks:#### Splice Mode (stream transports only)

```bash```bash

make benchmark./build/objmapper-test-client -t unix -m 3 /path/to/object -o output.dat

./benchmark./build/objmapper-test-client -t tcp -H localhost -p 9999 -m 3 /path/to/object

``````



## API UsageClient Options:

- `-t TYPE` - Transport type: unix (default), tcp, udp

### Client API- `-s PATH` - Unix socket path (default: /tmp/objmapper.sock)

- `-H HOST` - Host for TCP/UDP (default: localhost)

```c- `-p PORT` - Port for TCP/UDP (default: 9999/9998)

#include "lib/protocol/protocol.h"- `-m MODE` - Operation mode: 1=fdpass, 2=copy, 3=splice (default: 1)

- `-o FILE` - Output file (default: stdout)

// Connect to server

int sock = objm_client_connect("/tmp/objmapper.sock");### Client API

objm_connection_t *conn = objm_client_create(sock, OBJM_PROTO_V1);

```c

// PUT: Upload object#include "objmapper.h"

objm_request_t req = {

    .id = 0,// Unix socket with FD passing

    .flags = 0,client_config_t config = {

    .mode = OBJM_MODE_FDPASS,    .transport = OBJMAPPER_TRANSPORT_UNIX,

    .uri = "/my/object",    .socket_path = "/tmp/objmapper.sock",

    .uri_len = strlen("/my/object")    .operation_mode = OP_FDPASS

};};

objm_client_send_request(conn, &req);

// Or TCP with copy mode

objm_response_t *resp;client_config_t config = {

objm_client_recv_response(conn, &resp);    .transport = OBJMAPPER_TRANSPORT_TCP,

    .net = { .host = "localhost", .port = 9999 },

if (resp->status == OBJM_STATUS_OK) {    .operation_mode = OP_COPY

    int fd = resp->fd;};

    resp->fd = -1;  // Prevent objm_response_free() from closing FD

    objm_response_free(resp);// Connect to server

        .socket_path = "/tmp/objmapper.sock",

    write(fd, data, size);    .operation_mode = OP_FDPASS  // or OP_COPY, OP_SPLICE

    close(fd);};

}int sock = objmapper_client_connect(&config);



// GET: Download object// Request object

objm_client_send_request(conn, &req);int fd = objmapper_client_request(sock, "my-object-uri", OP_FDPASS);

objm_client_recv_response(conn, &resp);// Use fd...

close(fd);

if (resp->status == OBJM_STATUS_OK) {

    int fd = resp->fd;// Cleanup

    resp->fd = -1;  // Prevent premature closureobjmapper_client_close(sock);

    objm_response_free(resp);```

    

    read(fd, buffer, size);## API Documentation

    close(fd);

}### Storage Library (`lib/storage`)



// Cleanup```c

objm_client_destroy(conn);// Initialize storage

close(sock);storage_config_t config = {

```    .backing_dir = "/var/objmapper/data",

    .cache_dir = "/var/objmapper/cache",

**Critical**: Always set `resp->fd = -1` before calling `objm_response_free()` to prevent the library from closing the FD you're about to use.    .cache_limit = 1024*1024*1024,  // 1GB

    .hash_size = 16384

## How It Works};

object_storage_t *storage = storage_init(&config);

### File Descriptor Passing

// Store object

objmapper uses `SCM_RIGHTS` ancillary messages over Unix domain sockets to pass file descriptors between processes:storage_put(storage, "my-uri", data, size);



1. Server opens a file and gets FD (e.g., FD 5 in server process)// Retrieve object

2. Server sends FD via `sendmsg()` with `SCM_RIGHTS`int fd = storage_get_fd(storage, "my-uri", NULL);

3. Kernel creates new FD in client process (e.g., FD 4 in client process)void *ptr = storage_get_mmap(storage, "my-uri", &size);

4. Both FDs refer to the same underlying file description

5. Client can now read/write the file without any data copying// Query stats

storage_get_stats(storage, &objects, &cached, &hits);

This enables **true zero-copy** transfers - the data never leaves kernel memory.

// Cleanup

### Two-Tier Storagestorage_cleanup(storage);

```

Objects are automatically placed in the best available backend:

### FD Passing Library (`lib/fdpass`)

1. **Memory Tier** (fast): tmpfs-backed, 1GB limit, LRU eviction

2. **Persistent Tier** (large): disk-backed, 20GB limit, survives restarts```c

// Send file descriptor

The system automatically:fdpass_send(socket, NULL, fd, operation_type);

- Promotes hot objects to memory tier

- Evicts cold objects when space is low// Receive file descriptor

- Rebuilds index from filesystem on startupchar op_type;

int received_fd = fdpass_recv(socket, &op_type);

### Concurrency Model```



- **Thread per connection**: Each client gets a dedicated pthread (detached)## Performance

- **Bucket-level locks**: Index uses RW locks per hash bucket

- **Reference counting**: Safe FD sharing across threadsobjmapper is optimized for:

- **Atomic statistics**: Lock-free performance counters- **Low Latency**: Direct file descriptor passing

- **High Throughput**: Kernel-level splice support

## Configuration- **Memory Efficiency**: Configurable caching with mmap

- **Concurrency**: Multi-threaded with minimal lock contention

### Compile-Time Settings

## Varnish Integration

Edit `lib/backend/backend.h`:

```cobjmapper can be integrated with Varnish as a custom storage backend (stevedore). This allows Varnish to delegate object storage while maintaining its HTTP caching logic.

#define MEMORY_CACHE_SIZE (1024*1024*1024)      // 1GB memory tier

#define PERSISTENT_SIZE (20ULL*1024*1024*1024)  // 20GB persistent tier## Legacy Code

#define MAX_OBJECTS 100000

```The `datapass/` directory contains the original implementation that has been refactored into the modular architecture. It will be cleaned up in future releases.



Edit `lib/index/index.h`:## Development

```c

#define DEFAULT_BUCKET_COUNT 16384   // Hash buckets### Project Structure

#define DEFAULT_MAX_OPEN_FDS 1024    // FD limit

```- `lib/fdpass/` - File descriptor passing library

- `lib/storage/` - Object storage implementation  

### Runtime Configuration- `objmapper/` - Server and client

- `docs/` - Documentation

Server uses hardcoded paths (can be modified in `server.c`):- `Makefile` - Build system

- Socket: `/tmp/objmapper.sock`

- Memory backend: `/tmp/objmapper_memory/data/`### Debug Build

- Persistent backend: `/tmp/objmapper_persistent/data/`

```bash

## Project Structuremake debug

DEBUG=1 ./build/objmapper-server -b ./data

``````

objmapper/

├── lib/## Contributing

│   ├── protocol/      # Wire protocol and FD passing

│   ├── index/         # URI dictionary and FD managementContributions are welcome! The codebase follows clean architecture principles:

│   └── backend/       # Two-tier storage system- Modular components with well-defined interfaces

├── server.c           # Multi-threaded server- No redundant code between modules

├── client.c           # Test client- Clear separation of concerns

├── benchmark.c        # Performance benchmark suite- Thread-safe implementations

├── ARCHITECTURE.md    # Detailed design documentation

└── README.md          # This filePlease feel free to submit pull requests or open issues.

```

## Related Projects

## Dependencies

- [Varnish Cache](https://github.com/kmettinen1/varnish-cache) - HTTP accelerator

- **OS**: Linux 3.0+ (Unix sockets with SCM_RIGHTS)- [H2O](https://github.com/kmettinen1/h2o) - HTTP/2 web server

- **Compiler**: GCC 4.9+ or Clang 3.4+ (C11 support)

- **Libraries**: None (pure POSIX C)## License



## Known Limitations (v0.1)[To be determined]


- Unix sockets only (no TCP/UDP)
- No authentication or access control
- Single server instance only
- No hot backup or replication
- Index not persisted (rebuilt on restart)
- No splice mode (future enhancement)

## Future Roadmap

**v0.2 - Network Support**:
- TCP/UDP transport option
- Splice mode for network zero-copy
- Protocol V2 with pipelining

**v0.3 - Production Features**:
- Index persistence
- Statistics API
- Hot object detection
- FD pooling optimization

**v1.0 - Varnish Integration**:
- Varnish stevedore backend module
- Full production deployment
- Performance tuning

## Testing

```bash
# Unit tests
make test

# Integration tests  
./test.sh

# Performance benchmark
make benchmark
./benchmark

# Manual testing
./manual_test.sh
```

## Contributing

This project follows clean architecture principles:
- Modular components with well-defined interfaces
- No code duplication between modules
- Thread-safe implementations
- Comprehensive error handling

Pull requests welcome!

## License

[To be determined]

## References

- [ARCHITECTURE.md](ARCHITECTURE.md) - Detailed design documentation
- [FDPASS_STATUS.md](FDPASS_STATUS.md) - FD passing implementation notes
- Stevens, *UNIX Network Programming Vol 1*, Chapter 15 - Unix Domain Protocols
- `man 7 unix` - Unix domain sockets
- `man 3 sendmsg` - SCM_RIGHTS control messages

## Related Projects

- [Varnish Cache](https://varnish-cache.org/) - HTTP accelerator
- [nginx](https://nginx.org/) - HTTP server with caching

---

**Note**: This is v0.1 with core Unix socket FD passing functionality. TCP/UDP support and additional features are planned for future releases.
