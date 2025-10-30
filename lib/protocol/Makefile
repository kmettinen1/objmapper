# Makefile for objmapper protocol library

CC = gcc
CFLAGS = -Wall -Wextra -O2 -fPIC -I. -I../../include
LDFLAGS = 

# Library
LIB_NAME = libobmprotocol
LIB_SRC = protocol.c
LIB_OBJ = $(LIB_SRC:.c=.o)
LIB_STATIC = $(LIB_NAME).a
LIB_SHARED = $(LIB_NAME).so

# Examples
EXAMPLE_CLIENT = example_client
EXAMPLE_SERVER = example_server

.PHONY: all clean install examples

all: $(LIB_STATIC) $(LIB_SHARED)

test:
	@echo "No protocol-specific tests"

# Static library
$(LIB_STATIC): $(LIB_OBJ)
	ar rcs $@ $^

# Shared library
$(LIB_SHARED): $(LIB_OBJ)
	$(CC) -shared -o $@ $^

# Object files
%.o: %.c protocol.h
	$(CC) $(CFLAGS) -c $< -o $@

# Examples
examples: $(EXAMPLE_CLIENT) $(EXAMPLE_SERVER)

$(EXAMPLE_CLIENT): ../../examples/example_client.c $(LIB_STATIC)
	$(CC) $(CFLAGS) $< -L. -lobmprotocol -o $@

$(EXAMPLE_SERVER): ../../examples/example_server.c $(LIB_STATIC)
	$(CC) $(CFLAGS) $< -L. -lobmprotocol -o $@

# Install
install: $(LIB_STATIC) $(LIB_SHARED)
	install -d $(DESTDIR)/usr/local/lib
	install -d $(DESTDIR)/usr/local/include/objmapper
	install -m 644 $(LIB_STATIC) $(DESTDIR)/usr/local/lib/
	install -m 755 $(LIB_SHARED) $(DESTDIR)/usr/local/lib/
	install -m 644 protocol.h $(DESTDIR)/usr/local/include/objmapper/

# Clean
clean:
	rm -f $(LIB_OBJ) $(LIB_STATIC) $(LIB_SHARED)
	rm -f $(EXAMPLE_CLIENT) $(EXAMPLE_SERVER)
