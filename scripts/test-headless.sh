#!/bin/bash
# Simple test for TeXpresso headless mode
# Sends a few commands and checks output

cd "$(dirname "$0")/.."

echo "=== Simple Headless Mode Test ==="
echo ""

# Build if needed
if [ ! -f build/texpresso ]; then
    echo "Building texpresso..."
    make -j4 || exit 1
    echo ""
fi

# Create test input
cat > /tmp/texpresso-test-input.json << 'INPUT'
["change", "test/simple.tex", 71, 4, "MODIFIED"]
INPUT

echo "Starting texpresso in headless mode..."
echo "Input command: $(cat /tmp/texpresso-test-input.json)"
echo ""

# Run texpresso with timeout
timeout 5s ./build/texpresso --headless test/simple.tex < /tmp/texpresso-test-input.json > /tmp/texpresso-test-output.json 2> /tmp/texpresso-test-stderr.log || true

echo "=== Output ==="
echo ""
echo "Stderr log:"
tail -20 /tmp/texpresso-test-stderr.log
echo ""
echo "JSON output lines: $(wc -l < /tmp/texpresso-test-output.json)"
echo "Page count updates: $(grep -c 'doc.pageCount' /tmp/texpresso-test-output.json || echo 0)"
echo "Rendering commands: $(grep -c '"cmd":' /tmp/texpresso-test-output.json || echo 0)"
echo ""

# Show first few lines of output
echo "First 10 lines of JSON output:"
head -10 /tmp/texpresso-test-output.json

# Cleanup
rm -f /tmp/texpresso-test-input.json
