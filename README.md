# objmapper

Generic object storage optimized for efficient delivery

## Overview

objmapper is a high-performance cache daemon designed to serve as a stevedore backend for Varnish and other HTTP caching systems. It provides efficient object storage with optimized delivery mechanisms for web content acceleration.

## Features

- **High Performance**: Optimized for low-latency object retrieval
- **Varnish Integration**: Designed as a stevedore backend for Varnish Cache
- **Memory-Mapped I/O**: Efficient data access using mmap
- **Network-Optimized**: Fast client-server communication protocols
- **Scalable Architecture**: Handles large object stores efficiently

## Components

### datapass
Client-server framework for efficient data transfer:
- `server.c` - Server implementation for object serving
- `client.c` - Client library for object retrieval
- `bridge.c` - Bridge component for protocol translation
- `sendget.c/h` - Core send/get operations

### datastore
Object storage backend implementation

## Building

```bash
cd datapass/csrc
make
```

## Usage

### Starting the Server
```bash
./server [options]
```

### Client Integration
Link against the client library to integrate with your caching system.

## Varnish Integration

objmapper can be used as a custom storage backend for Varnish through the stevedore interface. This allows Varnish to delegate object storage to objmapper while maintaining its own caching logic.

## Performance

Performance benchmarks and test results are available in the `datapass/csrc/w-10Mpmap-1stride-10Gcache/` directory.

## License

[To be determined]

## Contributing

Contributions are welcome! Please feel free to submit pull requests or open issues.

## Related Projects

- [Varnish Cache](https://github.com/kmettinen1/varnish-cache) - HTTP accelerator
- [H2O](https://github.com/kmettinen1/h2o) - HTTP/2 web server

## Contact

For questions or support, please open an issue on GitHub.
