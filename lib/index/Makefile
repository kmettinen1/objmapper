# Makefile for objmapper index library

CC = gcc
CFLAGS = -Wall -Wextra -O2 -fPIC -I. -pthread -std=gnu11
LDFLAGS = -pthread -lm

# Library
LIB_NAME = libobjindex
LIB_SRC = index.c
LIB_OBJ = $(LIB_SRC:.c=.o)
LIB_STATIC = $(LIB_NAME).a
LIB_SHARED = $(LIB_NAME).so

# Tests
TEST_SRC = test_index.c
TEST_BIN = test_index

.PHONY: all clean test

all: $(LIB_STATIC) $(LIB_SHARED)

# Static library
$(LIB_STATIC): $(LIB_OBJ)
	ar rcs $@ $^

# Shared library
$(LIB_SHARED): $(LIB_OBJ)
	$(CC) -shared -o $@ $^ $(LDFLAGS)

# Object files
%.o: %.c index.h
	$(CC) $(CFLAGS) -c $< -o $@

# Test
test: $(TEST_BIN)
	LD_LIBRARY_PATH=. ./$(TEST_BIN)

$(TEST_BIN): $(TEST_SRC) $(LIB_STATIC)
	$(CC) $(CFLAGS) $< -L. -lobjindex $(LDFLAGS) -o $@

# Clean
clean:
	rm -f $(LIB_OBJ) $(LIB_STATIC) $(LIB_SHARED) $(TEST_BIN)

# Install
install: $(LIB_STATIC) $(LIB_SHARED)
	install -d $(DESTDIR)/usr/local/lib
	install -d $(DESTDIR)/usr/local/include/objmapper
	install -m 644 $(LIB_STATIC) $(DESTDIR)/usr/local/lib/
	install -m 755 $(LIB_SHARED) $(DESTDIR)/usr/local/lib/
	install -m 644 index.h $(DESTDIR)/usr/local/include/objmapper/
