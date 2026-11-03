#!/usr/bin/env python3
"""Build, run and replay the TeXpresso fuzzers.

  fuzz.py build                 build every harness into build/fuzz
  fuzz.py run [NAME...]         fuzz each harness (default: all) for --seconds
  fuzz.py replay [NAME...]      run seeds and regression inputs once, no fuzzing
  fuzz.py editing-tests         build and run the deterministic editing test

Harnesses are built with AddressSanitizer, UndefinedBehaviorSanitizer and
libFuzzer from fresh objects; release objects are never reused. Library flags
come from Makefile.config (run `make config` first) unless FUZZ_CFLAGS and
FUZZ_LIBS are set. The compiler is FUZZ_CC/FUZZ_CXX, or the first clang found
that ships libFuzzer (Apple's clang does not; `brew install llvm` does).
"""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
FUZZ = ROOT / 'test/fuzz'
OUT = ROOT / 'build/fuzz'

HARNESSES = ['dvi', 'incremental', 'special', 'channel', 'editor']

FRONTEND_SOURCES = ['incdvi', 'myabort', 'sprotocol', 'editor', 'json_parser',
                    'sexp_parser', 'prot_parser']
INCLUDES = ['src/include', 'src/dvi', 'src/frontend', 'test/fuzz']


def engine_sources():
    sources = sorted(p for p in (ROOT / 'src/dvi').glob('*.c') if not p.name.endswith('.re2c.c'))
    sources += sorted((ROOT / 'src/common').glob('*.c'))
    sources += [ROOT / f'src/frontend/{name}.c' for name in FRONTEND_SOURCES]
    return sources


def has_libfuzzer(cc):
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / 'probe.c'
        src.write_text('#include <stddef.h>\n#include <stdint.h>\n'
                       'int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) { return 0; }\n')
        try:
            result = subprocess.run(cc + ['-fsanitize=fuzzer,address', str(src), '-o', str(Path(tmp) / 'probe')],
                                    capture_output=True)
        except OSError:
            return False
        return result.returncode == 0


def find_compilers():
    if 'FUZZ_CC' in os.environ:
        cc = shlex.split(os.environ['FUZZ_CC'])
        cxx = shlex.split(os.environ.get('FUZZ_CXX', os.environ['FUZZ_CC'] + '++'))
        return cc, cxx
    candidates = []
    if shutil.which('clang'):
        candidates.append(shutil.which('clang'))
    for prefix in ['/opt/homebrew/opt', '/usr/local/opt']:
        candidates.append(f'{prefix}/llvm/bin/clang')
        candidates += sorted((str(p) for p in Path(prefix).glob('llvm@*/bin/clang')), reverse=True)
    # Non-Apple clang on macOS needs to be pointed at the SDK for libc++.
    sysroot = []
    if sys.platform == 'darwin':
        sdk = subprocess.run(['xcrun', '--show-sdk-path'], capture_output=True, text=True).stdout.strip()
        if sdk:
            sysroot = ['-isysroot', sdk, f'-L{sdk}/usr/lib']
    for cc in candidates:
        if not Path(cc).exists():
            continue
        for extra in ([], sysroot):
            if has_libfuzzer([cc] + extra):
                return [cc] + extra, [cc + '++'] + extra
    sys.exit('No clang with libFuzzer found. Install one (e.g. `brew install llvm`) '
             'or set FUZZ_CC/FUZZ_CXX.')


def makefile_config():
    path = ROOT / 'Makefile.config'
    if not path.exists():
        sys.exit('Makefile.config is missing: run `make config`, or set FUZZ_CFLAGS and FUZZ_LIBS.')
    values = {}
    for line in path.read_text().splitlines():
        key, sep, value = line.partition('=')
        if sep:
            values[key.strip()] = value.strip()
    return values


def library_flags():
    if 'FUZZ_CFLAGS' in os.environ and 'FUZZ_LIBS' in os.environ:
        return shlex.split(os.environ['FUZZ_CFLAGS']), shlex.split(os.environ['FUZZ_LIBS'])
    config = makefile_config()
    cflags = [f for f in shlex.split(config.get('CFLAGS', '')) if f.startswith(('-I', '-D'))]
    try:
        cflags += subprocess.check_output(['pkg-config', '--cflags', 'freetype2'], text=True).split()
    except (OSError, subprocess.CalledProcessError):
        pass
    # No harness uses SDL, and Homebrew's sdl2-compat can show a modal error
    # dialog when it is loaded without a usable SDL3.
    libs = [f for f in shlex.split(config.get('LIBS', '')) if f != '-lSDL2']
    return cflags, libs


def compile_flags(fuzzer):
    sanitizers = 'address,undefined' + (',fuzzer-no-link' if fuzzer else '')
    return ['-g', '-O1', '-fno-omit-frame-pointer', '-fno-sanitize-recover=all',
            '-fsanitize=' + sanitizers, '-UNDEBUG', '-w']


