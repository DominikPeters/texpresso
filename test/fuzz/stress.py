#!/usr/bin/env python3
"""Randomized editing sessions against a sanitizer build of the frontend.

Each iteration launches `texpresso -json -headless -stream` (built by
`fuzz.py build` into build/fuzz/stress) and plays an editor: it opens files,
then sends edits (typing bursts, line inserts and deletions, UTF-16 ranges,
whole-file reopens), render requests, pauses, reruns and barriers with random
timing, so that edits land while workers fork, read, write, die or hang.

Oracles:
  1. the frontend never dies and never reports a sanitizer error;
  2. once edits stop, the session settles, and its pages (rendered PNGs) and
     destinations equal those of a fresh session on the same final files.

Workers:
  fake  test/fuzz/fake_worker.c: deterministic, fast, and the document can
        make it crash, exit or hang (see that file for its input language).
  real  build/texpresso-xetex on generated LaTeX (needs a TeX distribution).

  stress.py --iterations 200 --seed 1
  stress.py --worker real --iterations 20
  stress.py --replay build/fuzz/stress-failures/<case>/actions.json

Failures are saved under build/fuzz/stress-failures/ with the action log, the
final files and the frontend's stderr.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import shutil
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
STRESS = ROOT / 'build/fuzz/stress'
FAILURES = ROOT / 'build/fuzz/stress-failures'
SANITIZER_MARKERS = ('ERROR: AddressSanitizer', 'runtime error:', 'ERROR: LeakSanitizer',
                     'Aborting from', 'AddressSanitizer:DEADLYSIGNAL')


class Frontend:
    """One texpresso process speaking the headless JSON protocol."""

    def __init__(self, bindir, workdir, log_path, label):
        self.workdir = workdir
        self.label = label
        self.log = open(log_path, 'wb')
        env = dict(os.environ, SOURCE_DATE_EPOCH='1711843200',
                   ASAN_OPTIONS='detect_leaks=0:abort_on_error=1:symbolize=1',
                   UBSAN_OPTIONS='print_stacktrace=1:halt_on_error=1')
        self.proc = subprocess.Popen([str(bindir / 'texpresso'), '-json', '-headless', '-stream', 'main.tex'],
                                     cwd=workdir, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=self.log, env=env)
        self.messages = []
        self.images = {}
        self.cond = threading.Condition()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()
        self.sent = []

    def _read(self):
        stream = self.proc.stdout
        while True:
            line = stream.readline()
            if not line:
                break
            try:
                msg = json.loads(line)
            except ValueError:
                continue
            if isinstance(msg, list) and msg and msg[0] == 'preview-image':
                header = msg[1]
                body = stream.read(header['byteLength'])
                with self.cond:
                    self.images[header['requestId']] = (header, body)
                    self.cond.notify_all()
                continue
            with self.cond:
                self.messages.append(msg)
                self.cond.notify_all()

    def send(self, command):
        self.sent.append(command)
        try:
            self.proc.stdin.write((json.dumps(command) + '\n').encode())
            self.proc.stdin.flush()
        except (BrokenPipeError, OSError):
            pass

    def alive(self):
        return self.proc.poll() is None

    def wait_for(self, predicate, timeout):
        deadline = time.monotonic() + timeout
        with self.cond:
            while True:
                result = predicate()
                if result is not None:
                    return result
                remaining = deadline - time.monotonic()
                if remaining <= 0 or not self.alive():
                    return predicate()
                self.cond.wait(min(remaining, 0.1))

    def latest(self, kind):
        for msg in reversed(self.messages):
            if msg and msg[0] == kind:
                return msg
        return None

    def close(self):
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        self.log.close()


def utf16_len(s):
    return len(s.encode('utf-16-le')) // 2


def position(text, offset):
    """(line, UTF-16 character) of a character offset."""
    before = text[:offset]
    line = before.count('\n')
    start = before.rfind('\n') + 1
    return line, utf16_len(text[start:offset])


class Documents:
    """The editor's view of the files and the edit commands that change it."""

    def __init__(self, rng, flavor, workdir):
        self.rng = rng
        self.flavor = flavor
        self.workdir = workdir
        self.files = {}

    def path(self, name):
        return str(self.workdir / name)

    def open_command(self, name):
        return ['open', self.path(name), self.files[name]]

    def random_edit(self):
        rng = self.rng
        name = rng.choice(sorted(self.files))
        text = self.files[name]
        kind = rng.random()
        if kind < 0.35:  # insert a line from the vocabulary
            lines = text.split('\n')
            at = rng.randrange(len(lines) + 1)
            start = sum(len(l) + 1 for l in lines[:at])
            start = min(start, len(text))
            return name, self._replace(name, start, start, self.flavor.line(rng) + '\n')
        if kind < 0.55:  # delete a line or a span
            if not text:
                return name, None
            a = rng.randrange(len(text))
            b = text.find('\n', a)
            b = len(text) if b < 0 or rng.random() < 0.3 else b + 1
            if rng.random() < 0.5:
                b = min(len(text), a + rng.randrange(1, 8))
            return name, self._replace(name, a, b, '')
        if kind < 0.8:  # a typing burst: several one-character edits
            at = rng.randrange(len(text) + 1)
            word = self.flavor.word(rng)
            return name, [self._replace(name, at + i, at + i, ch)[0] for i, ch in enumerate(word)]
        if kind < 0.9:  # replace a span
            if not text:
                return name, None
            a = rng.randrange(len(text))
            b = min(len(text), a + rng.randrange(0, 20))
            return name, self._replace(name, a, b, self.flavor.word(rng))
        # reopen the whole file with new contents
        new = self.flavor.document(rng, name)
        self.files[name] = new
        return name, [self.open_command(name)]

    def _replace(self, name, a, b, insertion):
        text = self.files[name]
        if self.rng.random() < 0.7:
            l0, c0 = position(text, a)
            l1, c1 = position(text, b)
            command = ['change-range', self.path(name), l0, c0, l1, c1, insertion]
        else:
            command = ['change', self.path(name), len(text[:a].encode()),
                       len(text[a:b].encode()), insertion]
        self.files[name] = text[:a] + insertion + text[b:]
        return [command]


