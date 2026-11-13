#!/usr/bin/env python3
"""Compile the actual generated frequency-selection block against CPUID cases."""
from pathlib import Path
import subprocess
import tempfile
from eglantine_source import EGLANTINE, source as eglantine_source  # the core, part of this repository

root = Path(__file__).resolve().parents[1]
cache = Path((root / '.local/headless-cache').read_text().strip())
derived = eglantine_source('src/common/cpu_features.cpp')
# One measured-TSC fallback, for every invariant TSC under 1 GHz (Eglantine/'s cpu_features.cpp).
assert derived.count('X64::EstimateRDTSCFrequency()') == 1
assert "if (caps.invariant_tsc && caps.tsc_frequency < 1'000'000'000ULL) {" in derived
selection = derived[derived.index('    if (max_std_fn >= 0x15) {'):derived.index('    if (max_std_fn >= 0x16) {')]
code = '''
#include <cassert>
#include <cstdint>
using u64 = uint64_t;
static unsigned estimates;
static u64 estimated;
namespace X64 { u64 EstimateRDTSCFrequency() { ++estimates; return estimated; } }
void __cpuid(int*, int) {} // cpu_id already holds the requested fixture.
u64 select(unsigned max_std_fn, bool invariant, int denominator, int numerator, int crystal) {
    int cpu_id[]{denominator, numerator, crystal, 0};
    struct { bool invariant_tsc; u64 tsc_frequency = 0, tsc_crystal_ratio_denominator = 0,
        tsc_crystal_ratio_numerator = 0, crystal_frequency = 0; } caps{invariant};
''' + selection + '''
    return caps.tsc_frequency;
}
int main() {
    estimated = 2'200'000'000;
    assert(select(0x10, true, 0, 0, 0) == estimated && estimates == 1);
    assert(select(0x15, true, 2, 4, 0) == estimated && estimates == 2);
    assert(select(0x15, true, 0, 0, 0) == estimated && estimates == 3);
    assert(select(0x15, true, 1, 100, 24'000'000) == 2'400'000'000 && estimates == 3);
    assert(select(0x10, false, 0, 0, 0) == 0 && estimates == 3);
    estimated = 0;
    assert(select(0x10, true, 0, 0, 0) == 0 && estimates == 4);
    estimated = 500'000'000;
    assert(select(0x10, true, 0, 0, 0) < 1'000'000'000 && estimates == 5);
}
'''
with tempfile.TemporaryDirectory(prefix='eden-tsc-') as directory:
    path = Path(directory)
    (path / 'check.cpp').write_text(code)
    subprocess.run(['clang++-18', '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(path / 'check.cpp'), '-o', str(path / 'check')], check=True)
    subprocess.run([str(path / 'check')], check=True, timeout=5)
print('Actual TSC selector: missing leaf/crystal/denominator, valid CPUID, noninvariant and rejected estimates PASS')