def build(out, targets, fuzzer=True):
    """targets: {binary name: harness source}."""
    out.mkdir(parents=True, exist_ok=True)
    cc, cxx = find_compilers()
    cflags, libs = library_flags()
    flags = compile_flags(fuzzer) + cflags + [f'-I{ROOT / d}' for d in INCLUDES]
    commands = []

    def compile_one(source):
        obj = out / ('_'.join(source.relative_to(ROOT).parts) + '.o')
        command = cc + flags + ['-c', str(source), '-o', str(obj)]
        subprocess.run(command, check=True)
        return command, obj

    sources = engine_sources() + list(targets.values())
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        compiled = dict(zip(sources, pool.map(compile_one, sources)))
    commands += [c for c, _ in compiled.values()]
    engine_objects = [str(compiled[s][1]) for s in engine_sources()]
    link_sanitizers = 'address,undefined' + (',fuzzer' if fuzzer else '')
    for name, source in targets.items():
        link = cxx + ['-fsanitize=' + link_sanitizers, str(compiled[source][1])]
        link += engine_objects + libs + ['-o', str(out / name)]
        subprocess.run(link, check=True)
        commands.append(link)
        print(f'built {out / name}')
    revision = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=ROOT, capture_output=True, text=True).stdout.strip()
    (out / 'build.json').write_text(json.dumps({'revision': revision, 'commands': commands}, indent=2) + '\n')


def selected(names):
    for name in names:
        if name not in HARNESSES:
            sys.exit(f'unknown harness {name!r}; choose from {", ".join(HARNESSES)}')
    return names or HARNESSES


def inputs(name):
    """Committed seeds and regression inputs for one harness."""
    dirs = [FUZZ / 'seeds' / name, FUZZ / 'regressions' / name]
    return [d for d in dirs if d.is_dir() and any(d.iterdir())]


def command_build(args):
    build(OUT, {f'fuzz-{name}': FUZZ / f'{name}.c' for name in HARNESSES})


def command_run(args):
    failed = []
    for name in selected(args.names):
        binary = OUT / f'fuzz-{name}'
        if not binary.exists():
            sys.exit(f'{binary} is missing: run `fuzz.py build` first')
        corpus = OUT / 'corpus' / name
        findings = OUT / 'findings' / name
        corpus.mkdir(parents=True, exist_ok=True)
        findings.mkdir(parents=True, exist_ok=True)
        command = [str(binary), f'-max_total_time={args.seconds}', f'-artifact_prefix={findings}/',
                   '-print_final_stats=1', f'-timeout={args.timeout}', '-rss_limit_mb=4096']
        if args.jobs > 1:
            command += [f'-fork={args.jobs}', '-ignore_crashes=1', '-ignore_ooms=1', '-ignore_timeouts=1']
        dictionary = FUZZ / 'dicts' / f'{name}.dict'
        if dictionary.exists():
            command.append(f'-dict={dictionary}')
        command += [str(corpus)] + [str(d) for d in inputs(name)]
        print(f'== fuzzing {name} for {args.seconds}s', flush=True)
        log = OUT / f'{name}.log'
        with log.open('w') as f:
            subprocess.run(command, stdout=f, stderr=subprocess.STDOUT)
        crashes = sorted(p.name for p in findings.iterdir())
        print(f'   corpus: {sum(1 for _ in corpus.iterdir())} inputs, findings: {len(crashes)} (log: {log})')
        if crashes:
            failed.append(name)
            for crash in crashes[:10]:
                print(f'   {findings / crash}')
    if failed:
        sys.exit(f'findings in: {", ".join(failed)}')


def command_replay(args):
    failed = []
    for name in selected(args.names):
        binary = OUT / f'fuzz-{name}'
        if not binary.exists():
            sys.exit(f'{binary} is missing: run `fuzz.py build` first')
        files = [str(p) for d in inputs(name) for p in sorted(d.iterdir()) if p.is_file()]
        if not files:
            continue
        result = subprocess.run([str(binary), '-runs=1', f'-timeout={args.timeout}'] + files,
                                capture_output=True, text=True)
        status = 'ok' if result.returncode == 0 else 'FAILED'
        print(f'{name}: {len(files)} inputs {status}')
        if result.returncode != 0:
            failed.append(name)
            print(result.stderr[-4000:])
    if failed:
        sys.exit(f'replay failed: {", ".join(failed)}')


def command_editing_tests(args):
    out = ROOT / 'build/editing-tests'
    build(out, {'test-editing-lifecycle': ROOT / 'test/test_editing_lifecycle.c'}, fuzzer=False)
    log = out / 'test-editing-lifecycle.stderr.log'
    print(f'running editing tests (60s timeout); diagnostics: {log}', flush=True)
    with log.open('w') as f:
        subprocess.run([str(out / 'test-editing-lifecycle')], check=True, timeout=60, stderr=f)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('build').set_defaults(func=command_build)
    run = sub.add_parser('run')
    run.add_argument('names', nargs='*')
    run.add_argument('--seconds', type=int, default=60)
    run.add_argument('--jobs', type=int, default=1, help='parallel fuzzing processes per harness')
    run.add_argument('--timeout', type=int, default=10, help='seconds before an input counts as a hang')
    run.set_defaults(func=command_run)
    replay = sub.add_parser('replay')
    replay.add_argument('names', nargs='*')
    replay.add_argument('--timeout', type=int, default=30)
    replay.set_defaults(func=command_replay)
    sub.add_parser('editing-tests').set_defaults(func=command_editing_tests)
    args = parser.parse_args()
    args.func(args)


if __name__ == '__main__':
    main()