class FakeFlavor:
    """Documents in fake_worker.c's input language."""
    words = ['alpha', 'beta', 'x', 'café', '\U0001d6fc', 'gamma delta', '{', '}', '\\']
    lines = ['some text', 'more words here', '', '', '\\label{a}', '\\label{b}', '\\ref{a}',
             '\\ref{b}', '\\input{sub.tex}', '\\slow', 'café \U0001d6fc', '\\label{c}']
    rare = ['\\crash', '\\exit', '\\hang']

    def line(self, rng):
        if rng.random() < 0.03:
            return rng.choice(self.rare)
        return rng.choice(self.lines)

    def word(self, rng):
        return rng.choice(self.words)

    def document(self, rng, name):
        body = [rng.choice(self.lines) for _ in range(rng.randrange(2, 12))]
        if name == 'main.tex':
            return '\n'.join(['preamble', '\\begin{document}'] + body + ['\\end{document}']) + '\n'
        return '\n'.join(body) + '\n'

    def initial_files(self, rng):
        return {'main.tex': self.document(rng, 'main.tex'), 'sub.tex': self.document(rng, 'sub.tex')}

    def settles(self, files):
        # A hanging worker never finishes its pass.
        return not any('\\hang' in text for text in files.values())


class LatexFlavor:
    """Small LaTeX documents with constructs that break while being typed."""
    words = ['word', ' ', '$x^2$', '\\emph{e}', '{', '}', '$', '\\', '\\ref{s1}', 'é', '%']
    lines = ['Some text in a paragraph.', '', '\\section{Intro}\\label{s1}', 'See \\ref{s1}.',
             '\\begin{itemize}\\item one\\end{itemize}', '$$a + b = c$$', '\\input{sub}',
             '\\textbf{bold}', '\\newpage', 'Café naïve.', '\\begin{equation}x\\end{equation}']

    def line(self, rng):
        return rng.choice(self.lines)

    def word(self, rng):
        return rng.choice(self.words)

    def document(self, rng, name):
        body = [rng.choice(self.lines) for _ in range(rng.randrange(2, 10))]
        if name == 'main.tex':
            return '\n'.join(['\\documentclass{article}', '\\begin{document}'] + body +
                             ['\\end{document}']) + '\n'
        return '\n'.join(body) + '\n'

    def initial_files(self, rng):
        return {'main.tex': self.document(rng, 'main.tex'), 'sub.tex': self.document(rng, 'sub.tex')}

    def settles(self, files):
        return True


