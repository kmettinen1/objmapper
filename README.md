# objmapper

Generic object storage optimized for efficient delivery

## Overview

objmapper is a high-performance object storage system designed to serve as a stevedore backend for Varnish and other HTTP caching systems. It provides efficient object storage with a URI-based dictionary and multiple delivery mechanisms optimized for zero-copy transfers.

## Features

- **Multiple Transport Types**: Unix sockets (primary), TCP, and UDP
- **Zero-Copy Delivery**: File descriptor passing eliminates data copying (Unix sockets)
- **Multiple Transfer Modes**: FD passing, traditional copy, and kernel splice
- **URI-Based Dictionary**: Fast hash-based object lookup
- **Memory-Mapped Caching**: Configurable cache with automatic management
- **Thread-Safe**: Concurrent access with reader-writer locks
- **Clean Modular Design**: Well-defined internal APIs and interfaces
- **Capability Detection**: Automatic validation of transport-mode compatibility
- **Varnish Integration**: Designed as a stevedore backend for Varnish Cache

## Architecture

```
lib/
├── fdpass/      - File descriptor passing over Unix sockets
├── storage/     - Object storage with URI dictionary and caching
└── transport/   - Multi-transport abstraction (Unix/TCP/UDP)

objmapper/
├── include/     - Public API headers
└── src/         - Server and client implementations
```

See documentation:
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) - Overall system architecture
- [docs/TRANSPORT.md](docs/TRANSPORT.md) - Transport abstraction layer
- [docs/REFACTORING.md](docs/REFACTORING.md) - Modular refactoring details

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

#### Unix Socket Mode (Primary - supports FD passing)
```bash
./build/objmapper-server -t unix -s /tmp/objmapper.sock -b /data
```

#### TCP Mode (Network - copy/splice only)
```bash
./build/objmapper-server -t tcp -H 0.0.0.0 -p 9999 -b /data
```

#### UDP Mode (Experimental)
```bash
./build/objmapper-server -t udp -H 0.0.0.0 -p 9998 -b /data
```

Server Options:
- `-t TYPE` - Transport type: unix (default), tcp, udp
- `-s PATH` - Unix socket path (default: /tmp/objmapper.sock)
- `-H HOST` - Host for TCP/UDP (default: *)
- `-p PORT` - Port for TCP/UDP (default: 9999/9998)
- `-b DIR` - Backing directory for persistent storage (required)
- `-c DIR` - Cache directory for hot objects (optional)
- `-l SIZE` - Cache size limit in bytes (default: 1GB)
- `-m NUM` - Max concurrent connections (default: 10)

### Using the Test Client

#### FD Passing Mode (Unix sockets only)
```bash
./build/objmapper-test-client -t unix -m 1 /path/to/object -o output.dat
```

#### Copy Mode (all transports)
```bash
# Unix socket
./build/objmapper-test-client -t unix -m 2 /path/to/object -o output.dat

# TCP
./build/objmapper-test-client -t tcp -H localhost -p 9999 -m 2 /path/to/object

# UDP
./build/objmapper-test-client -t udp -H localhost -p 9998 -m 2 /path/to/object
```

#### Splice Mode (stream transports only)
```bash
./build/objmapper-test-client -t unix -m 3 /path/to/object -o output.dat
./build/objmapper-test-client -t tcp -H localhost -p 9999 -m 3 /path/to/object
```

Client Options:
- `-t TYPE` - Transport type: unix (default), tcp, udp
- `-s PATH` - Unix socket path (default: /tmp/objmapper.sock)
- `-H HOST` - Host for TCP/UDP (default: localhost)
- `-p PORT` - Port for TCP/UDP (default: 9999/9998)
- `-m MODE` - Operation mode: 1=fdpass, 2=copy, 3=splice (default: 1)
- `-o FILE` - Output file (default: stdout)

### Client API

```c
#include "objmapper.h"

// Unix socket with FD passing
client_config_t config = {
    .transport = OBJMAPPER_TRANSPORT_UNIX,
    .socket_path = "/tmp/objmapper.sock",
    .operation_mode = OP_FDPASS
};

// Or TCP with copy mode
client_config_t config = {
    .transport = OBJMAPPER_TRANSPORT_TCP,
    .net = { .host = "localhost", .port = 9999 },
    .operation_mode = OP_COPY
};

// Connect to server
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
