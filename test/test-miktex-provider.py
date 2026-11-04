#!/usr/bin/env python3
"""Small provider regressions; no MiKTeX installation/download required."""
import ctypes
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='texpresso-miktex-') as temp:
    temp = Path(temp)
    (temp/'stub.c').write_text('const char *texlive_file_path(const char *name, void *record) { (void)name; (void)record; return 0; }')
    library = temp/('provider.dylib' if sys.platform == 'darwin' else 'provider.so')
    subprocess.run(['cc', '-shared', '-fPIC', '-DMIKTEX_LOOKUP_TIMEOUT_MS=250', '-Wall', '-Wextra', '-Werror',
                    '-I'+str(root/'src/include'), str(root/'src/common/miktex_provider.c'), str(temp/'stub.c'), '-o', str(library)], check=True)
    command = temp/'findtexmf'
    command.write_text('#!'+sys.executable+'\n'+r'''
import json, os, pathlib, sys, time
root = pathlib.Path(os.environ['MIKTEX_TEST_ROOT'])
with (root/'calls').open('a') as log: log.write(json.dumps(sys.argv[1:])+'\n')
if '--version' in sys.argv: print('MiKTeX test utility'); sys.exit(0)
name = sys.argv[-1]
if name == 'hang.tex':
    os.close(1); time.sleep(30)
path = root/name
if not path.is_file(): sys.exit(1)
print(path)
''')
    command.chmod(0o755)
    os.environ['PATH'] = str(temp)+os.pathsep+os.environ.get('PATH','')
    os.environ['MIKTEX_TEST_ROOT'] = str(temp)
    lib = ctypes.CDLL(str(library))
    lib.miktex_select.argtypes = [ctypes.c_bool, ctypes.c_bool]
    lib.miktex_file_path.argtypes = [ctypes.c_char_p, ctypes.c_void_p]
    lib.miktex_file_path.restype = ctypes.c_char_p
    lib.miktex_select(True, False)
    assert lib.miktex_available()
    name = "font space ' $(touch sentinel).otf"
    (temp/name).write_bytes(b'font')
    assert lib.miktex_file_path(name.encode(), None) == str(temp/name).encode()
    count = len((temp/'calls').read_text().splitlines())
    assert lib.miktex_file_path(name.encode(), None)
    assert len((temp/'calls').read_text().splitlines()) == count, 'positive cache'
    assert not lib.miktex_file_path(b'new.sty', None)
    (temp/'new.sty').write_text('newly installed')
    assert lib.miktex_file_path(b'new.sty', None), 'miss must not be cached'
    assert not any('--must-exist' in json.loads(line) for line in (temp/'calls').read_text().splitlines())
    (temp/'UnicodeData.txt').write_text('unicode table')
    assert lib.miktex_file_path(b'UnicodeData.txt', None)
    assert '--file-type=tex' in json.loads((temp/'calls').read_text().splitlines()[-1])
    lib.miktex_select(True, True)
    (temp/'install.sty').write_text('installed')
    assert lib.miktex_file_path(b'install.sty', None)
    assert '--must-exist' in json.loads((temp/'calls').read_text().splitlines()[-1])
    lib.miktex_select(True, False)
    started = time.monotonic()
    assert not lib.miktex_file_path(b'hang.tex', None)
    assert time.monotonic()-started < 2, 'closed stdout cannot evade deadline'
    assert not (temp/'sentinel').exists()
print('PASS: safe argv, positive cache, retry misses, explicit install policy, bounded child lifetime')
