#!/usr/bin/env python3
"""Check profiling records under concurrent compiler/frontend stderr writes."""
import json
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).with_suffix('.c')
with tempfile.TemporaryDirectory(prefix='txp-profile-') as temporary:
    executable = str(Path(temporary) / 'test-profile')
    subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra',
                    '-Wno-unused-function', str(source), '-o', executable], check=True)
    result = subprocess.run([executable], check=True, capture_output=True, timeout=15)
    records = set()
    markers = 0
    for line in result.stderr.decode().splitlines():
        if line == 'WORKER-MARKER':
            markers += 1
            continue
        assert line.startswith('[profile] '), line
        record = json.loads(line[len('[profile] '):])
        assert record['padding'] == 'x' * 300
        key = (record['worker'], record['record'])
        assert key not in records, key
        records.add(key)
    assert records == {(worker, record) for worker in range(4) for record in range(1000)}
    assert markers == 1000, markers
print('PASS: 4000 concurrent profile records and 1000 worker markers remain intact')
