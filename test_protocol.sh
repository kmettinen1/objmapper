#!/bin/bash
# Quick test of the protocol library examples

set -e

SOCKET="/tmp/objmapper_test_$$.sock"
TEST_FILE="/tmp/objmapper_test_file_$$.txt"

# Cleanup on exit
cleanup() {
    rm -f "$SOCKET" "$TEST_FILE"
    if [ -n "$SERVER_PID" ]; then
        kill $SERVER_PID 2>/dev/null || true
    fi
}
trap cleanup EXIT

# Create test file
echo "Hello from objmapper protocol test!" > "$TEST_FILE"

cd lib/protocol

export LD_LIBRARY_PATH="$(pwd):${LD_LIBRARY_PATH:-}"

echo "Starting server..."
./example_server "$SOCKET" &
SERVER_PID=$!

# Give server time to start
sleep 1

echo "Running client test..."
./example_client "$SOCKET" "$TEST_FILE" 1

echo "Running segmented client test..."
./example_client "$SOCKET" "$TEST_FILE" 4

echo ""
echo "Test completed successfully!"
echo "Library built and examples working."
