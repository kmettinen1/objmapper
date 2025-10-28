# Objmapper Makefile
# Integrated build system for new architecture

CC = gcc
CFLAGS = -Wall -Wextra -O2 -I. -Ilib/protocol -Ilib/index -Ilib/backend -pthread -std=gnu11
LDFLAGS = -pthread -lm

# Libraries
PROTOCOL_LIB = lib/protocol/libobmprotocol.a
INDEX_LIB = lib/index/libobjindex.a
BACKEND_LIB = lib/backend/libobjbackend.a

ALL_LIBS = $(BACKEND_LIB) $(INDEX_LIB) $(PROTOCOL_LIB)

# Executables
DEMO = demo_integration
SERVER = server
CLIENT = client

.PHONY: all clean libs test new old

# Default target - build new architecture
all: new

# New architecture
new: libs $(SERVER) $(CLIENT) $(DEMO)

# Old architecture (legacy)
old:
	@echo "Building legacy objmapper..."
	$(MAKE) -f Makefile.old

# Build all libraries
libs:
	$(MAKE) -C lib/protocol
	$(MAKE) -C lib/index
	$(MAKE) -C lib/backend

# Integration demo
$(DEMO): demo_integration.c $(ALL_LIBS)
	$(CC) $(CFLAGS) $< $(BACKEND_LIB) $(INDEX_LIB) $(PROTOCOL_LIB) $(LDFLAGS) -o $@

# Server
$(SERVER): server.c $(ALL_LIBS)
	$(CC) $(CFLAGS) $< $(BACKEND_LIB) $(INDEX_LIB) $(PROTOCOL_LIB) $(LDFLAGS) -o $@

# Client
$(CLIENT): client.c $(PROTOCOL_LIB)
	$(CC) $(CFLAGS) $< $(PROTOCOL_LIB) $(LDFLAGS) -o $@

# Test
test: all
	@echo "Running component tests..."
	$(MAKE) -C lib/protocol test
	$(MAKE) -C lib/index test
	$(MAKE) -C lib/backend test
	@echo "All tests passed!"

# Clean
clean:
	rm -f $(DEMO) $(SERVER) $(CLIENT)
	$(MAKE) -C lib/protocol clean
	$(MAKE) -C lib/index clean
	$(MAKE) -C lib/backend clean

# Install
install: all
	install -d $(DESTDIR)/usr/local/bin
	install -m 755 $(DEMO) $(DESTDIR)/usr/local/bin/objmapper-demo
	$(MAKE) -C lib/protocol install
	$(MAKE) -C lib/index install
	$(MAKE) -C lib/backend install
