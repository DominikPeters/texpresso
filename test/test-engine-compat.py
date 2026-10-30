#!/usr/bin/env python3
"""Behavioral comparisons with installed XeLaTeX; uses disposable caches/files."""
import argparse
import hashlib
import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--worker', type=Path, required=True)
parser.add_argument('--reference', type=Path, required=True)
parser.add_argument('--old-worker', type=Path)
args, remaining = parser.parse_known_args()
args.worker = args.worker.resolve(strict=True)
# Keep argv[0]: resolving the xelatex symlink changes its selected format.
args.reference = args.reference.absolute()
if args.old_worker:
    args.old_worker = args.old_worker.resolve(strict=True)


class EngineCompatibility(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='texpresso-primitives-')
        cls.root = Path(cls.temp.name)
        cls.serial = 0
        cls.env = dict(os.environ, XDG_CACHE_HOME=str(cls.root / 'cache'),
                       SOURCE_DATE_EPOCH='1711843200')
        cls.env['PATH'] = str(args.reference.parent) + os.pathsep + cls.env.get('PATH', '')

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_tex(self, body, reference=False, preamble='', worker=None, success=True):
        type(self).serial += 1
        directory = self.root / str(self.serial)
        directory.mkdir()
        source = '\\documentclass{article}\n' + preamble + '\n\\begin{document}\n' + body + '\n\\end{document}\n'
        (directory / 'main.tex').write_text(source)
        command = ([str(args.reference), '-interaction=nonstopmode', '-no-pdf', 'main.tex'] if reference
                   else [str(worker or args.worker), '-texlive', 'main.tex'])
        result = subprocess.run(command, cwd=directory, env=self.env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=60)
        output = result.stdout.decode(errors='replace')
        if success:
            self.assertEqual(result.returncode, 0, output[-6000:])
        return result.returncode, output, directory

    def compare(self, body, preamble=''):
        _, actual, _ = self.run_tex(body, preamble=preamble)
        _, expected, _ = self.run_tex(body, reference=True, preamble=preamble)
        markers = lambda text: re.findall(r'^CHECK:(.*)$', text, re.M)
        self.assertTrue(markers(expected), expected[-2000:])
        self.assertEqual(markers(actual), markers(expected))
        return markers(actual)

    def test_primitive_identity_and_redefinition(self):
        names = ['relax', 'count', 'hbox', 'partokencontext', 'partokenname',
                 'showstream', 'ignoreprimitiveerror']
        body = '\n'.join(r'\expandafter\ifprimitive\csname ' + name + r'\endcsname'
                         r'\typeout{CHECK:' + name + r':yes}\else\typeout{CHECK:' + name + r':no}\fi'
                         for name in names)
        body += r'\begingroup\def\hbox{}\ifprimitive\hbox\typeout{CHECK:redefined:yes}\else\typeout{CHECK:redefined:no}\fi\endgroup'
        self.assertEqual(self.compare(body), [name + ':yes' for name in names] + ['redefined:no'])

    def test_parameter_scopes(self):
        self.compare(r'''
\typeout{CHECK:defaults:\the\partokencontext,\the\showstream,\the\ignoreprimitiveerror}
\partokencontext=0
{\partokencontext=1 \showstream=7 \ignoreprimitiveerror=3
 \typeout{CHECK:local:\the\partokencontext,\the\showstream,\the\ignoreprimitiveerror}}
\typeout{CHECK:restored:\the\partokencontext,\the\showstream,\the\ignoreprimitiveerror}
{\global\partokencontext=2 \global\ignoreprimitiveerror=1}
\typeout{CHECK:global:\the\partokencontext,\the\ignoreprimitiveerror}
''')

    def test_paragraph_contexts(self):
        body = r'''
\newcount\hits
\def\parHook{\global\advance\hits by1\primitive\par}
\long\def\checkBox#1#2{\global\hits=0 #2\typeout{CHECK:#1:\the\hits}}
\partokenname\parHook
'''
        contexts = {
            'vbox': r'\setbox0=\vbox{A}',
            'vtop': r'\setbox0=\vtop{A}',
            'vcenter': r'\setbox0=\hbox{$\vcenter{A}$}',
            'vadjust': r'\setbox0=\vbox{A\vadjust{B}}',
            'insert': r'\setbox0=\vbox{\insert250{B}A}',
            'noalign': r'\setbox0=\vbox{\halign{#\cr A\cr\noalign{B}}}',
            'valign': r'\setbox0=\hbox{\valign{#\cr A\cr}}',
            'blank': '\\setbox0=\\vbox{A\n\nB}',
        }
        for level in range(3):
            body += '\\partokencontext=' + str(level) + '\n'
            for name, code in contexts.items():
                body += '\\checkBox{' + str(level) + '-' + name + '}{' + code + '}\n'
        body += r'\partokenname\par'
        markers = self.compare(body)
        self.assertIn('0-vbox:0', markers)
        self.assertIn('1-vbox:1', markers)
        self.assertIn('2-valign:1', markers)

    def test_output_paragraph_context(self):
        for level in range(3):
            with self.subTest(level=level):
                self.compare(r'''
\newcount\hits
\def\parHook{\global\advance\hits by1\primitive\par}
\partokenname\parHook
\partokencontext=''' + str(level) + r'''
\output={\primitive\shipout\box255\global\output={\primitive\shipout\box255}X}
A\eject
\typeout{CHECK:output:\the\hits}
\partokenname\par
''')

    def test_global_single_character_paragraph_name(self):
        self.assertIn('hook:1', self.compare(r'''
\newcount\hits
\def\?{\global\advance\hits by1\primitive\par}
{\partokenname\?}
\partokencontext=1
\setbox0=\vbox{A}
\typeout{CHECK:hook:\the\hits}
\partokenname\par
'''))

    def test_showstream_redirects_all_show_commands(self):
        body = r'''
\newwrite\auditout
\immediate\openout\auditout=show.txt
\showstream=\auditout
\show\relax
\showthe\partokencontext
\setbox0=\hbox{A}\showbox0
\showlists
\showgroups
\iftrue\showifs\fi
\typeout{CHECK:selector-restored}
\immediate\closeout\auditout
\showstream=-1
'''
        streams = []
        for reference in [False, True]:
            with self.subTest(reference=reference):
                _, output, directory = self.run_tex(body, reference=reference)
                text = (directory / 'show.txt').read_text()
                for item in [r'> \relax=\relax', '> 2\n', r'\box0=', '###', 'level']:
                    self.assertIn(item, text)
                self.assertNotIn(r'> \relax=\relax', output)
                self.assertIn('CHECK:selector-restored', output)
                streams.append(text)
        self.assertEqual(streams[0], streams[1])

    def test_unopened_showstream_keeps_diagnostics(self):
        for reference in [False, True]:
            _, output, _ = self.run_tex(r'\showstream=15\show\relax', reference=reference, success=False)
            self.assertIn(r'> \relax=\relax', output)

    def test_vsplit_error_control_and_log_only_warning(self):
        for flag in range(4):
            for reference in [False, True]:
                with self.subTest(flag=flag, reference=reference):
                    body = '\\ignoreprimitiveerror=' + str(flag) + r'''
\vbadness=10000\vfuzz=\maxdimen
\setbox0=\vbox{\hbox{A}\vskip0pt minus1fil\hbox{B}}
\setbox1=\vsplit0 to20pt
\typeout{CHECK:after-split}
'''
                    code, output, directory = self.run_tex(body, reference=reference, success=False)
                    message = 'Infinite glue shrinkage found in box being split'
                    if flag & 1:
                        self.assertEqual(code, 0, output[-2000:])
                        self.assertNotIn(message, output)
                        self.assertIn('ignored: ' + message, (directory / 'main.log').read_text())
                    else:
                        self.assertNotEqual(code, 0)
                        self.assertIn('! ' + message, output)
                    self.assertIn('CHECK:after-split', output)

    def test_microtype_title_and_footnote(self):
        self.compare(r'\maketitle Example.\footnote{Example footnote.}\typeout{CHECK:microtype-ok}',
                     r'\usepackage{microtype}\usepackage{authblk}\title{Example}\author[1]{Example Author}\affil[1]{Example Institute}')

    def test_cached_format_round_trip(self):
        body = r'\typeout{CHECK:format:\the\partokencontext,\the\showstream,\the\ignoreprimitiveerror}Hello.'
        _, first, directory = self.run_tex(body)
        formats = list((self.root / 'cache/texpresso/format').glob('*v1001*.fmt'))
        self.assertEqual(len(formats), 1)
        before = formats[0].stat().st_mtime_ns
        digest = hashlib.sha256(formats[0].read_bytes()).digest()
        _, second, again = self.run_tex(body)
        self.assertEqual(formats[0].stat().st_mtime_ns, before)
        self.assertEqual(hashlib.sha256(formats[0].read_bytes()).digest(), digest)
        self.assertEqual((directory / 'main.xdv').read_bytes(), (again / 'main.xdv').read_bytes())
        _, reference, _ = self.run_tex(body, reference=True)
        markers = lambda text: re.findall(r'^CHECK:format:(.*)$', text, re.M)
        self.assertEqual(markers(first), markers(reference))
        self.assertEqual(markers(second), markers(reference))

    @unittest.skipUnless(args.old_worker, 'Pass --old-worker to test migration from format 33')
    def test_old_format_cache_migration(self):
        self.run_tex('Old format.', worker=args.old_worker)
        formats = list((self.root / 'cache/texpresso/format').glob('texlive-xelatex.ini.fmt'))
        self.assertEqual(len(formats), 1)
        original = formats[0].read_bytes()
        self.run_tex(r'\typeout{CHECK:new-format:\the\partokencontext}')
        self.assertEqual(formats[0].read_bytes(), original)
        self.assertTrue(list((self.root / 'cache/texpresso/format').glob('*v1001*.fmt')))


unittest.main(argv=[__file__] + remaining)
