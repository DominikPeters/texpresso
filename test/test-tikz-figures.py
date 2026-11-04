#!/usr/bin/env python3
"""Real preview-only TikZ metadata, transformed bounds and unchanged page pixels."""
import json
import os
from pathlib import Path
import selectors
import signal
import subprocess
import sys
import tempfile
import time

engine = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='texpresso-tikz-') as temporary:
    root = Path(temporary)
    env = {**os.environ, 'SDL_VIDEODRIVER': 'dummy', 'XDG_CACHE_HOME': str(root/'cache'), 'SOURCE_DATE_EPOCH': '1'}
    source = root/'figures.tex'
    source.write_text(r'''\documentclass{article}
\usepackage{tikz,graphicx}
\newcommand{\badge}{\tikz{\fill (0,0) circle (2pt);}}
\begin{document}
Inline \badge.
\begin{tikzpicture}
\draw[line width=2pt] (0,0) rectangle (3,2);
\node at (1,1) {Nested \badge};
\end{tikzpicture}
\scalebox{0.5}{\begin{tikzpicture}\fill (0,0) rectangle (3,2);\end{tikzpicture}}
\end{document}
''')

    def render(path):
        with (root/'frontend.log').open('w+b') as log:
            p = subprocess.Popen([str(engine), '-texlive', '-headless', '-json', '-lines', '-stream', str(path)],
                cwd=root, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, start_new_session=True)
            try:
                p.stdin.write(b'["rerun",true]\n["resume"]\n'); p.stdin.flush()
                pending=b''; header=None; requested=set(); result=None
                deadline=time.monotonic()+90
                with selectors.DefaultSelector() as selector:
                    selector.register(p.stdout,selectors.EVENT_READ)
                    while time.monotonic()<deadline and result is None:
                        if not selector.select(.1):
                            assert p.poll() is None, 'frontend exited'
                            continue
                        data=os.read(p.stdout.fileno(),65536)
                        assert data, 'frontend EOF'
                        pending+=data
                        while True:
                            if header:
                                size=header['byteLength']
                                if len(pending)<size: break
                                body,pending=pending[:size],pending[size:]
                                result=(header,body); header=None; break
                            if b'\n' not in pending: break
                            line,pending=pending.split(b'\n',1); m=json.loads(line)
                            assert m[0] not in ('engine-error','preview-error'),m
                            if m[0]=='preview-image': header=m[1]
                            elif m[0]=='preview-state' and m[3]>0 and m[4]:
                                identity=(m[1],m[2])
                                if identity not in requested:
                                    requested.add(identity)
                                    p.stdin.write((json.dumps(['preview-render',len(requested),0,800,*identity])+'\n').encode());p.stdin.flush()
                assert result is not None, 'no rendered page'
                p.stdin.close(); assert p.wait(timeout=10)==0
                return result
            except Exception:
                log.seek(0); print(log.read().decode(errors='replace')[-12000:],file=sys.stderr); raise
            finally:
                try: os.killpg(p.pid,signal.SIGKILL)
                except ProcessLookupError: pass
                p.wait();p.stdout.close()
                if not p.stdin.closed:p.stdin.close()

    live,png=render(source)
    figures=live['annotations']['figures']
    assert len(figures)==3,figures  # nested badge does not become a fourth target
    assert [(f['line'],f['endLine']) for f in figures]==[(5,6),(6,10),(10,11)],figures
    assert all(f['file']=='figures.tex' for f in figures)
    outer,scaled=figures[1]['bounds'],figures[2]['bounds']
    assert abs((outer[2]-outer[0])-87.03)<.1,outer
    assert abs((scaled[2]-scaled[0])-42.52)<.1,scaled
    compiled=subprocess.run([str(engine.with_name('texpresso-xetex')),'-texlive',source.name],cwd=root,env=env,
                            capture_output=True,timeout=90)
    assert compiled.returncode==0,compiled.stdout.decode(errors='replace')[-6000:]
    ordinary,ordinary_png=render(source.with_suffix('.xdv'))
    assert not ordinary['annotations'].get('figures'),ordinary
    assert ordinary_png==png,'preview hook changed rendered pixels or layout'
print('PASS: real TikZ hook, nested suppression, source metadata, scaled geometry, identical ordinary-build PNG')
