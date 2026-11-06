import importlib.util
from pathlib import Path
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
for unsafe in (b'', b'.', b'..', b'/root', b'a/b', b'a\\b', b'a\x00b'):
    try: m.safe_component(unsafe)
    except (ValueError, UnicodeDecodeError): pass
    else: raise AssertionError(f'accepted {unsafe!r}')
print('PASS update overrides shared/code; base-only survives; add-on separate; unsafe paths rejected')
