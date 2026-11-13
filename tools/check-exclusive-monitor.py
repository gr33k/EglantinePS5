#!/usr/bin/env python3
"""Exercise Dynarmic's monitor operations with the PS5 lock (Eglantine/)."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
cache = Path((root / '.local/headless-cache').read_text().strip())
from eglantine_source import EGLANTINE, source as eglantine_source  # the core, part of this repository
dynarmic = EGLANTINE / 'src/dynarmic/src'
monitor = eglantine_source('src/dynarmic/src/dynarmic/backend/x64/exclusive_monitor.cpp')
# This implementation includes, but does not use, the application logger/asserts.
monitor = monitor.replace('#include "common/assert.h"', '')
# The PS5 lock (Eglantine/'s spin_lock_x64.cpp): from its ABI note to the end of Unlock.
spin = eglantine_source('src/dynarmic/src/dynarmic/common/spin_lock_x64.cpp')
lock_start = spin.index('// PS5 x64: retain the storage ABI')
unlock = spin.index('void SpinLock::Unlock() noexcept {', lock_start)
lock = spin[lock_start:spin.index('\n}\n', unlock) + 3]
source = r'''
#include <cassert>
#include <thread>
#include <vector>
#include <barrier>
#include "dynarmic/interface/exclusive_monitor.h"
namespace Dynarmic {
LOCK
}
MONITOR
int main() {
    Dynarmic::ExclusiveMonitor monitor(4);
    unsigned value = 0;
    auto read = [&](unsigned core) { return monitor.ReadAndMark<unsigned>(core, 0x1000, [&] { return value; }); };
    auto increment = [&](unsigned core) { return monitor.DoExclusiveOperation<unsigned>(core, 0x1000, [&](unsigned expected) {
        if (value != expected) return false;
        ++value;
        return true;
    }); };
    read(0); read(1);
    assert(increment(0) && !increment(1)); // One write invalidates peer reservation.
    read(0); monitor.ClearProcessor(0); assert(!increment(0));
    read(0); read(1); monitor.Clear(); assert(!increment(0) && !increment(1));
    read(0);
    assert(!monitor.DoExclusiveOperation<unsigned>(0, 0x2000, [](unsigned) { assert(false); return true; }));
    monitor.Clear(); value = 0;
    constexpr unsigned workers = 4, iterations = 50000;
    std::barrier ready(workers);
    std::vector<std::jthread> threads;
    for (unsigned core = 0; core < workers; ++core) threads.emplace_back([&, core] {
        ready.arrive_and_wait();
        for (unsigned i = 0; i < iterations; ++i) {
            do { read(core); } while (!increment(core));
        }
    });
    threads.clear();
    assert(value == workers * iterations);
}
'''.replace('LOCK', lock).replace('MONITOR', monitor)
boost = cache / 'source/.cache/cpm/boost/1.92.0'
includes = ['-I' + str(dynarmic)] + ['-I' + str(p) for p in sorted(boost.glob('libs/*/include'))]
assert len(includes) > 1
with tempfile.TemporaryDirectory(prefix='eden-monitor-') as tmp:
    cpp = Path(tmp) / 'check.cpp'
    binary = Path(tmp) / 'check'
    cpp.write_text(source)
    subprocess.run(['clang++-18', '-std=c++20', '-O3', '-flto=thin', '-fuse-ld=lld-18', '-pthread', '-Wall', '-Wextra',
                    '-Werror', *includes, str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=20)
native = cache / 'native-local/bin/eden-headless'
symbols = subprocess.check_output(['llvm-nm-18', '--defined-only', str(native)], text=True)
outlined = '_ZN8Dynarmic8SpinLock4LockEv' in symbols
names = ('_ZN8Dynarmic8SpinLock4LockEv,_ZN8Dynarmic8SpinLock6UnlockEv' if outlined
         else '_ZN8Dynarmic16ExclusiveMonitor14ClearProcessorEm')
assembly = subprocess.check_output(['llvm-objdump-18',
    '--disassemble-symbols=' + names,
    str(native)], text=True)
for name in names.split(','):
    assert '<' + name + '>:' in assembly
assert not re.search(r'\bcallq?\b', assembly), 'Unexpected runtime call in native monitor lock'
# ThinLTO inlines the lock in ClearProcessor and may lower the same seq_cst
# fence to a locked zero-OR on the stack instead of MFENCE.
full_fence = 'mfence' in assembly or re.search(r'\block\s*\n[^\n]*\borl\s+\$0x0,\s*-0x[0-9a-f]+\(%rsp\)', assembly)
assert full_fence and 'pause' in assembly and assembly.count('xchgl') == 2
print('Exclusive monitor: peer invalidation, clear, wrong address and 200000 contended increments PASS')
print('Native lock machine code: no calls, exchange acquisition/release and full fence PASS')
