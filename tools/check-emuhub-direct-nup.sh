#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
${CXX:-c++} -std=c++20 -O0 -Wall -Wextra -Werror -Iheadless tools/tests/test_emuhub_direct_nup.cpp -o "$scratch/admission"
"$scratch/admission"
python3 - "$scratch/minimal.nup" <<'PY'
import hashlib, struct, sys
from pathlib import Path
p=Path(sys.argv[1]); toc=b"TOC\x01"+bytes(0x2c); superblock=bytearray(0x80)
superblock[:4]=b"NUP\x01"; struct.pack_into("<H",superblock,4,0x80)
struct.pack_into("<Q",superblock,8,0x80); struct.pack_into("<Q",superblock,0x10,len(toc))
superblock[0x18:0x38]=hashlib.sha256(toc).digest()
struct.pack_into("<Q",superblock,0x38,0xb0); struct.pack_into("<Q",superblock,0x40,0xb1)
p.write_bytes(superblock+toc+b"\0")
PY
${CXX:-c++} -std=c++20 -O0 -Wall -Wextra -Werror -Iheadless tools/tests/test_emuhub_held_nup_fd.cpp -lcrypto -o "$scratch/held"
"$scratch/held" "$scratch/minimal.nup"
