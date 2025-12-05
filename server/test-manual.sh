#!/bin/bash

# Manual test for TeXpresso WebSocket server
# This script shows how to test the server with an actual TeX document

cd "$(dirname "$0")"

echo "=========================================="
echo "TeXpresso WebSocket Server - Manual Test"
echo "=========================================="
echo

# Check prerequisites
if [ ! -x "../build/texpresso" ]; then
    echo "ERROR: texpresso binary not found"
    exit 1
fi

if [ ! -f "../test/simple.tex" ]; then
    echo "ERROR: test/simple.tex not found"
    exit 1
fi

echo "This test will:"
echo "1. Start the WebSocket server on port 8080"
echo "2. Show you how to connect with the test client"
echo "3. Demonstrate file operations and compilation"
echo

read -p "Press Enter to start the server (Ctrl+C to cancel)..."

echo
echo "Starting server with verbose logging..."
echo

# Set environment variables for testing
export PORT=8080
export VERBOSE=true
export TEXPRESSO_PATH="../build/texpresso"

# Start the server
node src/index.js &
SERVER_PID=$!

cleanup() {
    echo
    echo "Stopping server (PID: $SERVER_PID)..."
    kill $SERVER_PID 2>/dev/null || true
    wait $SERVER_PID 2>/dev/null || true
    echo "Done"
}

trap cleanup EXIT

# Wait for server to start
sleep 2

echo
echo "=========================================="
echo "Server is running!"
echo "=========================================="
echo
echo "You can now test the server in another terminal:"
echo
echo "Option 1: Use the interactive test client"
echo "  cd server"
echo "  node src/test-client.js ws://localhost:8080"
echo
echo "Option 2: Use websocat (if installed)"
echo "  websocat ws://localhost:8080"
echo '  {"type":"init","document":{"name":"simple.tex","content":"\\\\documentclass{article}\\\\begin{document}Hello!\\\\end{document}"}}'
echo
echo "Option 3: Test with curl via the test client"
echo "  node src/test-client.js ws://localhost:8080 << EOF"
echo "  help"
echo "  change"
echo "  quit"
echo "  EOF"
echo
echo "=========================================="
echo "Press Ctrl+C to stop the server"
echo "=========================================="
echo

# Wait for Ctrl+C
wait $SERVER_PID
