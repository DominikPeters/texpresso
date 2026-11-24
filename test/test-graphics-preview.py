#!/usr/bin/env python3
"""Check XeTeX preview clipping and provider image lookup in an isolated project."""
from io import BytesIO
import json
import os
from pathlib import Path
import selectors
import signal
import subprocess
import sys
import tempfile
import time

from PIL import Image, ImageDraw


def render(engine, provider, source, env):
    with tempfile.TemporaryFile() as log, selectors.DefaultSelector() as selector:
        child = subprocess.Popen(
            [str(engine), '-' + provider, '-headless', '-json', '-lines', '-stream', source.name],
            cwd=source.parent, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=log, start_new_session=True,
        )
        pending = b''
        size = None
        requested = set()
        try:
            child.stdin.write(b'["resume"]\n')
            child.stdin.flush()
            selector.register(child.stdout, selectors.EVENT_READ)
            deadline = time.monotonic() + 90
            while time.monotonic() < deadline:
                if not selector.select(0.2):
                    continue
                data = os.read(child.stdout.fileno(), 65536)
                assert data, 'engine exited before rendering'
                pending += data
                while True:
                    if size is not None:
                        if len(pending) < size:
                            break
                        return Image.open(BytesIO(pending[:size])).convert('RGB')
                    if b'\n' not in pending:
                        break
                    line, pending = pending.split(b'\n', 1)
                    message = json.loads(line)
                    assert message[0] not in ('preview-error', 'engine-error'), message
                    if message[0] == 'preview-image':
                        size = message[1]['byteLength']
                    elif message[0] == 'preview-state' and message[3] > 0:
                        identity = tuple(message[1:3])
                        if identity not in requested:
                            requested.add(identity)
                            command = ['preview-render', len(requested), 0, 612, *identity]
                            child.stdin.write((json.dumps(command) + '\n').encode())
                            child.stdin.flush()
            raise TimeoutError('preview did not render')
        except Exception:
            log.seek(0)
            print(log.read().decode(errors='replace')[-12000:], file=sys.stderr)
            raise
        finally:
            child.stdin.close()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(child.pid, signal.SIGKILL)
                child.wait()
            child.stdout.close()


def main(engine, provider):
    with tempfile.TemporaryDirectory(prefix='texpresso-graphics-') as directory:
        root = Path(directory)
        project = root / 'project'
        project.mkdir()
        env = {**os.environ, 'SDL_VIDEODRIVER': 'dummy'}
        picture = Image.new('RGB', (144, 72), 'red')
        ImageDraw.Draw(picture).rectangle((72, 0, 143, 71), fill='blue')
        picture.save(project / 'txp-graphics-colors.pdf', resolution=72)
        picture.save(project / 'txp-graphics-colors.png', dpi=(72, 72))
        cases = {
            'adjustbox': r'\clipbox{72bp 0bp 0bp 0bp}{\textcolor{red}{\rule{72bp}{72bp}}\textcolor{blue}{\rule{72bp}{72bp}}}',
            'pdf': r'\includegraphics[trim=72bp 0bp 0bp 0bp,clip]{txp-graphics-colors.pdf}',
            'png': r'\includegraphics[trim=72bp 0bp 0bp 0bp,clip]{txp-graphics-colors.png}',
        }
        if provider == 'texlive':
            cases['tree-pdf'] = r'\includegraphics[width=120pt]{example-image.pdf}'
            cases['tree-png'] = r'\includegraphics[width=120pt]{example-image.png}'
        for name, body in cases.items():
            source = project / (name + '.tex')
            source.write_text(r'''\documentclass{article}
\usepackage{graphicx,adjustbox,xcolor}
\pagestyle{empty}
\begin{document}
''' + body + '\n' + r'\end{document}' + '\n')
            image = render(engine, provider, source, env)
            if name.startswith('tree-'):
                ink = sum(min(rgb) < 245 for rgb in image.getdata())
                assert ink > 8000, (provider, name, ink)
                print(f'{provider} {name}: TeX-tree image rendered ({ink} ink pixels)')
                continue
            red = sum(r > 175 and g < 55 and b < 55 for r, g, b in image.getdata())
            blue = sum(b > 175 and r < 55 and g < 55 for r, g, b in image.getdata())
            assert red < 50 and 4000 < blue < 6000, (provider, name, red, blue)
            print(f'{provider} {name}: expected crop ({red} red, {blue} blue pixels)')


if __name__ == '__main__':
    main(Path(sys.argv[1]).resolve(), sys.argv[2])
