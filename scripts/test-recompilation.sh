#!/bin/bash
# Test that recompilation occurs after change-range commands
# This is the key test to verify incremental updates work

set -e

cd "$(dirname "$0")/../test"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m'

echo "=== TeXpresso Recompilation Test ==="
echo "Testing that change-range commands trigger recompilation"
echo ""

# Ensure build
if [ ! -f ../build/texpresso ]; then
    echo "Building texpresso..."
    (cd .. && make -j4) || exit 1
fi

# Create a named pipe for input
PIPE=$(mktemp -u)
mkfifo "$PIPE"

# Output files
OUTPUT_JSON=/tmp/texpresso-recompile-output.json
OUTPUT_STDERR=/tmp/texpresso-recompile-stderr.log

# Cleanup function
cleanup() {
    if [ ! -z "$TEXPRESSO_PID" ]; then
        kill $TEXPRESSO_PID 2>/dev/null || true
        wait $TEXPRESSO_PID 2>/dev/null || true
    fi
    exec 3>&- 2>/dev/null || true
    rm -f "$PIPE"
}
trap cleanup EXIT

echo "[1] Starting texpresso in headless mode..."
../build/texpresso --headless simple.tex < "$PIPE" > "$OUTPUT_JSON" 2> "$OUTPUT_STDERR" &
TEXPRESSO_PID=$!

# Open the pipe for writing
exec 3>"$PIPE"

# Send the open command to put file content in VFS
# This is required before change-range commands will work
echo "[1.5] Sending open command to initialize file in VFS..."

# Read the actual file content
FILE_CONTENT=$(cat simple.tex)

# Escape special characters for JSON
ESCAPED_CONTENT=$(printf '%s' "$FILE_CONTENT" | python3 -c 'import json,sys; print(json.dumps(sys.stdin.read()))')

# Send open command - use absolute path since relative_path() requires it
ABSOLUTE_PATH="$(pwd)/simple.tex"
echo '["open", "'"$ABSOLUTE_PATH"'", '"$ESCAPED_CONTENT"']' >&3

# Wait for initial compilation
sleep 3

# Check initial compilation
INITIAL_PAGE_COUNT=$(grep -c '"type":"doc.pageCount"' "$OUTPUT_JSON" 2>/dev/null || echo 0)
echo "    Initial page count messages: $INITIAL_PAGE_COUNT"

if [ "$INITIAL_PAGE_COUNT" -eq 0 ]; then
    echo -e "${RED}✗ Initial compilation failed - no page count message${NC}"
    echo "Stderr:"
    cat "$OUTPUT_STDERR"
    exit 1
fi

echo -e "${GREEN}✓ Initial compilation successful${NC}"
echo ""

# Get line count of output after initial compilation
INITIAL_OUTPUT_LINES=$(wc -l < "$OUTPUT_JSON")
echo "[2] Output lines after initial compilation: $INITIAL_OUTPUT_LINES"

# The content of test/simple.tex at line 5 (0-indexed) is:
#   "  This is a very simple file, though it does include some mathematical"
# We'll change "very simple" to "MODIFIED" using change-range

echo ""
echo "[3] Sending change-range command..."
echo '    Change: "very simple" -> "MODIFIED" at line 4, chars 14-25'
echo ""

# change-range format: ["change-range", path, start-line, start-col, end-line, end-col, replacement]
# Line 4 (0-indexed) = "  This is a very simple file..."
# "very simple" starts at column 14 (0-indexed, counting UTF-16 units) and ends at column 25
CMD='["change-range", "'"$ABSOLUTE_PATH"'", 4, 14, 4, 25, "MODIFIED"]'
echo "    Command: $CMD"
echo "$CMD" >&3

# Wait for recompilation
sleep 3

# Check stderr for recompilation
echo ""
echo "[4] Checking for recompilation..."

if grep -q "changes detected" "$OUTPUT_STDERR"; then
    echo -e "${GREEN}✓ Recompilation was triggered ('changes detected' found in stderr)${NC}"
else
    echo -e "${YELLOW}⚠ 'changes detected' not found in stderr${NC}"
    echo "   This might be a problem - checking other indicators..."
fi

# Check if new output was generated
FINAL_OUTPUT_LINES=$(wc -l < "$OUTPUT_JSON")
NEW_OUTPUT=$((FINAL_OUTPUT_LINES - INITIAL_OUTPUT_LINES))
echo ""
echo "[5] Output lines after change: $FINAL_OUTPUT_LINES (new: $NEW_OUTPUT)"

FINAL_PAGE_COUNT=$(grep -c '"type":"doc.pageCount"' "$OUTPUT_JSON" 2>/dev/null || echo 0)
echo "    Page count messages (should be > 1 after recompilation): $FINAL_PAGE_COUNT"

# Count rendering commands
RENDER_CMDS=$(grep -c '"cmd":' "$OUTPUT_JSON" 2>/dev/null || echo 0)
echo "    Total rendering commands: $RENDER_CMDS"

echo ""
echo "=== Results ==="

# Test 1: More page count messages (indicates recompilation)
if [ "$FINAL_PAGE_COUNT" -gt 1 ]; then
    echo -e "${GREEN}✓ TEST 1 PASSED: Multiple page count messages ($FINAL_PAGE_COUNT) indicate recompilation${NC}"
    TEST1_PASSED=1
else
    echo -e "${RED}✗ TEST 1 FAILED: Only $FINAL_PAGE_COUNT page count message${NC}"
    TEST1_PASSED=0
fi

# Test 2: New output was generated
if [ "$NEW_OUTPUT" -gt 0 ]; then
    echo -e "${GREEN}✓ TEST 2 PASSED: New output generated after change ($NEW_OUTPUT lines)${NC}"
    TEST2_PASSED=1
else
    echo -e "${RED}✗ TEST 2 FAILED: No new output after change${NC}"
    TEST2_PASSED=0
fi

# Test 3: Rendering commands present
if [ "$RENDER_CMDS" -gt 0 ]; then
    echo -e "${GREEN}✓ TEST 3 PASSED: Rendering commands present ($RENDER_CMDS)${NC}"
    TEST3_PASSED=1
else
    echo -e "${RED}✗ TEST 3 FAILED: No rendering commands${NC}"
    TEST3_PASSED=0
fi

echo ""
echo "=== Debugging Info ==="
echo "Stderr log (last 20 lines):"
tail -20 "$OUTPUT_STDERR"
echo ""
echo "Last 10 JSON output lines:"
tail -10 "$OUTPUT_JSON"

echo ""
if [ "$TEST1_PASSED" -eq 1 ] && [ "$TEST2_PASSED" -eq 1 ]; then
    echo -e "${GREEN}✓✓✓ RECOMPILATION TEST PASSED ✓✓✓${NC}"
    exit 0
else
    echo -e "${RED}✗✗✗ RECOMPILATION TEST FAILED ✗✗✗${NC}"
    echo ""
    echo "Full stderr log saved to: $OUTPUT_STDERR"
    echo "Full JSON output saved to: $OUTPUT_JSON"
    exit 1
fi
