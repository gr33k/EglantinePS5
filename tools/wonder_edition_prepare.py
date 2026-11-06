"""Prepare the exact Wonder Edition loose tree from its root NUP on the NAS.

Plan reads the NUP TOC only. Materialize reads and verifies the whole source and
selected payload; run it only after the Yoshi I/O gate clears. Never targets PS5.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import sys

from nup.read import NupReader
from nup.access import iter_children
from nup import format as F

ROOT_SHA = '460c025c23fcbd32fffb49ce87647bf0a7d895f21af5ecbd1c6717dbf227efdc'
TOC_SHA = '9d073c56b6d04cc230d7eafc88aafaccbcd5b9addc032623b212d7dcf6625853'
ROOT_BYTES = 8896026108
BASE = '010015100B514000'
AOC = '010015100B515001'
UPDATE = 327680

def safe_component(raw):
    name = raw.decode('utf-8')
    if not name or name in ('.', '..') or '/' in name or '\\' in name or '\x00' in name:
        raise ValueError('unsafe NUP component')
    return name

def package_files(reader, pkg):
    out = {}
    stack = [(pkg.root_dir, '')]
    while stack:
        directory, prefix = stack.pop()
        for node in iter_children(reader, directory):
            name = safe_component(node.name)
            rel = prefix + name
            if node.is_dir:
                stack.append((node.index, rel + '/'))
            else:
                if rel in out:
                    raise ValueError('duplicate path')
                f = reader.files[node.index]
                out[rel] = {'bytes': f[1], 'sha256': f[2].hex(), 'index': node.index}
    return out

def merge_entries(base, update):
    merged = dict(base)
    merged.update(update)
    return merged

def update_package_allowed(base_id, update_id, npdm_id):
    """Accept native package IDs with or without the conventional 0x800 update bit."""
    base = int(base_id, 16)
    return int(update_id, 16) in (base, base | 0x800) and int(npdm_id, 16) == base

def canonical_digest(entries):
    lines = (f'{name}\t{info["bytes"]}\t{info["sha256"]}\n' for name, info in sorted(entries.items()))
    return hashlib.sha256(''.join(lines).encode()).hexdigest()

def source_hash(path):
    h = hashlib.sha256()
    with path.open('rb', buffering=8 * 1024 * 1024) as src:
        while chunk := src.read(8 * 1024 * 1024):
            h.update(chunk)
    return h.hexdigest()

class ExtentReader:
    def __init__(self, fd, offset, size):
        self.fd, self.offset, self.remaining = fd, offset, size
    def read(self, count=-1):
        if not self.remaining: return b''
        take = min(self.remaining, count if count > 0 else 4 * 1024 * 1024)
        data = os.pread(self.fd, take, self.offset)
        if not data: raise ValueError('short NUP extent')
        self.offset += len(data); self.remaining -= len(data)
        return data

def member_chunks(reader, index):
    _, size, _, offset, extent_size, codec = reader.file_info(index)
    raw = ExtentReader(reader._fh.fileno(), offset, extent_size)
    if codec == F.CODEC_RAW:
        if extent_size != size: raise ValueError('raw extent length mismatch')
        stream = raw
    elif codec == F.CODEC_ZSTD:
        import zstandard
        stream = zstandard.ZstdDecompressor().stream_reader(raw)
    else:
        raise ValueError('unknown NUP codec')
    try:
        while chunk := stream.read(4 * 1024 * 1024):
            yield chunk
    finally:
        if codec == F.CODEC_ZSTD: stream.close()
    if raw.remaining:
        raise ValueError('compressed extent not fully consumed')

def write_member(reader, dest, rel, info, replace=False):
    target = dest / rel
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.is_symlink() or (target.exists() and not replace):
        raise ValueError('destination collision')
    tmp = target.with_name(target.name + '.nup-part')
    h = hashlib.sha256(); count = 0
    with tmp.open('xb') as out:
        for chunk in member_chunks(reader, info['index']):
            h.update(chunk); count += len(chunk); out.write(chunk)
        out.flush(); os.fsync(out.fileno())
    if count != info['bytes'] or h.hexdigest() != info['sha256']:
        raise ValueError('member SHA/size mismatch')
    os.replace(tmp, target)

def run(args):
    source = Path(args.nup)
    out = Path(args.output)
    before = source.stat()
    if before.st_size != ROOT_BYTES or not source.is_file():
        raise ValueError('source length/type mismatch')
    with NupReader(str(source)) as reader:
        opened = os.fstat(reader._fh.fileno())
        if (opened.st_size, opened.st_ino, opened.st_mtime_ns) != (before.st_size, before.st_ino, before.st_mtime_ns):
            raise ValueError('source changed before TOC read')
        if reader.super['toc_sha256'].hex() != TOC_SHA:
            raise ValueError('TOC SHA mismatch')
        packages = reader.packages
        if len(packages) != 3 or [(p.kind_name, f'{p.title_id:016X}', p.version) for p in packages] != [
            ('base', BASE, 0), ('update', BASE, UPDATE), ('addon', AOC, 0)]:
            raise ValueError('package identities mismatch')
        base, update, addon = [package_files(reader, p) for p in packages]
        if (not update_package_allowed(BASE, format(packages[1].title_id, '016X'), BASE) or
            packages[1].flags != 0 or any(not rel.startswith('romfs/') for rel in addon)):
            raise ValueError('unexpected update/add-on package shape')
        for files in (base, update):
            npdm = reader.read_file(files['exefs/main.npdm']['index'])
            magic = npdm.find(b'ACI0')
            if magic < 0 or int.from_bytes(npdm[magic + 16:magic + 24], 'little') != int(BASE, 16):
                raise ValueError('NPDM ACI0 identity mismatch')
        if [(len(base), sum(v['bytes'] for v in base.values())),
            (len(update), sum(v['bytes'] for v in update.values())),
            (len(addon), sum(v['bytes'] for v in addon.values()))] != [
            (19016, 3759363036), (19038, 3766241273), (26449, 5358773733)]:
            raise ValueError('package counts mismatch')
        merged = merge_entries(base, update)
        if args.file_manifest:
            detail = Path(args.file_manifest)
            if detail.exists() or detail.is_symlink():
                raise ValueError('file manifest output exists')
            detail.write_text(json.dumps({
                'merged': {name: {'bytes': info['bytes'], 'sha256': info['sha256']} for name, info in sorted(merged.items())},
                'addon': {name[6:]: {'bytes': info['bytes'], 'sha256': info['sha256']} for name, info in sorted(addon.items())},
            }, sort_keys=True) + '\n')
        if merged['exefs/main']['sha256'] != 'ff9a3728246d45a750a686d1625bdeafb0a361ea75e19964675a14f629f98c32':
            raise ValueError('update code not selected')
        receipt = {
            'root_nup_sha256': ROOT_SHA, 'toc_sha256': TOC_SHA,
            'base_id': BASE, 'update_id': BASE, 'update_raw': UPDATE,
            'addon_id': AOC, 'merged_files': len(merged),
            'merged_bytes': sum(v['bytes'] for v in merged.values()),
            'merged_manifest_sha256': canonical_digest(merged),
            'addon_files': len(addon), 'addon_bytes': sum(v['bytes'] for v in addon.values()),
            'addon_manifest_sha256': canonical_digest(addon),
            'payload_digest': hashlib.sha256((canonical_digest(merged) + '\n' + canonical_digest(addon) + '\n').encode()).hexdigest(),
            'materialized': args.materialize,
        }
        if args.materialize:
            if source_hash(source) != ROOT_SHA:
                raise ValueError('root full SHA mismatch')
            partial = out.with_name(out.name + '.partial')
            if out.exists() or partial.exists():
                raise ValueError('destination exists')
            partial.mkdir(parents=True)
            for rel, info in sorted(base.items()):
                write_member(reader, partial, rel, info)
            for rel, info in sorted(update.items()):
                if base.get(rel, {}).get('sha256') != info['sha256'] or base.get(rel, {}).get('bytes') != info['bytes']:
                    write_member(reader, partial, rel, info, replace=rel in base)
            aoc_dir = partial / 'aoc' / AOC / 'romfs'
            for rel, info in sorted(addon.items()):
                if not rel.startswith('romfs/'):
                    raise ValueError('AOC has non-RomFS payload')
                write_member(reader, aoc_dir, rel[6:], info)
            after = source.stat()
            if (after.st_size, after.st_ino, after.st_mtime_ns) != (before.st_size, before.st_ino, before.st_mtime_ns):
                raise ValueError('source changed during extraction')
            os.replace(partial, out)
            out.with_name(out.name + '.manifest.json').write_text(json.dumps(receipt, indent=2, sort_keys=True) + '\n')
        else:
            if out.exists() or out.is_symlink():
                raise ValueError('plan output exists')
            out.write_text(json.dumps(receipt, indent=2, sort_keys=True) + '\n')
        print(json.dumps(receipt, sort_keys=True))

if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('--nup', required=True)
    p.add_argument('--output', required=True)
    p.add_argument('--materialize', action='store_true')
    p.add_argument('--file-manifest')
    run(p.parse_args())
