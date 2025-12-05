#!/bin/bash

# Start Web TeXpresso
# This script starts both the WebSocket server and HTTP client

cd "$(dirname "$0")"

echo "=========================================="
echo "Starting Web TeXpresso"
echo "=========================================="
echo

# Check if texpresso binary exists
if [ ! -x "build/texpresso" ]; then
    echo "ERROR: texpresso binary not found"
    echo "Please build texpresso first:"
    echo "  make clean && make"
    exit 1
fi

# Kill any processes using the ports
echo "Checking for processes on ports 8080 and 3000..."
WS_PID=$(lsof -ti:8080)
HTTP_PID=$(lsof -ti:3000)

if [ -n "$WS_PID" ]; then
    echo "Killing process $WS_PID on port 8080"
    kill -9 $WS_PID 2>/dev/null || true
    sleep 1
fi

if [ -n "$HTTP_PID" ]; then
    echo "Killing process $HTTP_PID on port 3000"
    kill -9 $HTTP_PID 2>/dev/null || true
    sleep 1
fi

echo "Step 1: Starting WebSocket server..."
PROJECT_ROOT="$(pwd)"
cd server
PORT=8080 \
TEXPRESSO_PATH="$PROJECT_ROOT/build/texpresso" \
WORKING_DIR="$PROJECT_ROOT" \
npm start &
SERVER_PID=$!
cd ..

sleep 2

echo "Step 2: Starting HTTP client server..."
cd client
PORT=3000 npm start &
CLIENT_PID=$!
cd ..

sleep 2

echo
echo "=========================================="
echo "Web TeXpresso is running!"
echo "=========================================="
echo
echo "WebSocket Server: ws://localhost:8080"
echo "Web Client:       http://localhost:3000"
echo
echo "Open your browser to:"
echo "  http://localhost:3000"
echo
echo "Press Ctrl+C to stop all servers"
echo "=========================================="

# Cleanup function
cleanup() {
    echo
    echo "Stopping servers..."
    kill $SERVER_PID $CLIENT_PID 2>/dev/null || true
    echo "Done"
    exit 0
}

trap cleanup INT TERM

# Wait for Ctrl+C
wait
