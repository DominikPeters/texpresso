#!/usr/bin/env python3
"""An external full build must not prevent idle completion of a later edit."""
import json
import os
from pathlib import Path
import selectors
import signal
import subprocess
import sys
import tempfile
import time

def source(pages):
    return '\\documentclass{article}\n\\begin{document}\n' + '\\newpage\n'.join(
        f'Page {n}.\\par\n' for n in range(1,pages+1)) + '\\end{document}\n'

engine = str(Path(sys.argv[1]).resolve())
artifacts = Path(os.environ.get('TEXPRESSO_TEST_ARTIFACTS','artifacts'))/'idle-rerun'
artifacts.mkdir(parents=True,exist_ok=True)
for automatic in (False,True):
    with tempfile.TemporaryDirectory(prefix='texpresso-idle-rerun-') as directory:
        folder = Path(directory); main = folder/'main.tex'; main.write_text(source(5))
        built = subprocess.run(['xelatex','-interaction=nonstopmode','-halt-on-error','main.tex'],
                               cwd=folder, capture_output=True, timeout=30)
        assert built.returncode == 0, built.stdout.decode(errors='replace')[-6000:]
        with (artifacts/('automatic.log' if automatic else 'lazy.log')).open('w+b') as log:
            p = subprocess.Popen([engine,'-texlive','-headless','-json','-lines','-stream',str(main)],
                                 cwd=folder,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=log,
                                 start_new_session=True,env={**os.environ,'SDL_VIDEODRIVER':'dummy'})
            state = None; pending = b''
            def send(*messages):
                for message in messages: p.stdin.write((json.dumps(message)+'\n').encode())
                p.stdin.flush()
            def pump(seconds, condition=lambda: False):
                global state,pending
                end = time.monotonic()+seconds
                with selectors.DefaultSelector() as selector:
                    selector.register(p.stdout,selectors.EVENT_READ)
                    while time.monotonic() < end and not condition():
                        if not selector.select(.05):
                            assert p.poll() is None, 'engine exited'
                            continue
                        data = os.read(p.stdout.fileno(),65536)
                        assert data, 'unexpected engine EOF'
                        pending += data
                        while b'\n' in pending:
                            line,pending = pending.split(b'\n',1)
                            msg = json.loads(line)
                            if msg[0] == 'preview-state': state = msg
                return condition()
            try:
                # Establish the old exact count explicitly, independently of
                # the idle behavior being tested, using preexisting disk .aux.
                send(['open',str(main),source(5)],['preview-barrier',1],['rerun',False],['resume'],['rerun-once'])
                assert pump(30,lambda: bool(state and state[2]==1 and state[3]==5 and state[4]))
                send(['pause'],['open',str(main),source(3)],['preview-barrier',2],['rerun',automatic])
                pump(1.2)
                assert not (state and state[2]==2 and state[3]==3 and state[4]), 'paused source pass advanced'
                send(['resume'])
                if not automatic:
                    pump(1.2)
                    assert not (state and state[2]==2 and state[4]), 'rerun=false must remain lazy'
                    send(['rerun',True])
                assert pump(30,lambda: bool(state and state[2]==2 and state[3]==3 and state[4])), state
                p.stdin.close(); p.stdin = None
                p.communicate(timeout=10); assert p.returncode == 0
            except Exception:
                print(Path(log.name).read_text(errors='replace')[-24000:],file=sys.stderr)
                raise
            finally:
                try: os.killpg(p.pid,signal.SIGKILL)
                except ProcessLookupError: pass
                p.wait(); p.stdout.close()
                if p.stdin and not p.stdin.closed: p.stdin.close()
print('PASS: preexisting auxiliary input, shrink 5 to 3, idle completion, pause and lazy-mode exclusions')
