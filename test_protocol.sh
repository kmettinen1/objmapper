#!/bin/bash
# Quick test of the protocol library examples

set -e
set -o pipefail

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

echo "Running segmented reuse test..."
SEGMENT_REUSE_OUTPUT=$(./example_client "$SOCKET" "${TEST_FILE}::reuse" 4)
printf "%s\n" "$SEGMENT_REUSE_OUTPUT"
echo "$SEGMENT_REUSE_OUTPUT" | grep -q "Segmented payload: 3 segments" || {
    echo "Expected segmented reuse test to return 3 segments" >&2
    exit 1
}
echo "$SEGMENT_REUSE_OUTPUT" | grep -q "flags=0x03" || {
    echo "Expected final segment to reuse FD with FIN flag" >&2
    exit 1
}

echo "Running segmented optional inline test..."
SEGMENT_OPTIONAL_OUTPUT=$(./example_client "$SOCKET" "${TEST_FILE}::optional" 4)
printf "%s\n" "$SEGMENT_OPTIONAL_OUTPUT"
echo "$SEGMENT_OPTIONAL_OUTPUT" | grep -q "flags=0x04" || {
    echo "Expected optional inline segment flag" >&2
    exit 1
}

echo ""
echo "Test completed successfully!"
echo "Library built and examples working."
