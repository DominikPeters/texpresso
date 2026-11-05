#!/usr/bin/env python3
"""Exercise production checkpoint sockets after EOF, rollback and frontend death."""
import array
import ctypes
import json
import os
from pathlib import Path
import select
import selectors
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time


SOURCE = r'''
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include "txp_activity.h"
extern pid_t texpresso_fork_with_channel(int, uint32_t, txp_activity *, uint64_t);
int main(int argc, char **argv) {
    assert(argc == 2);
    const int fd = atoi(argv[1]);
    /* Production workers ignore TERM while parked; socket ownership must be
     * sufficient to release them without a cooperative signal handler. */
    signal(SIGTERM, SIG_IGN);
    for (int depth = 0; depth < 8; ++depth) {
        if (texpresso_fork_with_channel(fd, depth, NULL, 0) > 0) {
            assert(write(fd, "BACK", 4) == 4);
            goto idle;
        }
    }
    assert(write(fd, "LEAF", 4) == 4);
idle:
    for (;;) {
        char byte;
        const ssize_t n = read(fd, &byte, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return n == 0 ? 0 : 1;
        assert(write(fd, &byte, 1) == 1);
    }
}
'''


def exact(peer, count):
    data = b''
    while len(data) < count:
        part = peer.recv(count - len(data))
        assert part, 'unexpected fixture EOF'
        data += part
    return data


def owner(fixture):
    """A real frontend owns every server endpoint; workers inherit only theirs."""
    server, worker = socket.socketpair()
    process = subprocess.Popen([fixture, str(worker.fileno())], pass_fds=(worker.fileno(),))
    worker.close()
    peers, workers = [server], [process.pid]
    for _ in range(8):
        data, controls, flags, _ = peers[-1].recvmsg(12, socket.CMSG_SPACE(4))
        data += exact(peers[-1], 12 - len(data))
        assert data[:4] == b'CHLD' and not flags, (data, flags)
        fds = array.array('i')
        for level, kind, payload in controls:
            assert level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS
            fds.frombytes(payload)
        assert len(fds) == 1
        peers.append(socket.socket(fileno=fds[0]))
        workers.append(struct.unpack('=i', data[8:12])[0])
    assert exact(peers[-1], 4) == b'LEAF'
    print(json.dumps({'group': os.getpgrp(), 'workers': workers}), flush=True)
    for command in sys.stdin:
        command = command.strip()
        if command == 'ping':
            peers[-1].sendall(b'P')
            assert exact(peers[-1], 1) == b'P'
            print('pong', flush=True)
        elif command == 'rewind':
            peers.pop().close()
            peers[-1].sendall(b'DONE')
            assert exact(peers[-1], 4) == b'BACK'
            print('resumed', flush=True)
        elif command == 'close':
            for peer in reversed(peers):
                peer.close()
            assert process.wait(timeout=4) == 0
            print('closed', flush=True)
            return
        else:
            raise AssertionError(command)


def line(process, timeout=5):
    assert select.select([process.stdout], [], [], timeout)[0], 'fixture did not respond'
    result = process.stdout.readline().strip()
    assert result, 'fixture exited without a response'
    return result


def wait_workers_gone(pids):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        alive = []
        for pid in pids:
            try:
                os.waitpid(pid, os.WNOHANG)  # Linux subreaper; macOS launchd reaps orphans.
            except ChildProcessError:
                pass
            try:
                os.kill(pid, 0)
                alive.append(pid)
            except ProcessLookupError:
                pass
        if not alive:
            return
        time.sleep(.02)
    raise AssertionError(f'checkpoint workers survived: {alive}')


