#!/bin/bash
# Test script for objmapper server with FD passing

set -e

OBJDIR="/home/dagge/src/objmapper"
SERVER="$OBJDIR/server"
CLIENT="$OBJDIR/client"
SOCKET="/tmp/objmapper.sock"

# Kill any existing server
pkill -f "$SERVER" 2>/dev/null || true
sleep 1

# Remove old socket
rm -f "$SOCKET"

# Start server in background
echo "Starting server..."
$SERVER &
SERVER_PID=$!

# Give server time to start
sleep 2

# Test data
echo "Creating test file..."
TEST_DATA="Hello from objmapper with FD passing! This is a test."
echo "$TEST_DATA" > /tmp/objmapper_test.txt

# Test 1: PUT
echo ""
echo "Test 1: PUT /testfile.txt"
$CLIENT put /testfile.txt /tmp/objmapper_test.txt

# Test 2: GET
echo ""
echo "Test 2: GET /testfile.txt"
$CLIENT get /testfile.txt /tmp/objmapper_retrieved.txt

# Verify content
echo ""
echo "Verifying content..."
RETRIEVED=$(cat /tmp/objmapper_retrieved.txt)
if [ "$RETRIEVED" == "$TEST_DATA" ]; then
    echo "✓ Content matches!"
else
    echo "✗ Content mismatch!"
    echo "Expected: $TEST_DATA"
    echo "Got: $RETRIEVED"
    kill $SERVER_PID
    exit 1
fi

# Test 3: PUT another file
echo ""
echo "Test 3: PUT /data/file2.txt"
echo "Second file content" > /tmp/test2.txt
$CLIENT put /data/file2.txt /tmp/test2.txt

# Test 4: LIST
echo ""
echo "Test 4: LIST objects"
$CLIENT list

# Test 5: DELETE
echo ""
echo "Test 5: DELETE /testfile.txt"
$CLIENT delete /testfile.txt

# Test 6: Try to GET deleted file (should fail)
echo ""
echo "Test 6: GET deleted file (should fail)"
if $CLIENT get /testfile.txt /tmp/should_fail.txt 2>&1 | grep -q "GET failed"; then
    echo "✓ GET correctly failed for deleted file"
else
    echo "✗ GET should have failed"
fi

# Cleanup
echo ""
echo "Stopping server..."
kill $SERVER_PID
wait $SERVER_PID 2>/dev/null || true

echo ""
echo "All tests passed!"
