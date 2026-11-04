#!/usr/bin/env python3
"""Small provider regressions; no MiKTeX installation/download required."""
import ctypes
import base64
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
root = Path(__file__).resolve().parents[1]
script = (root/'src/common/miktex_windows.ps1').read_text()
assert script.isascii(), 'encoded-command template must be ASCII; request data is UTF-8 base64'
expected = '/* Generated from miktex_windows.ps1; checked by test-miktex-provider.py. */\nstatic const char miktex_windows_script[] =\n'+''.join(json.dumps(line)+'\n' for line in script.splitlines(True))+';\n'
assert (root/'src/common/miktex_windows.h').read_text() == expected
with tempfile.TemporaryDirectory(prefix='texpresso-miktex-') as temp:
    temp = Path(temp)
    (temp/'stub.c').write_text('const char *texlive_file_path(const char *name, void *record) { (void)name; (void)record; return 0; }')
    library = temp/('provider.dylib' if sys.platform == 'darwin' else 'provider.so')
    subprocess.run(['cc', '-shared', '-fPIC', '-DMIKTEX_LOOKUP_TIMEOUT_MS=1000', '-Wall', '-Wextra', '-Werror',
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
if name == "pdftex.map" and (root/"override.map").is_file(): path = root/"override.map"
if not path.is_file(): sys.exit(1)
print(path)
''')
    command.chmod(0o755)
    utility = temp/'miktex'
    utility.write_text('#!'+sys.executable+'\nimport json,os,pathlib,sys\nwith (pathlib.Path(os.environ["MIKTEX_TEST_ROOT"])/"calls").open("a") as log: log.write(json.dumps(sys.argv[1:])+"\\n")\n')
    utility.chmod(0o755)
    os.environ['PATH'] = str(temp)+os.pathsep+os.environ.get('PATH','')
    os.environ['MIKTEX_TEST_ROOT'] = str(temp)
    lib = ctypes.CDLL(str(library))
    lib.miktex_available.restype = ctypes.c_bool
    lib.miktex_refresh_fontmaps.restype = ctypes.c_bool
    lib.miktex_use_windows.argtypes = [ctypes.c_bool]
    lib.miktex_cache_name.restype = ctypes.c_char_p
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
    for filename, filetype in [('tex-text.tec','misc fonts'), ('figure.pdf','graphic/figure')]:
        (temp/filename).write_bytes(b'resource')
        assert lib.miktex_file_path(filename.encode(), None)
        assert '--file-type='+filetype in json.loads((temp/'calls').read_text().splitlines()[-1])
    (temp/'pdftex.map').write_text('old system map')
    assert lib.miktex_file_path(b'pdftex.map', None) == str(temp/'pdftex.map').encode()
    (temp/'override.map').write_text('new user map')
    assert lib.miktex_refresh_fontmaps()
    assert json.loads((temp/'calls').read_text().splitlines()[-1]) == ['--disable-installer','fontmaps','configure']
    assert lib.miktex_file_path(b'pdftex.map', None) == str(temp/'override.map').encode()
    lib.miktex_select(True, True)
    assert lib.miktex_refresh_fontmaps()
    assert json.loads((temp/'calls').read_text().splitlines()[-1]) == ['--enable-installer','fontmaps','configure']
    (temp/'install.sty').write_text('installed')
    assert lib.miktex_file_path(b'install.sty', None)
    assert '--must-exist' in json.loads((temp/'calls').read_text().splitlines()[-1])
    # Exercise the real encoded command builder and path converter with tiny
    # fake interop tools. Actual PowerShell/job semantics run on Windows CI.
    powershell = temp/'powershell.exe'
    powershell.write_text('#!'+sys.executable+'\n'+r'''
import base64,json,os,pathlib,re,sys
root=pathlib.Path(os.environ['MIKTEX_TEST_ROOT'])
text=base64.b64decode(sys.argv[-1]).decode('utf-16-le')
request=base64.b64decode(re.search(r"\$encoded='([^']*)'",text)[1]).decode('utf8').split('\0')
with (root/'windows-calls').open('a') as log: log.write(json.dumps(request)+'\n')
if '--version' in request: print('MiKTeX Windows test'); sys.exit(0)
if request[1]=='miktex.exe': sys.exit(0)
print('D:\\MiKTeX café\\'+request[-1])
''')
    powershell.chmod(0o755)
    converter=temp/'wslpath'
    converter.write_text('#!'+sys.executable+'\n'+r'''
import os,pathlib,sys
assert sys.argv[1]=='-u'
assert sys.argv[2].startswith('D:\\MiKTeX café\\')
print(pathlib.Path(os.environ['MIKTEX_TEST_ROOT'])/sys.argv[2].rsplit('\\',1)[1])
''')
    converter.chmod(0o755)
    windows_bin = r"C:\MiKTeX café ' $(literal)"
    os.environ['TEXPRESSO_MIKTEX_WINDOWS_BIN'] = windows_bin
    lib.miktex_use_windows(True); lib.miktex_select(True,False)
    assert lib.miktex_cache_name().startswith(b'miktex-windows-')
    assert lib.miktex_available()
    bridge_name = "bridge space ' $(literal).sty"
    (temp/bridge_name).write_text('Windows package')
    assert lib.miktex_file_path(bridge_name.encode(),None) == str(temp/bridge_name).encode()
    request=json.loads((temp/'windows-calls').read_text().splitlines()[-1])
    assert request[0] == windows_bin and request[1]=='findtexmf.exe' and request[-1]==bridge_name
    assert '--must-exist' not in request
    assert lib.miktex_refresh_fontmaps()
    assert json.loads((temp/'windows-calls').read_text().splitlines()[-1])[1:]==['miktex.exe','--disable-installer','fontmaps','configure']
    lib.miktex_use_windows(False)
    assert lib.miktex_cache_name()==b'miktex'
    lib.miktex_select(True, False)
    started = time.monotonic()
    assert not lib.miktex_file_path(b'hang.tex', None)
    assert time.monotonic()-started < 3, 'closed stdout cannot evade deadline'
    assert not (temp/'sentinel').exists()
print('PASS: safe argv, positive cache, retry misses, explicit install policy, bounded child lifetime')
