#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Names the EDEN_HANG_STACK lines of a stderr.log (headless/hang_stacks.h).

  python3 -I tools/symbolize-hang.py STDERR_LOG ELF [--report N] [--depth D] [--only NAME,...]

Each line's offsets are values found on a thread's stack that point into the app's code; the ones
that do not follow a call instruction in ELF are dropped, the rest are named, nearest first.
"""
import argparse, importlib.util, subprocess
from pathlib import Path
here = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('symcrash', here / 'symbolize-crash.py')
sc = importlib.util.module_from_spec(spec); spec.loader.exec_module(sc)
ap = argparse.ArgumentParser(); ap.add_argument('log'); ap.add_argument('elf')
ap.add_argument('--report', type=int, default=-1); ap.add_argument('--depth', type=int, default=12)
ap.add_argument('--only', default='')
a = ap.parse_args()
elf = Path(a.elf).read_bytes(); start, at, size = sc.sections(elf)['.text']
reports, cur = [], None
for line in open(a.log, errors='replace'):
    if line.startswith('EDEN_HANG '): cur = (line.strip(), []); reports.append(cur)
    elif line.startswith('EDEN_HANG_STACK ') and cur: cur[1].append(line.split())
head, rows = reports[a.report]
only = set(filter(None, a.only.split(',')))
parsed, wanted = [], set()
for f in rows:
    name, kind, rip = f[1], f[2], int(f[3].split('=')[1].rstrip(':'), 16)
    if only and not any(name.startswith(o) for o in only): continue
    offs = []
    for x in f[4:]:
        o = int(x, 16)
        if 8 <= o <= size and sc.after_call(elf[at + o - 8:at + o]) and o not in offs: offs.append(o)
    offs = offs[:a.depth]; parsed.append((name, kind, rip, offs))
    wanted |= {o - 1 for o in offs}
    if rip < size: wanted.add(rip)
wanted = sorted(wanted)
out = subprocess.run(['llvm-symbolizer', '-e', a.elf, '-f', '-C', '--no-inlines', *[hex(start + w) for w in wanted]],
                     capture_output=True, text=True).stdout
names = dict(zip(wanted, [b.strip().split('\n')[0] for b in out.strip().split('\n\n')]))
print(head)
for name, kind, rip, offs in parsed:
    print(f'== {name} ({kind}) rip {names.get(rip, hex(rip))[:110]}')
    for o in offs: print(f'     {o:#9x} {names.get(o - 1, "?")[:150]}')
