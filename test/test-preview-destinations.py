#!/usr/bin/env python3
"""Resolve links before any DVI output exists, using a disposable public document."""
import json
import os
from pathlib import Path
import selectors
import signal
import subprocess
import sys
import tempfile
import time


def check(engine, destination, expected):
    with tempfile.TemporaryDirectory(prefix='texpresso-destination-') as temp:
        directory = Path(temp)
        source = directory / 'main.tex'
        source.write_text(r'''\documentclass{article}
\usepackage[xetex]{hyperref}
\begin{document}
\section{First}First page.
\newpage
\section{Second}Second page.
\end{document}
''')
        with (directory / 'engine.log').open('w+b') as log:
            process = subprocess.Popen([str(engine), '-texlive', '-headless', '-json', '-lines', '-stream', str(source)],
                                       cwd=directory, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                       stderr=log, start_new_session=True)
            try:
                # -stream starts paused. Queue the destination before the first
                # compiler step: there is no output filecell to dereference yet.
                for command in (['preview-destination', 1, 0, destination], ['resume']):
                    process.stdin.write(json.dumps(command).encode() + b'\n')
                process.stdin.flush()
                pending = b''
                found = False
                deadline = time.monotonic() + 30
                with selectors.DefaultSelector() as selector:
                    selector.register(process.stdout, selectors.EVENT_READ)
                    while not found and time.monotonic() < deadline:
                        if not selector.select(timeout=.1):
                            if process.poll() is not None:
                                break
                            continue
                        data = os.read(process.stdout.fileno(), 65536)
                        if not data:
                            break
                        pending += data
                        while b'\n' in pending:
                            line, pending = pending.split(b'\n', 1)
                            message = json.loads(line)
                            if message[0] == 'preview-destination':
                                assert message == ['preview-destination', 1, 0, destination, expected], message
                                found = True
                if not found:
                    log.seek(0)
                    raise AssertionError(f'No destination reply; exit={process.poll()}\n{log.read().decode(errors="replace")[-8000:]}')
                process.stdin.close()
                assert process.wait(timeout=5) == 0
            finally:
                # Include worker/checkpoint descendants if the test times out.
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.wait()
                process.stdout.close()
                if not process.stdin.closed:
                    process.stdin.close()
    print(f'PASS: destination before output: {destination} -> {expected}')


if __name__ == '__main__':
    engine = Path(sys.argv[1]).resolve()
    check(engine, 'section.2', 1)
    check(engine, 'missing', -1)