def main():
    if sys.platform.startswith('linux'):
        # Reap only the known fixture PIDs after their frontend is killed, even
        # in containers whose PID 1 does not reap adopted children promptly.
        assert ctypes.CDLL(None, use_errno=True).prctl(36, 1, 0, 0, 0) == 0
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='texpresso-checkpoint-cleanup-') as directory:
        folder = Path(directory)
        code, fixture = folder / 'worker.c', folder / 'worker'
        code.write_text(SOURCE)
        subprocess.run([os.environ.get('CC', 'cc'), '-Wall', '-Wextra', '-Werror',
                        '-I', str(root / 'src/include'), str(code),
                        str(root / 'src/engine/main/fork.c'), '-o', str(fixture)], check=True)
        launched = []

        def start(name):
            log = (folder / (name + '.log')).open('wb')
            p = subprocess.Popen([sys.executable, __file__, '--owner', str(fixture)],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log,
                                 text=True, start_new_session=True)
            record = (p, log, folder / (name + '.log'), [])
            launched.append(record)
            info = json.loads(line(p))
            assert info['group'] == p.pid and len(info['workers']) == 9
            record[3].extend(info['workers'])
            return p, info['workers']

        def command(p, text, expected):
            p.stdin.write(text + '\n'); p.stdin.flush()
            assert line(p) == expected

        try:
            # A separate live session must remain functional through every
            # shutdown, including while the killed tree is reparented.
            sentinel, sentinel_pids = start('unrelated-session')
            for mode in ('channel-eof', 'frontend-sigkill', 'rewind-then-sigkill'):
                p, pids = start(mode)
                if mode == 'rewind-then-sigkill':
                    command(p, 'rewind', 'resumed')
                    command(p, 'ping', 'pong')
                if mode == 'channel-eof':
                    command(p, 'close', 'closed')
                    assert p.wait(timeout=5) == 0
                else:
                    p.kill(); p.wait(timeout=5)
                wait_workers_gone(pids)
                command(sentinel, 'ping', 'pong')
                print(f'checkpoint cleanup: {mode}: all 9 workers exited; unrelated session works', flush=True)
            command(sentinel, 'close', 'closed')
            assert sentinel.wait(timeout=5) == 0
            wait_workers_gone(sentinel_pids)
        finally:
            # Only isolated groups created by this test are eligible. No scan
            # or signal by executable name, document path or shared app folder.
            for p, log, path, pids in launched:
                try:
                    os.killpg(p.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                p.wait(timeout=5)
                p.stdin.close(); p.stdout.close(); log.close()
                wait_workers_gone(pids)
                if p.returncode not in (0, -signal.SIGKILL):
                    print(path.read_text(), file=sys.stderr)


def group_members(group):
    return [int(row[0]) for row in (line.split() for line in subprocess.check_output(
        ['ps', '-axo', 'pid=,pgid='], text=True).splitlines()) if int(row[1]) == group]


def real_engine(engine):
    """Optional integration check: real XeLaTeX checkpoints, no test worker stub."""
    with tempfile.TemporaryDirectory(prefix='texpresso-real-checkpoint-cleanup-') as directory:
        folder = Path(directory)
        source = folder / 'main.tex'
        # Keep actual source reads and token progress after the first shipout:
        # macOS intentionally postpones its first snapshot until fonts load.
        source.write_text('\\documentclass{article}\n\\usepackage[xetex]{hyperref}\n\\begin{document}\n' + '\\newpage\n'.join(
            f'\\section{{Page {i}}}\\input{{page-{i}}}\n' for i in range(1, 13)) + '\\end{document}\n')
        for i in range(1, 13):
            (folder / f'page-{i}.tex').write_text(f'Page {i}.\\par\n' + '\\relax\n' * 800)
        env = {**os.environ, 'SDL_VIDEODRIVER': 'dummy', 'XDG_CACHE_HOME': str(folder / 'cache')}
        sentinel = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(120)'], start_new_session=True)
        try:
            for mode in ('eof', 'frontend-sigkill', 'supervisor-term', 'group-term'):
                with (folder / (mode + '.log')).open('w+b') as log:
                    args = [str(engine), '-texlive', '-headless', '-json', '-lines', '-stream']
                    if mode in ('supervisor-term', 'group-term'):
                        args.append('-watch-parent')
                    p = subprocess.Popen([*args, str(source)], cwd=folder, env=env,
                                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log,
                                         start_new_session=True)
                    try:
                        # Stop at a real demand in the middle of the document.
                        # Reaching EOF can discard the retained tree during aux
                        # convergence, leaving no parked ancestors to exercise.
                        p.stdin.write(b'["rerun",false]\n["preview-destination",1,0,"section.6"]\n["resume"]\n'); p.stdin.flush()
                        pending, ready = b'', False
                        deadline = time.monotonic() + 90
                        with selectors.DefaultSelector() as events:
                            events.register(p.stdout, selectors.EVENT_READ)
                            while time.monotonic() < deadline and not ready:
                                if not events.select(.05):
                                    assert p.poll() is None, 'real engine exited during compilation'
                                    continue
                                data = os.read(p.stdout.fileno(), 65536)
                                assert data, 'real engine EOF during compilation'
                                pending += data
                                while b'\n' in pending:
                                    raw, pending = pending.split(b'\n', 1)
                                    message = json.loads(raw)
                                    assert message[0] != 'engine-error', message
                                    if message[0] == 'preview-destination':
                                        assert message == ['preview-destination', 1, 0, 'section.6', 5], message
                                        ready = True
                        assert ready, 'real engine did not reach the sixth page'
                        members = group_members(p.pid)
                        assert len(members) >= 4, f'Expected real checkpoints, got {members}'
                        started = time.monotonic()
                        if mode == 'eof':
                            p.stdin.close(); assert p.wait(timeout=5) == 0
                        elif mode == 'supervisor-term':
                            p.terminate(); p.wait(timeout=5)
                        elif mode == 'group-term':
                            os.killpg(p.pid, signal.SIGTERM); p.wait(timeout=5)
                        else:
                            p.kill(); p.wait(timeout=5)
                        wait_workers_gone(members)
                        assert not group_members(p.pid), 'uncaptured checkpoint descendants survived'
                        assert sentinel.poll() is None, 'unrelated session was stopped'
                        print(f'real checkpoint cleanup: {mode}: {len(members)} processes gone in '
                              f'{time.monotonic() - started:.3f}s', flush=True)
                    except Exception:
                        log.seek(0); print(log.read().decode(errors='replace')[-12000:], file=sys.stderr)
                        raise
                    finally:
                        try:
                            os.killpg(p.pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                        p.wait(timeout=5)
                        wait_workers_gone(group_members(p.pid))
                        p.stdin.close(); p.stdout.close()
        finally:
            sentinel.kill(); sentinel.wait(timeout=5)


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--owner':
        owner(sys.argv[2])
    else:
        main()
        if len(sys.argv) == 2:
            real_engine(Path(sys.argv[1]).resolve())
