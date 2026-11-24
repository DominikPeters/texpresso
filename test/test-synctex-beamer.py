#!/usr/bin/env python3
"""Check that an inverse SyncTeX hit in a fragile frame selects editable source."""
import json
import os
from pathlib import Path
import selectors
import signal
import subprocess
import sys
import tempfile
import time


SOURCE = r"""\documentclass{beamer}
\setbeamertemplate{navigation symbols}{}
\begin{document}
\begin{frame}[fragile]
\begin{verbatim}
UNIQUE FRAGILE LINE
\end{verbatim}
\end{frame}
\begin{frame}
Ordinary source
\end{frame}
\end{document}
"""


def check(engine, source):
    with tempfile.TemporaryFile() as log, selectors.DefaultSelector() as selector:
        process = subprocess.Popen(
            [engine, '-texlive', '-headless', '-json', '-lines', '-stream', str(source)],
            cwd=source.parent, env={**os.environ, 'SDL_VIDEODRIVER': 'dummy'},
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, start_new_session=True,
        )

        def send(command):
            process.stdin.write(json.dumps(command).encode() + b'\n')
            process.stdin.flush()

        try:
            send(['rerun', False])
            send(['resume'])
            selector.register(process.stdout, selectors.EVENT_READ)
            pending = b''
            generated_tag = None
            replies = {}
            requested = False
            deadline = time.monotonic() + 60
            while time.monotonic() < deadline and len(replies) < 2:
                if not selector.select(0.2):
                    continue
                data = os.read(process.stdout.fileno(), 65536)
                assert data, 'engine exited before returning inverse SyncTeX replies'
                pending += data
                while b'\n' in pending:
                    line, pending = pending.split(b'\n', 1)
                    message = json.loads(line)
                    if message[0] == 'input-file' and Path(message[2]).suffix == '.vrb':
                        generated_tag = message[1]
                    elif (message[0] == 'preview-state' and message[3] > 0
                          and generated_tag is not None and not requested):
                        requested = True
                        revision, sequence = message[1:3]
                        # In PDF points, inside UNIQUE FRAGILE LINE on page one.
                        send(['preview-inverse-precise', 1, revision, sequence, 0, 93, 116, None, True])
                        send(['preview-inverse-precise', 2, revision, sequence, 0, 93, 116, [generated_tag], True])
                    elif message[0] == 'preview-inverse-result':
                        replies[message[1]] = message[5]
            assert replies == {1: {'file': str(source), 'line': 8, 'column': -1, 'word': ''}, 2: None}, replies
            process.stdin.close()
            assert process.wait(timeout=5) == 0
        except Exception:
            log.seek(0)
            print(log.read().decode(errors='replace')[-8000:], file=sys.stderr)
            raise
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            process.stdin.close()
            process.stdout.close()


if __name__ == '__main__':
    with tempfile.TemporaryDirectory(prefix='texpresso-synctex-beamer-') as directory:
        source = Path(directory).resolve() / 'fragile.tex'
        source.write_text(SOURCE)
        check(Path(sys.argv[1]).resolve(), source)
    print('PASS: inverse SyncTeX selects the fragile frame in editable Beamer source')
