#!/usr/bin/env python3
"""Keep diagnostic collection out of production guest, JIT and storage hot paths."""
from pathlib import Path
from eglantine_source import source

root = Path(__file__).resolve().parents[1]
cache = Path((root / '.local/headless-cache').read_text().strip())
generated = cache / 'native-local/headless'
cmake_cache = (generated.parent / 'CMakeCache.txt').read_text()
development = 'EDEN_DEV_PROFILE:BOOL=ON' in cmake_cache
# EDEN_MKW_DIAGNOSTICS builds time each JIT run (headless/CMakeLists.txt, the MKW probe).
mkw_diagnostics = 'EDEN_MKW_DIAGNOSTICS:BOOL=ON' in cmake_cache

JIT = 'src/dynarmic/src/dynarmic/backend/x64/'
for name in (JIT + 'a32_interface.cpp', JIT + 'a64_interface.cpp', JIT + 'block_of_code.cpp',
             'src/core/arm/dynarmic/arm_dynarmic_32.cpp', 'src/core/arm/dynarmic/arm_dynarmic_64.cpp'):
    text = source(name)
    if not mkw_diagnostics:
        assert 'Eden::Performance::Timer' not in text, name
    if not development:
        assert 'Eden::Performance::SampleCpu' not in text
    assert 'Eden::Performance::cpu_state' not in text

jit = source(JIT + 'a64_interface.cpp')
block = jit.split('    CodePtr GetBlock(', 1)[1].split(
    '    void PerformRequestedCacheInvalidation', 1)[0]
for statement in (
    'A64::Translate(ir_block, arch_descriptor, get_code,',
    'Optimization::Optimize(ir_block, conf, polyfill_options);',
    'const auto emitted = emitter.Emit(ir_block, false);',
):
    assert statement in block

for bits in (32, 64):
    wrapper = source(f'src/core/arm/dynarmic/arm_dynarmic_{bits}.cpp')
    assert ('m_core_index == 0 ? 256_MiB : m_core_index < 3 ? 192_MiB : 128_MiB' if bits == 64 else
            'm_core_index == 0 ? 512_MiB : m_core_index < 3 ? 64_MiB : 16_MiB') in wrapper
    if bits == 64:  # Switch 2 games: 688 MiB over six cores (CMakeLists.txt).
        assert 'IsSwitch2() ? (m_core_index == 0 ? 160_MiB : m_core_index == 1 ? 144_MiB : m_core_index < 6 ? 96_MiB : 16_MiB)' in wrapper
    assert 'config.code_cache_size = std::uint32_t(8_MiB);' in wrapper  # Null JIT stays small.
    if development:
        assert f'EDEN_DEV_JIT bits={bits}' in wrapper
        assert 'Eden::Performance::SampleCpu' in wrapper

cmake = (root / 'headless/CMakeLists.txt').read_text()
assert 'Timer read_timer' not in cmake
assert 'Timer compile_timer' not in cmake
assert 'protection_timer' not in cmake
main = (root / 'headless/main.cpp').read_text()
assert 'Performance::Reset()' not in main
assert 'Performance::Report()' not in main
assert 'Settings::values.renderer_debug = false;' in main
print('ARM32/ARM64 JIT budgets and development-only CPU sampling PASS')
