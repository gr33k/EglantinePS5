#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""build-hud-shaders.py OUTPUT_DIR: compiles the HUD's shaders (headless/vulkan_hud.vert, .frag) to
SPIR-V and writes them as arrays in OUTPUT_DIR/vulkan_hud_shaders.h, which the Vulkan window pass
in Eglantine/ includes. (The rest of the Vulkan port, once edits made here to a pinned Eden source,
is part of Eglantine/ itself.)"""
from pathlib import Path
import struct
import subprocess
import sys

output = Path(sys.argv[1])
port = Path(__file__).resolve().parents[1] / 'headless'
output.mkdir(parents=True, exist_ok=True)
shader_header = '#pragma once\n#include <cstdint>\n'
for stage in ('vert', 'frag'):
    binary = output / f'vulkan_hud.{stage}.spv'
    subprocess.run(['glslangValidator', '-V', '--target-env', 'vulkan1.0',
                    '-o', str(binary), str(port / f'vulkan_hud.{stage}')], check=True)
    subprocess.run(['spirv-val', '--target-env', 'vulkan1.0', str(binary)], check=True)
    data = binary.read_bytes()
    words = struct.unpack('<' + 'I' * (len(data) // 4), data)
    shader_header += f'inline constexpr uint32_t EDEN_HUD_{stage.upper()}_SPV[] = {{\n'
    shader_header += ','.join(hex(word) for word in words) + '\n};\n'
path = output / 'vulkan_hud_shaders.h'
if not path.exists() or path.read_text() != shader_header:
    path.write_text(shader_header)
