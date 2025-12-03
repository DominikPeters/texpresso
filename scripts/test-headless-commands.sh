#!/bin/bash
# Test script for TeXpresso headless mode file operations
# Tests: open, change, close commands

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}=== TeXpresso Headless Mode Test ===${NC}"
echo ""

# Build texpresso first
echo -e "${BLUE}Building texpresso...${NC}"
cd "$(dirname "$0")/.."
make clean > /dev/null 2>&1 || true
make -j4

# Test directory
TEST_DIR="test"
MAIN_FILE="$TEST_DIR/simple.tex"

# Start texpresso in headless mode
echo -e "${BLUE}Starting texpresso in headless mode...${NC}"
echo ""

# Create a named pipe for communication
PIPE=$(mktemp -u)
mkfifo "$PIPE"

# Start texpresso reading from the pipe
./build/texpresso --headless "$MAIN_FILE" < "$PIPE" > /tmp/texpresso-output.json 2> /tmp/texpresso-stderr.log &
TEXPRESSO_PID=$!

# Open the pipe for writing
exec 3>"$PIPE"

# Give it time to start
sleep 1

echo -e "${GREEN}TeXpresso started (PID: $TEXPRESSO_PID)${NC}"
echo ""

# Function to send command and wait
send_command() {
    local cmd="$1"
    local desc="$2"
    echo -e "${BLUE}Test: $desc${NC}"
    echo "  Command: $cmd"
    echo "$cmd" >&3
    sleep 0.5
    echo ""
}

# Wait for initial compilation
echo -e "${BLUE}Waiting for initial compilation...${NC}"
sleep 2

# Check initial output
if grep -q "doc.pageCount" /tmp/texpresso-output.json; then
    PAGE_COUNT=$(grep "doc.pageCount" /tmp/texpresso-output.json | head -1 | grep -o '"count":[0-9]*' | cut -d: -f2)
    echo -e "${GREEN}✓ Initial compilation successful: $PAGE_COUNT pages${NC}"
else
    echo -e "${RED}✗ Initial compilation failed${NC}"
    cat /tmp/texpresso-stderr.log
    kill $TEXPRESSO_PID 2>/dev/null || true
    rm -f "$PIPE"
    exit 1
fi
echo ""

# Test 1: Open a new file
echo -e "${BLUE}=== Test 1: Open new file ===${NC}"
send_command '["open", "test/newfile.tex", "\\documentclass{article}\\begin{document}Hello from new file!\\end{document}"]' \
    "Opening new file test/newfile.tex"

# Test 2: Change the main file (byte-based change)
echo -e "${BLUE}=== Test 2: Change main file (byte offset) ===${NC}"
# Replace "This is a very simple file" with "This is a MODIFIED simple file"
# The word "very" starts at offset ~71 (after \begin{document}\n\n  This is a )
send_command '["change", "test/simple.tex", 71, 4, "MODIFIED"]' \
    "Changing 'very' to 'MODIFIED' in main file"

# Wait for recompilation
sleep 2

# Check if recompilation happened
if tail -20 /tmp/texpresso-stderr.log | grep -q "changes detected"; then
    echo -e "${GREEN}✓ Recompilation triggered after change${NC}"
else
    echo -e "${RED}✗ Recompilation not detected${NC}"
fi
echo ""

# Test 3: Change using line-based change
echo -e "${BLUE}=== Test 3: Change using line offset ===${NC}"
# Change line 6 (the one with math symbols)
send_command '["change-lines", "test/simple.tex", 6, 1, "  symbols, $\\alpha, \\beta$ and $\\gamma$,  and some equations,"]' \
    "Replacing line 6 with different math symbols"

sleep 2

# Test 4: Change using range-based change
echo -e "${BLUE}=== Test 4: Change using range ===${NC}"
# Change range from line 7, char 0 to line 9, char 0 (the first equation)
send_command '["change-range", "test/simple.tex", 7, 0, 9, 0, " \\begin{equation}\n \\frac{2}{3} + \\frac{1}{3} = 1.\n"]' \
    "Replacing first equation using range-based change"

sleep 2

# Test 5: Rescan filesystem
echo -e "${BLUE}=== Test 5: Rescan filesystem ===${NC}"
send_command '["rescan"]' "Rescanning filesystem"

sleep 1

# Test 6: Close a file
echo -e "${BLUE}=== Test 6: Close file ===${NC}"
send_command '["close", "test/newfile.tex"]' "Closing test/newfile.tex"

sleep 1

# Close pipe and terminate texpresso
exec 3>&-
sleep 1
kill $TEXPRESSO_PID 2>/dev/null || true
wait $TEXPRESSO_PID 2>/dev/null || true
rm -f "$PIPE"

echo ""
echo -e "${BLUE}=== Test Results ===${NC}"
echo ""

# Count output lines
OUTPUT_LINES=$(wc -l < /tmp/texpresso-output.json)
echo "Total output lines: $OUTPUT_LINES"

# Count rendering commands
RENDER_CMDS=$(grep -c '"cmd":' /tmp/texpresso-output.json || true)
echo "Rendering commands: $RENDER_CMDS"

# Count page count updates
PAGE_UPDATES=$(grep -c 'doc.pageCount' /tmp/texpresso-output.json || true)
echo "Page count updates: $PAGE_UPDATES"

# Count errors
ERRORS=$(grep -c '"type":"error"' /tmp/texpresso-output.json || true)
echo "Errors: $ERRORS"

echo ""
if [ "$PAGE_UPDATES" -ge 2 ] && [ "$RENDER_CMDS" -gt 0 ]; then
    echo -e "${GREEN}✓✓✓ All tests passed! ✓✓✓${NC}"
    echo ""
    echo "Output saved to:"
    echo "  - JSON output: /tmp/texpresso-output.json"
    echo "  - Stderr log:  /tmp/texpresso-stderr.log"
    echo ""
    echo "Sample rendering commands:"
    grep '"cmd":' /tmp/texpresso-output.json | head -5
else
    echo -e "${RED}✗✗✗ Some tests failed ✗✗✗${NC}"
    echo ""
    echo "Check logs:"
    echo "  - /tmp/texpresso-output.json"
    echo "  - /tmp/texpresso-stderr.log"
    exit 1
fi
