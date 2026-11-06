import importlib.util
from pathlib import Path
import os
import tempfile
import zstandard
from nup import format as F
spec = importlib.util.spec_from_file_location('prepare', Path(__file__).resolve().parents[1] / 'tools/wonder_edition_prepare.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
base = {'exefs/main': {'bytes': 4, 'sha256': 'a'}, 'romfs/old': {'bytes': 2, 'sha256': 'b'}, 'romfs/shared': {'bytes': 3, 'sha256': 'c'}}
update = {'exefs/main': {'bytes': 5, 'sha256': 'd'}, 'romfs/shared': {'bytes': 6, 'sha256': 'e'}, 'romfs/new': {'bytes': 7, 'sha256': 'f'}}
merged = m.merge_entries(base, update)
assert merged['exefs/main'] == update['exefs/main']
assert merged['romfs/shared'] == update['romfs/shared']
assert merged['romfs/old'] == base['romfs/old']
assert merged['romfs/new'] == update['romfs/new']
assert len(merged) == 4 and len(base) == 3 and len(update) == 3
assert m.canonical_digest(merged) != m.canonical_digest(base)
assert m.update_package_allowed('010015100B514000', '010015100B514000', '010015100B514000')
assert m.update_package_allowed('0400486024DEC000', '0400486024DEC800', '0400486024DEC000')
assert m.update_package_allowed('0400245021D2C000', '0400245021D2C000', '0400245021D2C000')
assert not m.update_package_allowed('0400486024DEC000', '0400486024DEC800', '0400486024DEC800')
class Reader:
    def __init__(self, fh, size, extent, codec): self._fh=fh; self.info=(b'x',size,b'',0,extent,codec)
    def file_info(self, index): assert index==0; return self.info
with tempfile.TemporaryFile() as fh:
    data=b'a'*10000+b'b'*50000
    fh.write(data); fh.flush()
    assert b''.join(m.member_chunks(Reader(fh,len(data),len(data),F.CODEC_RAW),0))==data
with tempfile.TemporaryFile() as fh:
    packed=zstandard.ZstdCompressor().compress(data)
    fh.write(packed); fh.flush()
    assert b''.join(m.member_chunks(Reader(fh,len(data),len(packed),F.CODEC_ZSTD),0))==data

for unsafe in (b'', b'.', b'..', b'/root', b'a/b', b'a\\b', b'a\x00b'):
    try: m.safe_component(unsafe)
    except (ValueError, UnicodeDecodeError): pass
    else: raise AssertionError(f'accepted {unsafe!r}')
print('PASS generic Wonder/Ocarina/Yoshi update IDs; merge/base-only; raw/zstd streamed members; unsafe paths rejected')
