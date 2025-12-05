#!/bin/bash

# Integration test for TeXpresso WebSocket server
# Tests the server with headless TeXpresso

set -e

cd "$(dirname "$0")"

echo "=========================================="
echo "TeXpresso WebSocket Server Integration Test"
echo "=========================================="
echo

# Check if texpresso binary exists
if [ ! -x "../build/texpresso" ]; then
    echo "ERROR: texpresso binary not found at ../build/texpresso"
    echo "Please build texpresso first"
    exit 1
fi

# Check if test document exists
if [ ! -f "../test/simple.tex" ]; then
    echo "ERROR: test document not found at ../test/simple.tex"
    exit 1
fi

echo "Step 1: Starting WebSocket server..."
PORT=8181 VERBOSE=true npm start &
SERVER_PID=$!

# Give server time to start
sleep 2

# Check if server is running
if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo "ERROR: Server failed to start"
    exit 1
fi

echo "Server started (PID: $SERVER_PID)"
echo

# Cleanup function
cleanup() {
    echo
    echo "Cleaning up..."
    if [ ! -z "$SERVER_PID" ]; then
        kill $SERVER_PID 2>/dev/null || true
        wait $SERVER_PID 2>/dev/null || true
    fi
    echo "Done"
}

trap cleanup EXIT

echo "Step 2: Testing with websocat (if available)..."
if command -v websocat &> /dev/null; then
    echo '{"type":"init","document":{"name":"test.tex","content":"\\documentclass{article}\\begin{document}Hello!\\end{document}"}}' | \
        timeout 5 websocat ws://localhost:8181 2>&1 | head -20 || true
    echo
else
    echo "websocat not installed, skipping direct test"
    echo "Install with: brew install websocat (macOS)"
    echo
fi

echo "Step 3: Run manual test with test-client.js"
echo "You can now run in another terminal:"
echo "  node src/test-client.js ws://localhost:8181"
echo
echo "The server will continue running until you press Ctrl+C"
echo "=========================================="

# Wait for Ctrl+C
wait $SERVER_PID
