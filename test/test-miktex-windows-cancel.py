#!/usr/bin/env python3
"""Real WSL interop: crashing the Linux lookup owner releases Windows children."""
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
root = Path(__file__).resolve().parents[1]
windows_folder = sys.argv[1]
folder = Path(subprocess.check_output(['wslpath','-u',windows_folder],text=True).strip())
with tempfile.TemporaryDirectory(prefix='texpresso-windows-cancel-') as temporary:
    temporary = Path(temporary)
    source = temporary/'driver.c'
    source.write_text('''#include "providers.h"
const char *texlive_file_path(const char *name, FILE *record) { (void)name; (void)record; return NULL; }
int main(void) { miktex_select(true,false); miktex_use_windows(true); return miktex_file_path("cancel.tex",NULL) ? 0 : 1; }
''')
    binary = temporary/'driver'
    subprocess.run(['cc','-I'+str(root/'src/include'),str(source),str(root/'src/common/miktex_provider.c'),'-o',str(binary)],check=True)
    p = subprocess.Popen([str(binary)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,start_new_session=True,
                         env={**os.environ,'TEXPRESSO_MIKTEX_WINDOWS_BIN':windows_folder})
    try:
        end = time.monotonic()+60
        while not all((folder/name).exists() for name in ('lookup.pid','worker.pid')):
            if p.poll() is not None: raise AssertionError(p.communicate()[1].decode(errors='replace'))
            assert time.monotonic() < end, 'Windows fixture did not start'
            time.sleep(.1)
        ids = [int((folder/name).read_text()) for name in ('lookup.pid','worker.pid')]
        assert all(pid > 0 for pid in ids)
        started = time.monotonic()
        os.killpg(p.pid,signal.SIGKILL)
        p.communicate(timeout=10)
        query = '$items=@(Get-Process -Id '+','.join(map(str,ids))+' -ErrorAction SilentlyContinue); if ($items.Count) { exit 1 }; exit 0'
        while subprocess.run(['powershell.exe','-NoProfile','-NonInteractive','-Command',query],capture_output=True).returncode:
            assert time.monotonic()-started < 15, ('Windows lookup workers survived cancellation',ids)
            time.sleep(.2)
        print('PASS: killing Linux owner released Windows lookup and worker in %.1fs' % (time.monotonic()-started))
    finally:
        if p.poll() is None: os.killpg(p.pid,signal.SIGKILL)
        p.communicate()