def sanitizer_report(log_path):
    text = Path(log_path).read_text(errors='replace')
    for marker in SANITIZER_MARKERS:
        at = text.find(marker)
        if at >= 0:
            return text[max(0, at - 2000):at + 6000]
    return None


def settle(fe, sequence, timeout, quiet=2.0):
    """Wait until the session reports a finished pass for `sequence` and then
    stays silent for `quiet` seconds: auxiliary-file reruns start after 500ms
    of idleness and publish new states. Returns (revision, page_count,
    complete) or None."""
    def done():
        for msg in reversed(fe.messages):
            if msg[0] == 'preview-state' and msg[2] == sequence and msg[4]:
                return msg[1], msg[3], True
            if msg[0] == 'preview-stopped' and msg[2] == sequence:
                return msg[1], msg[3], False
        return None
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        state = fe.wait_for(done, deadline - time.monotonic())
        if state is None:
            return None
        count = len(fe.messages)
        time.sleep(quiet)
        if len(fe.messages) == count and done() == state:
            return state
    return None


def snapshot(fe, revision, sequence, pages, labels, request_base):
    """Rendered pages and destination pages, as comparable values."""
    result = {'pages': pages, 'images': [], 'destinations': {}}
    for page in range(min(pages, 8)):
        rid = request_base + page
        fe.send(['preview-render', rid, page, 48, revision, sequence])
        got = fe.wait_for(lambda: fe.images.get(rid) or next(
            (m for m in fe.messages if m[0] in ('preview-error', 'preview-retry') and m[1] == rid), None), 20)
        if isinstance(got, tuple):
            result['images'].append(hashlib.sha256(got[1]).hexdigest()[:16])
        else:
            result['images'].append(repr(got and got[0]))
    for name in labels:
        fe.send(['preview-destination', revision, sequence, name])
        got = fe.wait_for(lambda: next((m for m in reversed(fe.messages)
                                        if m[0] == 'preview-destination' and m[3] == name), None), 20)
        result['destinations'][name] = got[4] if got else 'timeout'
    return result


def run_session(bindir, workdir, files, actions, rng, label, flavor, settle_timeout):
    """Launch, open, run actions (or none), settle. Returns (frontend, outcome)."""
    log_path = workdir / f'{label}.stderr.log'
    fe = Frontend(bindir, workdir, log_path, label)
    for name, text in files.items():
        fe.send(['open', str(workdir / name), text])
    # Rerun until auxiliary files converge, so that both sessions compare
    # converged output whatever passes the actions happened to trigger.
    fe.send(['rerun', True])
    fe.send(['resume'])
    sequence = 0
    for action in actions:
        kind = action[0]
        if kind == 'sleep':
            time.sleep(action[1])
        elif kind == 'send':
            fe.send(action[1])
        if not fe.alive():
            break
    sequence = 1 + sum(1 for a in actions if a[0] == 'send' and a[1][0] == 'preview-barrier')
    fe.send(['preview-barrier', sequence])
    if not fe.alive():
        return fe, ('died', None)
    state = settle(fe, sequence, settle_timeout)
    return fe, ('settled', (state, sequence)) if state else ('timeout', sequence)


