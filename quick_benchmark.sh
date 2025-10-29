#!/bin/bash
# Quick benchmark test - single-threaded throughput

set -e

echo "=== Quick FD Passing Benchmark ==="
echo ""

# Ensure server is running
if ! pgrep -f "server" > /dev/null; then
    echo "ERROR: Server not running"
    exit 1
fi

# Create test data
dd if=/dev/urandom of=/tmp/bench_4k.bin bs=4096 count=1 2>/dev/null

echo "Testing PUT throughput (4KB objects, 100 operations)..."

START=$(date +%s%N)

for i in $(seq 1 100); do
    ./client put "/bench/test_$i.bin" /tmp/bench_4k.bin > /dev/null 2>&1 || {
        echo "PUT failed at iteration $i"
        exit 1
    }
done

ELAPSED=$(($(date +%s%N) - START))
ELAPSED_SEC=$(echo "scale=3; $ELAPSED / 1000000000" | bc)
OPS_PER_SEC=$(echo "scale=1; 100 / $ELAPSED_SEC" | bc)
MB_PER_SEC=$(echo "scale=2; (100 * 4) / $ELAPSED_SEC / 1024" | bc)

echo "PUT Results:"
echo "  Operations: 100"
echo "  Duration:   ${ELAPSED_SEC}s"
echo "  Throughput: ${OPS_PER_SEC} ops/sec"
echo "  Bandwidth:  ${MB_PER_SEC} MB/s"
echo ""

echo "Testing GET throughput (same 100 objects)..."

START=$(date +%s%N)

for i in $(seq 1 100); do
    ./client get "/bench/test_$i.bin" /tmp/bench_out.bin > /dev/null 2>&1 || {
        echo "GET failed at iteration $i"
        exit 1
    }
done

ELAPSED=$(($(date +%s%N) - START))
ELAPSED_SEC=$(echo "scale=3; $ELAPSED / 1000000000" | bc)
OPS_PER_SEC=$(echo "scale=1; 100 / $ELAPSED_SEC" | bc)
MB_PER_SEC=$(echo "scale=2; (100 * 4) / $ELAPSED_SEC / 1024" | bc)

echo "GET Results:"
echo "  Operations: 100"
echo "  Duration:   ${ELAPSED_SEC}s"
echo "  Throughput: ${OPS_PER_SEC} ops/sec"
echo "  Bandwidth:  ${MB_PER_SEC} MB/s"
echo ""

# Cleanup
echo "Cleaning up..."
for i in $(seq 1 100); do
    ./client delete "/bench/test_$i.bin" > /dev/null 2>&1 || true
done

rm -f /tmp/bench_4k.bin /tmp/bench_out.bin

echo "Benchmark complete!"
