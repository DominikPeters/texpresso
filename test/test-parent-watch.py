#!/usr/bin/env python3
"""Watch real session ownership changes using disposable process groups only."""
import json
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
import time


def wait_gone(group, child=None):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if child is not None:
            child.poll()  # Reap our direct child, if it has exited.
        try:
            os.killpg(group, 0)
        except ProcessLookupError:
            return
        time.sleep(.02)
    raise AssertionError(f'Disposable process group {group} survived')


source = r'''
#include "parent_watch.h"
int main(void) {
    const int result = txp_watch_parent();
    if (result >= 0) return result;
    const pid_t child = fork();
    if (child < 0) return 2;
    if (!child) { for (;;) pause(); }
    printf("{\"frontend\":%d,\"checkpoint\":%d}\n", getpid(), child);
    fflush(stdout);
    char byte;
    while (read(STDIN_FILENO, &byte, 1) > 0) {}
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='texpresso-parent-watch-') as directory:
    folder = Path(directory)
    code = folder / 'fixture.c'; code.write_text(source)
    fixture = folder / 'fixture'
    include = Path(__file__).resolve().parents[1] / 'src/frontend'
    subprocess.run([os.environ.get('CC', 'cc'), '-Wall', '-Wextra', '-Werror', '-I', str(include), str(code), '-o', str(fixture)], check=True)
    # Refuse to target an ordinary shell/test runner process group.
    bad = subprocess.run([str(fixture)], capture_output=True, timeout=5)
    assert bad.returncode == 1 and b'isolated process group' in bad.stderr

    for mode in ('frontend-crash', 'frontend-eof', 'owner-crash', 'watcher-term', 'watcher-int', 'watcher-hup', 'owner-crash-closed-stderr'):
        log = (folder / f'{mode}.log').open('wb')
        owner = None
        if mode.startswith('owner-crash'):
            marker = folder / (mode + '.json')
            script = '''import json,subprocess,sys,time
p = subprocess.Popen([sys.argv[1]], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.PIPE if sys.argv[3] == 'closed-stderr' else None,
                     start_new_session=True)
line = p.stdout.readline()
with open(sys.argv[2], 'w') as f: json.dump({'group':p.pid, 'workers':json.loads(line)},f)
time.sleep(30)
'''
            owner = subprocess.Popen([sys.executable, '-c', script, str(fixture), str(marker),
                                      'closed-stderr' if mode.endswith('closed-stderr') else 'file-stderr'], stderr=log)
            deadline = time.monotonic() + 5
            while not marker.exists() and time.monotonic() < deadline:
                time.sleep(.02)
            assert marker.exists(), 'Owner fixture did not start'
            info = json.loads(marker.read_text()); group = info['group']
            owner.kill(); owner.wait(timeout=5)
            child = None
        else:
            child = subprocess.Popen([str(fixture)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, start_new_session=True)
            group = child.pid
            assert select.select([child.stdout], [], [], 5)[0], 'Frontend fixture did not start'
            workers = json.loads(child.stdout.readline())
            if mode == 'frontend-crash':
                os.kill(workers['frontend'], signal.SIGKILL)
            elif mode.startswith('watcher-'):
                os.kill(group, {'watcher-term': signal.SIGTERM, 'watcher-int': signal.SIGINT,
                                'watcher-hup': signal.SIGHUP}[mode])
            else:
                child.stdin.close()
        try:
            wait_gone(group, child)
        finally:
            # Only groups created by the fixture above are eligible for cleanup.
            try:
                os.killpg(group, signal.SIGKILL)
            except ProcessLookupError:
                pass
            if child is not None:
                child.wait(timeout=5)
            log.close()
        print(f'parent watcher: {mode}: no surviving workers')
