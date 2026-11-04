#!/usr/bin/env python3
"""Actual MiKTeX: automatic package lookup, compiler workers, renderer fonts, warm cache."""
import json
import os
from pathlib import Path
import selectors
import signal
import subprocess
import sys
import tempfile
import time
import struct
import zlib
def assert_png_ink(body):
    offset = 8; compressed = b''; width = height = channels = None
    while offset < len(body):
        n = int.from_bytes(body[offset:offset+4], 'big')
        kind, data = body[offset+4:offset+8], body[offset+8:offset+8+n]
        if kind == b'IHDR':
            width, height, depth, color, _, _, interlace = struct.unpack('>IIBBBBB', data)
            assert depth == 8 and color in (0, 2, 4, 6) and interlace == 0, (depth,color,interlace)
            channels = {0:1, 2:3, 4:2, 6:4}[color]
        elif kind == b'IDAT': compressed += data
        offset += n+12
    raw = zlib.decompress(compressed); stride = width*channels
    assert len(raw) == height*(stride+1)
    previous = bytearray(stride); ink = 0
    for y in range(height):
        start = y*(stride+1); filtering = raw[start]
        row = bytearray(raw[start+1:start+1+stride])
        assert filtering in range(5)
        for x in range(stride):
            a = row[x-channels] if x >= channels else 0
            b = previous[x]; c = previous[x-channels] if x >= channels else 0
            predictor = 0
            if filtering == 1: predictor = a
            elif filtering == 2: predictor = b
            elif filtering == 3: predictor = (a+b)//2
            elif filtering == 4:
                estimate = a+b-c
                predictor = min((a,b,c), key=lambda value: abs(estimate-value))
            row[x] = (row[x]+predictor)&255
        ink += sum((channels not in (2,4) or row[x+channels-1] >= 128) and
                   (row[x] if channels in (1,2) else min(row[x:x+3])) < 160
                   for x in range(0,stride,channels))
        previous = row
    assert ink > 100, ('blank rendered page', ink)

engine = str(Path(sys.argv[1]).resolve())
provider = os.environ.get("TEXPRESSO_TEST_MIKTEX_PROVIDER", "miktex")
assert provider in ("miktex", "miktex-windows")
caps = json.loads(subprocess.check_output([engine, '-capabilities']))
assert 'miktex' in caps['packageProviders'] and caps['miktexAutoInstall']
with tempfile.TemporaryDirectory(prefix='texpresso-miktex-real-') as directory:
    folder = Path(directory)
    source = folder/'café main.tex'
    source.write_text(r'''\documentclass{article}
\usepackage{amsmath}
\begin{document}
MiKTeX live preview: caf\'e. $\int_0^1 x^2\,dx = \frac13$.
\end{document}
''')
    if provider == 'miktex-windows':
        source.write_text(source.read_text().replace(r'\usepackage{amsmath}', r'\usepackage{amsmath}'+ '\n'+r'\usepackage{csquotes}'))
    env = {**os.environ, 'XDG_CACHE_HOME': str(folder/'cache'), 'SDL_VIDEODRIVER': 'dummy'}
    diagnostics = Path(os.environ.get('TEXPRESSO_TEST_ARTIFACTS', 'artifacts'))/'miktex'
    diagnostics.mkdir(parents=True, exist_ok=True)
    format_stamp = None
    for automatic in (True, False):
        with (diagnostics/('cold.log' if automatic else 'warm.log')).open('w+b') as log:
            args = [engine, '-'+provider, '-headless', '-json', '-lines']
            if automatic: args.append('-miktex-auto-install')
            p = subprocess.Popen(args+[str(source)], cwd=folder, env=env, stdin=subprocess.PIPE,
                                 stdout=subprocess.PIPE, stderr=log, start_new_session=True)
            try:
                pending = b''; requested = set(); image_header = None; received = set(); rendered_snapshot = None
                deadline = time.monotonic()+600
                with selectors.DefaultSelector() as selector:
                    selector.register(p.stdout, selectors.EVENT_READ)
                    while time.monotonic() < deadline and len(received) < 2:
                        if not selector.select(.2):
                            with open(log.name, 'rb') as diagnostic:
                                diagnostic.seek(0, 2); length = diagnostic.tell(); diagnostic.seek(max(0,length-8192))
                                assert b'Compiler startup failed' not in diagnostic.read(), 'compiler startup failed'
                            if p.poll() is not None: break
                            continue
                        data = os.read(p.stdout.fileno(), 65536)
                        if not data: break
                        pending += data
                        while True:
                            if image_header:
                                n = image_header['byteLength']
                                if len(pending) < n: break
                                body, pending = pending[:n], pending[n:]
                                mime = image_header['mime']
                                if mime == 'image/png':
                                    assert body.startswith(b'\x89PNG'); assert_png_ink(body)
                                else:
                                    assert b'<svg' in body and body.count(b'<path') >= 5, 'missing SVG glyph paths'
                                rendered_snapshot = (image_header['revision'], image_header['sequence'])
                                received.add(mime); image_header = None
                            elif b'\n' in pending:
                                line, pending = pending.split(b'\n',1)
                                msg = json.loads(line)
                                if msg[0] == 'preview-image': image_header = msg[1]
                                elif msg[0] == 'preview-state' and msg[3] > 0:
                                    codec = 'preview-render' if not received else 'preview-svg'
                                    identity = (codec,msg[1],msg[2])
                                    if identity not in requested:
                                        requested.add(identity)
                                        p.stdin.write((json.dumps([codec,len(requested),0,800,msg[1],msg[2]])+'\n').encode()); p.stdin.flush()
                                elif msg[0] == 'preview-error': raise AssertionError(msg)
                            else: break
                            # Ask for SVG after the PNG, even if no new state arrives.
                            if received == {'image/png'} and not any(x[0]=='preview-svg' for x in requested):
                                revision, sequence = rendered_snapshot
                                requested.add(('preview-svg',revision,sequence))
                                p.stdin.write((json.dumps(['preview-svg',100,0,800,revision,sequence])+'\n').encode()); p.stdin.flush()
                assert received == {'image/png','image/svg+xml'}, (received,p.poll())
                p.stdin.close(); assert p.wait(timeout=10) == 0
                contents = Path(log.name).read_text(errors='replace')
                assert 'no font file nor VF file found' not in contents, 'missing renderer font'
                assert not any('Font mapping' in line and 'not found' in line for line in contents.splitlines()), 'missing TECkit mapping'
                formats = list((folder/'cache/texpresso/format').glob(provider+'-*.fmt'))
                assert len(formats) == 1
                stamp = formats[0].stat().st_mtime_ns
                if not automatic: assert stamp == format_stamp, 'warm launch rebuilt its format'
                format_stamp = stamp
            except Exception:
                log.seek(0); print(log.read().decode(errors='replace')[-16000:], file=sys.stderr); raise
            finally:
                try: os.killpg(p.pid,signal.SIGKILL)
                except ProcessLookupError: pass
                p.wait(); p.stdout.close()
                if not p.stdin.closed: p.stdin.close()
    assert list((folder/'cache/texpresso/format').glob(provider+'-*.fmt'))
    assert not list((folder/'cache/texpresso/format').glob('texlive-*.fmt'))
print('PASS: real MiKTeX compilation, package installation, PNG/SVG font rendering, warm offline-policy restart')
