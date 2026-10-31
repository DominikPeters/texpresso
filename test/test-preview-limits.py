#!/usr/bin/env python3
"""Capability and launch-budget validation; does not initialize a TeX worker."""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

engine = sys.argv[1]
for value in [None, '0', '-1', '16000001', '1.5', 'wrong', '9' * 100]:
    arguments = [engine, '-capabilities', '-preview-max-pixels'] + ([] if value is None else [value])
    result = subprocess.run(arguments, capture_output=True, text=True)
    assert result.returncode != 0, (value, result.stdout)
for value in ['1', '4000000', '8000000', '16000000']:
    result = subprocess.run([engine, '-preview-max-pixels', value, '-capabilities'], capture_output=True, text=True, check=True)
    assert json.loads(result.stdout)['maxPreviewPixels'] == 16000000
with tempfile.TemporaryDirectory() as directory:
    missing = Path(directory) / 'missing.tex'
    result = subprocess.run([engine, '-json', '-headless', str(missing)], capture_output=True, text=True)
    assert result.returncode == 1, (result.returncode, result.stderr)
    assert 'finding document path' in result.stderr and 'Aborting from' not in result.stderr
print('preview launch limits and startup failures: 12 cases passed')
