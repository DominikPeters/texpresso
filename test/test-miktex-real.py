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
engine = str(Path(sys.argv[1]).resolve())
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
    env = {**os.environ, 'XDG_CACHE_HOME': str(folder/'cache'), 'SDL_VIDEODRIVER': 'dummy'}
    for automatic in (True, False):
        with (folder/'engine.log').open('w+b') as log:
            args = [engine, '-miktex', '-headless', '-json', '-lines']
            if automatic: args.append('-miktex-auto-install')
            p = subprocess.Popen(args+[str(source)], cwd=folder, env=env, stdin=subprocess.PIPE,
                                 stdout=subprocess.PIPE, stderr=log, start_new_session=True)
            try:
                pending = b''; requested = set(); image_header = None; received = set()
                deadline = time.monotonic()+600
                with selectors.DefaultSelector() as selector:
                    selector.register(p.stdout, selectors.EVENT_READ)
                    while time.monotonic() < deadline and len(received) < 2:
                        if not selector.select(.2):
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
                                assert body.startswith(b'\x89PNG') if mime == 'image/png' else b'<svg' in body
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
                                identity = next(iter(requested)); requested.add(('preview-svg',identity[1],identity[2]))
                                p.stdin.write((json.dumps(['preview-svg',100,0,800,identity[1],identity[2]])+'\n').encode()); p.stdin.flush()
                assert received == {'image/png','image/svg+xml'}, (received,p.poll())
                p.stdin.close(); assert p.wait(timeout=10) == 0
            except Exception:
                log.seek(0); print(log.read().decode(errors='replace')[-16000:], file=sys.stderr); raise
            finally:
                try: os.killpg(p.pid,signal.SIGKILL)
                except ProcessLookupError: pass
                p.wait(); p.stdout.close()
                if not p.stdin.closed: p.stdin.close()
    assert list((folder/'cache/texpresso/format').glob('miktex-*.fmt'))
    assert not list((folder/'cache/texpresso/format').glob('texlive-*.fmt'))
print('PASS: real MiKTeX compilation, package installation, PNG/SVG font rendering, warm offline-policy restart')
