# objmapper

Generic object storage optimized for efficient delivery

## Overview

objmapper is a high-performance object storage system designed to serve as a stevedore backend for Varnish and other HTTP caching systems. It provides efficient object storage with a URI-based dictionary and multiple delivery mechanisms optimized for zero-copy transfers.

## Features

- **Zero-Copy Delivery**: File descriptor passing eliminates data copying
- **Multiple Transfer Modes**: FD passing, traditional copy, and kernel splice
- **URI-Based Dictionary**: Fast hash-based object lookup
- **Memory-Mapped Caching**: Configurable cache with automatic management
- **Thread-Safe**: Concurrent access with reader-writer locks
- **Clean Modular Design**: Well-defined internal APIs and interfaces
- **Varnish Integration**: Designed as a stevedore backend for Varnish Cache

## Architecture

```
lib/
├── fdpass/      - File descriptor passing over Unix sockets
└── storage/     - Object storage with URI dictionary and caching

objmapper/
├── include/     - Public API headers
└── src/         - Server and client implementations
```

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for detailed architecture documentation.

## Building

```bash
make                  # Build all components
make debug            # Build with debug symbols
make libs             # Build libraries only
make clean            # Remove build artifacts
```

Built artifacts are placed in `build/`:
- `objmapper-server` - Main server executable
- `objmapper-test-client` - Test client
- `libobjmapper.a` - Static library

## Usage

### Starting the Server

```bash
./build/objmapper-server -b /path/to/backing/dir \
                         -c /path/to/cache/dir \
                         -l 1073741824 \
                         -s /tmp/objmapper.sock
```

Options:
- `-b DIR` - Backing directory for persistent storage (required)
- `-c DIR` - Cache directory for hot objects (optional)
- `-l SIZE` - Cache size limit in bytes (default: 1GB)
- `-s PATH` - Unix socket path (default: /tmp/objmapper.sock)
- `-m NUM` - Max concurrent connections (default: 10)

### Using the Test Client

```bash
# Request object via FD passing (zero-copy)
./build/objmapper-test-client -m 1 myobject.dat -o output.dat

# Request via traditional copy
./build/objmapper-test-client -m 2 myobject.dat -o output.dat

# Request via splice
./build/objmapper-test-client -m 3 myobject.dat -o output.dat
```

### Client API

```c
#include "objmapper.h"

// Connect to server
client_config_t config = {
    .socket_path = "/tmp/objmapper.sock",
    .operation_mode = OP_FDPASS  // or OP_COPY, OP_SPLICE
};
int sock = objmapper_client_connect(&config);

// Request object
int fd = objmapper_client_request(sock, "my-object-uri", OP_FDPASS);
// Use fd...
close(fd);

// Cleanup
objmapper_client_close(sock);
```

## API Documentation

### Storage Library (`lib/storage`)

```c
// Initialize storage
storage_config_t config = {
    .backing_dir = "/var/objmapper/data",
    .cache_dir = "/var/objmapper/cache",
    .cache_limit = 1024*1024*1024,  // 1GB
    .hash_size = 16384
};
object_storage_t *storage = storage_init(&config);

// Store object
storage_put(storage, "my-uri", data, size);

// Retrieve object
int fd = storage_get_fd(storage, "my-uri", NULL);
void *ptr = storage_get_mmap(storage, "my-uri", &size);

// Query stats
storage_get_stats(storage, &objects, &cached, &hits);

// Cleanup
storage_cleanup(storage);
```

### FD Passing Library (`lib/fdpass`)

```c
// Send file descriptor
fdpass_send(socket, NULL, fd, operation_type);

// Receive file descriptor
char op_type;
int received_fd = fdpass_recv(socket, &op_type);
```

## Performance

objmapper is optimized for:
- **Low Latency**: Direct file descriptor passing
- **High Throughput**: Kernel-level splice support
- **Memory Efficiency**: Configurable caching with mmap
- **Concurrency**: Multi-threaded with minimal lock contention

## Varnish Integration

objmapper can be integrated with Varnish as a custom storage backend (stevedore). This allows Varnish to delegate object storage while maintaining its HTTP caching logic.

## Legacy Code

The `datapass/` directory contains the original implementation that has been refactored into the modular architecture. It will be cleaned up in future releases.

## Development

### Project Structure

- `lib/fdpass/` - File descriptor passing library
- `lib/storage/` - Object storage implementation  
- `objmapper/` - Server and client
- `docs/` - Documentation
- `Makefile` - Build system

### Debug Build

```bash
make debug
DEBUG=1 ./build/objmapper-server -b ./data
```

## Contributing

Contributions are welcome! The codebase follows clean architecture principles:
- Modular components with well-defined interfaces
- No redundant code between modules
- Clear separation of concerns
- Thread-safe implementations

Please feel free to submit pull requests or open issues.

## Related Projects

- [Varnish Cache](https://github.com/kmettinen1/varnish-cache) - HTTP accelerator
- [H2O](https://github.com/kmettinen1/h2o) - HTTP/2 web server

## License

[To be determined]