def generate_actions(rng, docs, count):
    actions = []
    barrier = 0
    for _ in range(count):
        r = rng.random()
        if r < 0.6:
            _, commands = docs.random_edit()
            for c in commands or []:
                actions.append(('send', c))
                if rng.random() < 0.5:
                    actions.append(('sleep', rng.choice([0, 0, 0.001, 0.003])))
        elif r < 0.7:
            barrier += 1
            actions.append(('send', ['preview-barrier', barrier]))
        elif r < 0.8:
            actions.append(('send', ['preview-render', 1000 + len(actions), rng.randrange(4), 64,
                                     rng.randrange(1, 6), barrier]))
        elif r < 0.85:
            actions.append(('send', [rng.choice(['pause', 'resume', 'rerun-once', 'rescan'])]))
        elif r < 0.9:
            actions.append(('send', ['preview-destination', rng.randrange(1, 6), barrier,
                                     rng.choice(['a', 'b', 's1'])]))
        actions.append(('sleep', rng.choice([0, 0.002, 0.005, 0.02, 0.05])))
    # Leave the session runnable.
    actions.append(('send', ['resume']))
    return actions


def labels_in(files):
    names = set()
    for text in files.values():
        for part in text.split('\\label{')[1:]:
            names.add(part.split('}', 1)[0][:20])
    return sorted(n for n in names if n and '\n' not in n)


def iteration(args, seed, bindir):
    rng = random.Random(seed)
    flavor = FakeFlavor() if args.worker == 'fake' else LatexFlavor()
    with tempfile.TemporaryDirectory(prefix='txp-stress-') as tmp:
        tmp = Path(os.path.realpath(tmp))
        edited, fresh_dir = tmp / 'edited', tmp / 'fresh'
        edited.mkdir()
        fresh_dir.mkdir()
        docs = Documents(rng, flavor, edited)
        docs.files = flavor.initial_files(rng)
        initial = dict(docs.files)
        actions = generate_actions(rng, docs, rng.randrange(5, args.max_actions))
        final = dict(docs.files)
        problem = run_case(args, bindir, edited, fresh_dir, initial, actions, final, flavor, rng)
        if problem:
            save_failure(seed, tmp, initial, actions, final, problem, args)
        return problem


def run_case(args, bindir, edited, fresh_dir, initial, actions, final, flavor, rng):
    fe, (status, detail) = run_session(bindir, edited, initial, actions, rng, 'edited', flavor,
                                       args.settle_timeout)
    try:
        report = sanitizer_report(edited / 'edited.stderr.log')
        if report:
            return 'sanitizer report in edited session:\n' + report
        if status == 'died' or not fe.alive():
            return f'edited session died (exit {fe.proc.poll()})'
        if status == 'timeout':
            if flavor.settles(final):
                return 'edited session did not settle'
            return None
        (revision, pages, complete), sequence = detail
        labels = labels_in(final)
        mine = snapshot(fe, revision, sequence, pages, labels, 100000)
        mine['complete'] = complete
    finally:
        fe.close()
    report = sanitizer_report(edited / 'edited.stderr.log')
    if report:
        return 'sanitizer report in edited session (after snapshot):\n' + report

    fresh, (status, detail) = run_session(bindir, fresh_dir, final, [], rng, 'fresh', flavor,
                                          args.settle_timeout)
    try:
        if status != 'settled':
            return f'fresh session did not settle: {status}'
        (revision, pages, complete), sequence = detail
        theirs = snapshot(fresh, revision, sequence, pages, labels_in(final), 200000)
        theirs['complete'] = complete
    finally:
        fresh.close()
    if mine != theirs:
        return f'edited session differs from a fresh run:\n edited: {mine}\n fresh:  {theirs}'
    return None


