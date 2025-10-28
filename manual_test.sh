#!/bin/bash
# Simple manual test

cd /home/dagge/src/objmapper

# Kill old server
pkill -f "/home/dagge/src/objmapper/server" 2>/dev/null || true
sleep 1

# Start server
echo "Starting server..."
./server > /tmp/server.log 2>&1 &
SERVER_PID=$!
sleep 2

echo "Server PID: $SERVER_PID"

# Create test file
echo "Test content from FD passing!" > /tmp/mytest.txt

# Test PUT
echo ""
echo "=== Testing PUT ==="
./client put /test.txt /tmp/mytest.txt
echo "PUT done"

# Test GET
echo ""
echo "=== Testing GET ==="
./client get /test.txt /tmp/output.txt
echo "GET done"

# Show retrieved content
echo ""
echo "=== Retrieved content ==="
cat /tmp/output.txt

# Test DELETE
echo ""
echo "=== Testing DELETE ==="
./client delete /test.txt
echo "DELETE done"

# Show stats
echo ""
echo "=== Server log (last 20 lines) ==="
tail -20 /tmp/server.log

# Kill server
kill $SERVER_PID 2>/dev/null || true

echo ""
echo "Test complete!"
