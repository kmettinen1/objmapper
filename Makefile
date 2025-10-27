# Objmapper Makefile
# Clean, modular build system

CC = gcc
CFLAGS = -Wall -Wextra -O2 -pthread -I./objmapper/include
DEBUG_FLAGS = -g -DDEBUG
LDFLAGS = -pthread

# Directories
LIB_FDPASS = lib/fdpass
LIB_STORAGE = lib/storage
OBJMAPPER_SRC = objmapper/src
OBJMAPPER_INC = objmapper/include
BUILD_DIR = build

# Source files
FDPASS_SRC = $(LIB_FDPASS)/fdpass.c
STORAGE_SRC = $(LIB_STORAGE)/storage.c
SERVER_SRC = $(OBJMAPPER_SRC)/server.c
CLIENT_SRC = $(OBJMAPPER_SRC)/client.c
MAIN_SRC = $(OBJMAPPER_SRC)/main.c
TEST_CLIENT_SRC = $(OBJMAPPER_SRC)/test_client.c

# Object files
FDPASS_OBJ = $(BUILD_DIR)/fdpass.o
STORAGE_OBJ = $(BUILD_DIR)/storage.o
SERVER_OBJ = $(BUILD_DIR)/server.o
CLIENT_OBJ = $(BUILD_DIR)/client.o
MAIN_OBJ = $(BUILD_DIR)/main.o
TEST_CLIENT_OBJ = $(BUILD_DIR)/test_client.o

# Libraries
LIB_OBJMAPPER = $(BUILD_DIR)/libobjmapper.a

# Executables
OBJMAPPER_SERVER = $(BUILD_DIR)/objmapper-server
OBJMAPPER_TEST_CLIENT = $(BUILD_DIR)/objmapper-test-client

.PHONY: all clean debug install libs

all: $(BUILD_DIR) $(OBJMAPPER_SERVER) $(OBJMAPPER_TEST_CLIENT)

debug: CFLAGS += $(DEBUG_FLAGS)
debug: all

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Library objects
$(FDPASS_OBJ): $(FDPASS_SRC) $(LIB_FDPASS)/fdpass.h
	$(CC) $(CFLAGS) -c $< -o $@

$(STORAGE_OBJ): $(STORAGE_SRC) $(LIB_STORAGE)/storage.h
	$(CC) $(CFLAGS) -c $< -o $@

# Static library
$(LIB_OBJMAPPER): $(FDPASS_OBJ) $(STORAGE_OBJ)
	ar rcs $@ $^

# Server components
$(SERVER_OBJ): $(SERVER_SRC) $(OBJMAPPER_INC)/objmapper.h
	$(CC) $(CFLAGS) -c $< -o $@

$(MAIN_OBJ): $(MAIN_SRC) $(OBJMAPPER_INC)/objmapper.h
	$(CC) $(CFLAGS) -c $< -o $@

# Client components
$(CLIENT_OBJ): $(CLIENT_SRC) $(OBJMAPPER_INC)/objmapper.h
	$(CC) $(CFLAGS) -c $< -o $@

$(TEST_CLIENT_OBJ): $(TEST_CLIENT_SRC) $(OBJMAPPER_INC)/objmapper.h
	$(CC) $(CFLAGS) -c $< -o $@

# Executables
$(OBJMAPPER_SERVER): $(SERVER_OBJ) $(MAIN_OBJ) $(LIB_OBJMAPPER)
	$(CC) $(CFLAGS) $(SERVER_OBJ) $(MAIN_OBJ) $(LIB_OBJMAPPER) -o $@ $(LDFLAGS)

$(OBJMAPPER_TEST_CLIENT): $(CLIENT_OBJ) $(TEST_CLIENT_OBJ) $(LIB_OBJMAPPER)
	$(CC) $(CFLAGS) $(CLIENT_OBJ) $(TEST_CLIENT_OBJ) $(LIB_OBJMAPPER) -o $@ $(LDFLAGS)

clean:
	rm -rf $(BUILD_DIR)

install: all
	install -m 755 $(OBJMAPPER_SERVER) /usr/local/bin/
	install -m 755 $(OBJMAPPER_TEST_CLIENT) /usr/local/bin/

libs: $(LIB_OBJMAPPER)

help:
	@echo "Objmapper Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all                 Build all components (default)"
	@echo "  debug               Build with debug symbols and DEBUG defined"
	@echo "  clean               Remove all build artifacts"
	@echo "  install             Install to /usr/local/bin"
	@echo "  libs                Build static library only"
	@echo "  help                Show this help"
	@echo ""
	@echo "Components:"
	@echo "  lib/fdpass          File descriptor passing library"
	@echo "  lib/storage         Object storage with URI dictionary"
	@echo "  objmapper/src       Server and client implementations"
