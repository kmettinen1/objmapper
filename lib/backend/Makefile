# Makefile for objmapper backend library

CC = gcc
CFLAGS = -Wall -Wextra -O2 -fPIC -I. -I../index -pthread -std=gnu11
LDFLAGS = -pthread -lm
SHARED_LDFLAGS = -L../index -lobjindex -Wl,-rpath,'$$ORIGIN/../index'

# Library
LIB_NAME = libobjbackend
LIB_SRC = backend.c metadata_schema.c
LIB_OBJ = $(LIB_SRC:.c=.o)
LIB_STATIC = $(LIB_NAME).a
LIB_SHARED = $(LIB_NAME).so

# Dependencies
INDEX_LIB = ../index/libobjindex.a

# Tests
TEST_SRC = test_backend.c
TEST_BIN = test_backend

.PHONY: all clean test

all: $(LIB_STATIC) $(LIB_SHARED)

# Static library
$(LIB_STATIC): $(LIB_OBJ)
	ar rcs $@ $^

# Shared library
$(LIB_SHARED): $(LIB_OBJ) $(INDEX_LIB)
	$(CC) -shared -o $@ $(LIB_OBJ) $(SHARED_LDFLAGS) $(LDFLAGS)

# Object files
%.o: %.c backend.h metadata_schema.h ../index/index.h
	$(CC) $(CFLAGS) -c $< -o $@

# Test
test: $(TEST_BIN)
	LD_LIBRARY_PATH=.:../index ./$(TEST_BIN)

$(TEST_BIN): $(TEST_SRC) $(LIB_STATIC) $(INDEX_LIB)
	$(CC) $(CFLAGS) $< -L. -L../index -lobjbackend -lobjindex $(LDFLAGS) -o $@

# Clean
clean:
	rm -f $(LIB_OBJ) $(LIB_STATIC) $(LIB_SHARED) $(TEST_BIN)

# Install
install: $(LIB_STATIC) $(LIB_SHARED)
	install -d $(DESTDIR)/usr/local/lib
	install -d $(DESTDIR)/usr/local/include/objmapper
	install -m 644 $(LIB_STATIC) $(DESTDIR)/usr/local/lib/
	install -m 755 $(LIB_SHARED) $(DESTDIR)/usr/local/lib/
	install -m 644 backend.h $(DESTDIR)/usr/local/include/objmapper/
