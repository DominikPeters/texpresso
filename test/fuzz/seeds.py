#!/usr/bin/env python3
"""Regenerate the committed seed inputs in test/fuzz/seeds/<harness>/.

Seeds only give libFuzzer a starting point; the corpus it grows lives in
build/fuzz/corpus and is not committed. Crash inputs worth keeping go to
test/fuzz/regressions/<harness>/ instead.
"""
from pathlib import Path
import struct

SEEDS = Path(__file__).resolve().parent / 'seeds'


def save(harness, name, data):
    d = SEEDS / harness
    d.mkdir(parents=True, exist_ok=True)
    (d / name).write_bytes(data if isinstance(data, bytes) else data.encode())


def special(s):
    b = s.encode()
    return bytes([239, len(b)]) + b


PRE = bytes([247, 7]) + struct.pack('>III', 25400000, 473628672, 1000) + b'\0'
BOP = bytes([139]) + bytes(44)
EOP = bytes([140])
SPECIALS = [
    'pdf:dest (target) [@thispage /XYZ @xpos @ypos null]',
    'pdf:bann << /Subtype /Link /A << /S /URI /URI (https://example.test) >> >>',
    'pdf:eann',
    'pdf:code 0 0 m 100 100 l S',
    'pdf:code q 1 0 0 1 10 10 cm 0 0 5 5 re f Q',
    'pdf:btrans matrix 1 0 0 1 2 3',
    'pdf:etrans',
    'pdfcolorstackinit 4 page direct (0 g)',
    'pdfcolorstack 4 push (1 g)',
    'pdfcolorstack 4 pop',
    'color push rgb 1 0 0',
    'color pop',
    'x:rotate 30',
    'pdf:pagesize width 100pt height 200pt',
    'landscape',
]

# dvi: whole outputs.
save('dvi', 'empty-page', PRE + BOP + EOP)
save('dvi', 'rule-page', PRE + BOP + bytes([132]) + struct.pack('>II', 655360, 1310720) + EOP)
for i, s in enumerate(SPECIALS):
    save('dvi', f'special-{i:02}', PRE + BOP + special(s) + EOP)
for i, b in enumerate([bytes([239, 255]), bytes([242]) + bytes([255]) * 4,
                       bytes([253, 0, 0, 0, 0, 255, 255]), bytes([254, 255, 255]),
                       bytes([235, 255]), bytes([141]) * 33 + bytes([142]) * 34]):
    save('dvi', f'boundary-{i}', PRE + BOP + b + EOP)

# incremental: operation sequences (see incremental.c for the encoding).
for i, b in enumerate([b'\x03\x05\x00\x07\x01\x03\x04\x02',
                       b'\x03\x08\x07\x07\x00\x07\x01\x00\x10\x06\x01',
                       b'\x04\x10\x05\x05\x05\x06\x06\x00\x40\x03\x02\x00\x01',
                       b'\x02\x03\x03\x07\x00\x05\x02\x05\x06\x02',
                       b'\x03\x04\x0c\xf7\x0c\x00\x01\x00\x10\x03\x01\x01']):
    save('incremental', f'strict-{i}', b'\x01' + b)
    save('incremental', f'lax-{i}', b'\x00' + b)

# special: NUL-separated specials.
save('special', 'all', '\0'.join(SPECIALS))
for i, s in enumerate(SPECIALS):
    save('special', f'one-{i:02}', s)
save('special', 'colorstack-across-pages',
     '\0'.join(['pdfcolorstackinit 2 page direct (0 g)', 'pdfcolorstack 2 push (1 0 0 rg)',
                'pdf:code 0 0 m 10 10 l S', 'x:rotate 10', 'pdfcolorstack 2 pop']))


# channel: socket writes, then reader actions (see channel.c).
def chunk(data, fd=False):
    return bytes([1 if fd else 0]) + struct.pack('>H', len(data)) + data


def q(tag, *u32s, tail=b''):
    return tag + b''.join(struct.pack('<I', v) for v in u32s) + tail


seen = q(b'SEEN', 5, 1, 10)
chld = q(b'CHLD', 7, 1234)
save('channel', 'seen-chld', bytes([2]) + chunk(seen) + chunk(chld, fd=True) + bytes([4, 4, 4]))
save('channel', 'reset-before-chld', bytes([1]) + chunk(seen + chld, fd=True) + bytes([4, 1, 4]))
save('channel', 'queries', bytes([1]) + chunk(
    q(b'OPRD', 1, 0, tail=b'main.tex\0TEX\0') + q(b'READ', 2, 0, 0, 4096) +
    q(b'APND', 3, 1, 3, tail=b'abc') + q(b'SIZE', 4, 0) + q(b'CLOS', 5, 0) +
    q(b'GPIC', 6, tail=b'a.pdf\0' + struct.pack('<II', 1, 0)) + q(b'FINI', 7, 1)) + bytes([4] * 8))
save('channel', 'peek-at-eof', bytes([0, 0, 0]))

# editor: first byte = flags (bit 0: JSON, bit 1: raw text).
save('editor', 'json-commands', b'\x03' + b'["open", "main.tex", "x"]\n'
     b'["change", "main.tex", 1, 2, "ab"]\n["change-lines", "main.tex", 1, 1, "l\\n"]\n'
     b'["change-range", "main.tex", 2, 3, 2, 5, "\\u00e9"]\n')
save('editor', 'sexp-commands', b'\x02' + b'(open "main.tex" "x")\n'
     b'(change "main.tex" 1 2 "ab")\n(change-lines "main.tex" 1 1 "l\n")\n'
     b'(change-range "main.tex" 2 3 2 5 "x")\n')
save('editor', 'synthesized-json', b'\x01' + bytes(range(0, 64)))
save('editor', 'synthesized-sexp', b'\x00' + bytes(range(64, 128)))
print(SEEDS)
