#!/bin/bash
# Quick test script for objmapper

set -e

echo "=== Objmapper Test Suite ==="
echo

# Setup
TEST_DIR=$(mktemp -d)
BACKING_DIR="$TEST_DIR/backing"
CACHE_DIR="$TEST_DIR/cache"
SOCK_PATH="$TEST_DIR/objmapper.sock"

mkdir -p "$BACKING_DIR" "$CACHE_DIR"

echo "Test directory: $TEST_DIR"
echo "Backing: $BACKING_DIR"
echo "Cache: $CACHE_DIR"
echo "Socket: $SOCK_PATH"
echo

# Create test objects
echo "Creating test objects..."
echo "Hello, World!" > "$BACKING_DIR/test1.txt"
dd if=/dev/urandom of="$BACKING_DIR/test2.bin" bs=1024 count=10 2>/dev/null
echo "Test objects created"
echo

# Start server in background
echo "Starting objmapper server..."
./build/objmapper-server -b "$BACKING_DIR" -c "$CACHE_DIR" -l 10485760 -s "$SOCK_PATH" &
SERVER_PID=$!

# Wait for server to start
sleep 1

if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo "ERROR: Server failed to start"
    exit 1
fi

echo "Server running (PID: $SERVER_PID)"
echo

# Test 1: FD passing mode
echo "Test 1: FD passing mode"
./build/objmapper-test-client -s "$SOCK_PATH" -m 1 test1.txt -o "$TEST_DIR/output1.txt"
if diff -q "$BACKING_DIR/test1.txt" "$TEST_DIR/output1.txt" > /dev/null; then
    echo "✓ FD passing mode: PASS"
else
    echo "✗ FD passing mode: FAIL"
fi
echo

# Test 2: Copy mode
echo "Test 2: Copy mode"
./build/objmapper-test-client -s "$SOCK_PATH" -m 2 test2.bin -o "$TEST_DIR/output2.bin"
if diff -q "$BACKING_DIR/test2.bin" "$TEST_DIR/output2.bin" > /dev/null; then
    echo "✓ Copy mode: PASS"
else
    echo "✗ Copy mode: FAIL"
fi
echo

# Test 3: Splice mode
echo "Test 3: Splice mode"
./build/objmapper-test-client -s "$SOCK_PATH" -m 3 test1.txt -o "$TEST_DIR/output3.txt"
if diff -q "$BACKING_DIR/test1.txt" "$TEST_DIR/output3.txt" > /dev/null; then
    echo "✓ Splice mode: PASS"
else
    echo "✗ Splice mode: FAIL"
fi
echo

# Cleanup
echo "Cleaning up..."
kill $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true
rm -rf "$TEST_DIR"

echo "=== All tests completed ==="