def save_failure(seed, tmp, initial, actions, final, problem, args):
    case = FAILURES / f'{args.worker}-seed-{seed}'
    if case.exists():
        shutil.rmtree(case)
    case.mkdir(parents=True)
    (case / 'actions.json').write_text(json.dumps({'worker': args.worker, 'seed': seed, 'initial': initial,
                                                   'actions': actions, 'final': final}, indent=1))
    (case / 'problem.txt').write_text(problem)
    for log in tmp.glob('*/*.stderr.log'):
        shutil.copy(log, case / f'{log.parent.name}-{log.name}')
    print(f'  saved {case}')


def worker_bindir(args, tmp):
    if args.worker == 'fake':
        return STRESS
    worker = ROOT / 'build/texpresso-xetex'
    if not worker.exists():
        sys.exit(f'{worker} is missing: run `make texpresso-xetex`')
    bindir = Path(tmp) / 'bin'
    bindir.mkdir()
    (bindir / 'texpresso').symlink_to(STRESS / 'texpresso')
    (bindir / 'texpresso-xetex').symlink_to(worker)
    if (STRESS / 'libSDL3.dylib').exists():
        (bindir / 'libSDL3.dylib').symlink_to((STRESS / 'libSDL3.dylib').resolve())
    return bindir


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--worker', choices=['fake', 'real'], default='fake')
    parser.add_argument('--iterations', type=int, default=50)
    parser.add_argument('--seconds', type=float, default=0, help='stop starting iterations after this long')
    parser.add_argument('--seed', type=int, default=None)
    parser.add_argument('--max-actions', type=int, default=40)
    parser.add_argument('--settle-timeout', type=float, default=None)
    parser.add_argument('--replay', type=Path, help='rerun a saved actions.json')
    args = parser.parse_args()
    if args.settle_timeout is None:
        args.settle_timeout = 20 if args.worker == 'fake' else 120
    if not (STRESS / 'texpresso').exists():
        sys.exit(f'{STRESS / "texpresso"} is missing: run `python3 test/fuzz/fuzz.py build`')

    with tempfile.TemporaryDirectory(prefix='txp-stress-bin-') as bintmp:
        if args.replay:
            saved = json.loads(args.replay.read_text())
            args.worker = saved['worker']
            bindir = worker_bindir(args, bintmp)
            flavor = FakeFlavor() if args.worker == 'fake' else LatexFlavor()
            with tempfile.TemporaryDirectory(prefix='txp-replay-') as tmp:
                tmp = Path(os.path.realpath(tmp))
                (tmp / 'edited').mkdir()
                (tmp / 'fresh').mkdir()
                # Paths in the log point at the original directory; rewrite them.
                actions = [tuple(a) for a in saved['actions']]
                problem = run_case(args, bindir, tmp / 'edited', tmp / 'fresh', saved['initial'],
                                   rewrite_paths(actions, tmp / 'edited'), saved['final'], flavor,
                                   random.Random(saved['seed']))
                print(problem or 'no problem reproduced')
            return
        bindir = worker_bindir(args, bintmp)
        start = time.monotonic()
        base = args.seed if args.seed is not None else random.randrange(1 << 30)
        failures = 0
        for i in range(args.iterations):
            if args.seconds and time.monotonic() - start > args.seconds:
                break
            seed = base + i
            problem = iteration(args, seed, bindir)
            status = 'ok' if not problem else 'FAIL: ' + problem.splitlines()[0]
            print(f'[{i + 1}/{args.iterations}] seed {seed}: {status}', flush=True)
            failures += bool(problem)
        print(f'{failures} failing iteration(s)')
        sys.exit(1 if failures else 0)


def rewrite_paths(actions, directory):
    out = []
    for action in actions:
        if action[0] == 'send' and len(action[1]) > 1 and isinstance(action[1][1], str) and action[1][1].startswith('/'):
            command = list(action[1])
            command[1] = str(directory / Path(command[1]).name)
            out.append(('send', command))
        else:
            out.append(action)
    return out


if __name__ == '__main__':
    main()
