# SPDX-License-Identifier: GPL-3.0-or-later
"""The core's sources (Eglantine/) as one build configuration compiles them.

Where the PS5 build, the host's test build and the development switches need different code, the
source has a chain of conditionals whose lines end in a fixed comment:

    #if !defined(__PROSPERO__) // not PS5        (or #if defined(__PROSPERO__) // PS5: a PS5-only block)
    #elif defined(EDEN_DEV_PROFILE) // PS5 development build
    #elif defined(EDEN_MKW_DIAGNOSTICS) // PS5 Mario Kart diagnostics
    #elif !defined(EDEN_INLINE_EXCLUSIVES) // PS5 without inline exclusive accesses
    #elif !defined(EDEN_A32_FASTMEM) // PS5 without the A32 fastmem window
    #else // PS5
    #endif // PS5

source() resolves only those chains (every other directive stays as written), for checks that
look at what a configuration compiles. defines() is the set of the current build: the PS5 build
(__PROSPERO__) with the switches its CMakeCache.txt has on.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
EGLANTINE = ROOT / 'Eglantine'
LABELS = ('// not PS5', '// PS5 development build', '// PS5 Mario Kart diagnostics',
          '// PS5 without inline exclusive accesses', '// PS5 without the A32 fastmem window')
PS5_ONLY = '#if defined(__PROSPERO__) // PS5'  # a PS5-only block, closed by END
ELSE, END = '#else // PS5', '#endif // PS5'
DEFAULTS = {'EDEN_INLINE_EXCLUSIVES', 'EDEN_A32_FASTMEM'}


def defines(build='native-local', ps5=True):
    """__PROSPERO__ (unless ps5=False) and the switches the build's CMakeCache.txt has on."""
    found = set(DEFAULTS)
    try:
        cache = Path((ROOT / '.local/headless-cache').read_text().strip()) / build / 'CMakeCache.txt'
        text = cache.read_text()
        for name in ('EDEN_DEV_PROFILE', 'EDEN_MKW_DIAGNOSTICS'):
            if f'{name}:BOOL=ON' in text:
                found.add(name)
        for name in ('EDEN_INLINE_EXCLUSIVES', 'EDEN_A32_FASTMEM'):
            if f'{name}:BOOL=OFF' in text:
                found.discard(name)
    except OSError:
        pass
    if ps5:
        found.add('__PROSPERO__')
    return found


def resolve(text, active):
    out, stack = [], []  # per chain: [a branch was taken, this branch is taken]

    def true(expression):
        name = re.search(r'defined\((\w+)\)', expression).group(1)
        return (name in active) != expression.lstrip().startswith('!')

    for line in text.split('\n'):
        if line == PS5_ONLY:
            taken = '__PROSPERO__' in active
            stack.append([taken, taken])
            continue
        if line.endswith(LABELS) and line.startswith('#if '):
            taken = true(line[4:].split(' //')[0])
            stack.append([taken, taken])
            continue
        if line.endswith(LABELS) and line.startswith('#elif ') and stack:
            taken = not stack[-1][0] and true(line[6:].split(' //')[0])
            stack[-1] = [stack[-1][0] or taken, taken]
            continue
        if line == ELSE and stack:
            stack[-1][1] = not stack[-1][0]
            continue
        if line == END and stack:
            stack.pop()
            continue
        if all(entry[1] for entry in stack):
            out.append(line)
    return '\n'.join(out)


def source(relative, active=None):
    """Eglantine/<relative> as the configuration `active` (default: defines()) compiles it."""
    return resolve((EGLANTINE / relative).read_text(), defines() if active is None else active)
