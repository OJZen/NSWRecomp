"""Read ExeFS from the final section of a solid NCZ, without writing an NSP.

Format: https://github.com/nicoboss/nsz/blob/master/docs/formats.md
The stream contains plaintext; embedded encryption material is neither used nor logged.
"""
import argparse
from compression.zstd import ZstdFile
import hashlib
import io
import json
from pathlib import Path
import struct
import tempfile


def exact(stream, size):
    data = stream.read(size)
    if len(data) != size:
        raise ValueError('truncated container')
    return data


def pfs_entries(stream, start, size):
    stream.seek(start)
    magic, count, strings, _ = struct.unpack('<4sIII', exact(stream, 16))
    if magic != b'PFS0' or not 0 < count <= 4096 or strings > 1 << 20:
        raise ValueError('invalid PFS0 header')
    header = 16 + count * 24 + strings
    if header > size:
        raise ValueError('PFS0 header exceeds container')
    records = [struct.unpack('<QQII', exact(stream, 24)) for _ in range(count)]
    names = exact(stream, strings)
    entries = {}
    ranges = []
    for offset, length, nameoff, _ in records:
        end = names.find(b'\0', nameoff)
        if nameoff >= strings or end < 0 or offset + length > size - header:
            raise ValueError('invalid PFS0 entry bounds')
        name = names[nameoff:end].decode('utf-8')
        if not name or name in ('.', '..') or '/' in name or '\\' in name or name in entries:
            raise ValueError('unsafe or duplicate PFS0 filename')
        entries[name] = (start + header + offset, length)
        ranges.append((offset, offset + length))
    ordered = sorted(ranges)
    if any(a[1] > b[0] for a, b in zip(ordered, ordered[1:])):
        raise ValueError('overlapping PFS0 entries')
    return entries


class LimitedReader:
    def __init__(self, stream, remaining):
        self.stream, self.remaining = stream, remaining

    def read(self, size=-1):
        size = self.remaining if size < 0 else min(size, self.remaining)
        data = self.stream.read(size)
        self.remaining -= len(data)
        return data


def exefs_from_ncz(stream, offset, size):
    stream.seek(offset + 0x4000)
    magic, count = struct.unpack('<8sQ', exact(stream, 16))
    if magic != b'NCZSECTN' or not 0 < count <= 65536 or 0x4010 + count * 64 >= size:
        raise ValueError('invalid NCZ section table')
    end, last = 0x4000, None
    for _ in range(count):
        section = exact(stream, 64)
        start, length = struct.unpack_from('<QQ', section)
        if start != end or length == 0:
            raise ValueError('noncontiguous NCZ sections are unsupported')
        end = start + length
        last = (start, length)
    start, length = last
    if length > 256 << 20:
        raise ValueError('final section is too large for ExeFS')
    compressed_start = stream.tell()
    if exact(stream, 4) != b'\x28\xb5\x2f\xfd':
        raise ValueError('only solid Zstandard NCZ is supported')
    stream.seek(compressed_start)
    with ZstdFile(LimitedReader(stream, offset + size - compressed_start), 'rb') as z:
        remaining = start - 0x4000
        while remaining:
            chunk = z.read(min(remaining, 4 << 20))
            if not chunk:
                raise ValueError('truncated NCZ stream')
            remaining -= len(chunk)
        tail = exact(z, length)
        if z.read(1):
            raise ValueError('NCZ decompressed size does not match section table')
    # shortcut: current dumps put ExeFS last; add header-based selection for other layouts.
    for pos in range(0, min(len(tail) - 15, 1 << 20), 16):
        if tail[pos:pos + 4] != b'PFS0':
            continue
        entries = pfs_entries(io.BytesIO(tail), pos, len(tail) - pos)
        if not {'main', 'sdk', 'main.npdm'} <= entries.keys():
            continue
        allowed = {'main', 'sdk', 'rtld', 'main.npdm'} | {f'subsdk{i}' for i in range(10)}
        if not entries.keys() <= allowed:
            raise ValueError('unexpected ExeFS file')
        return {name: tail[off:off + size] for name, (off, size) in entries.items()}
    raise ValueError('no ExeFS found in the final NCZ section')


def nso_info(data):
    import lz4.block
    if len(data) < 0x100 or data[:4] != b'NSO0':
        raise ValueError('invalid NSO header')
    flags = struct.unpack_from('<I', data, 0xc)[0]
    compressed = struct.unpack_from('<III', data, 0x60)
    segments = []
    for i in range(3):
        offset, memory, size = struct.unpack_from('<III', data, 0x10 + 16 * i)
        stored = compressed[i] if flags & (1 << i) else size
        if offset < 0x100 or offset + stored > len(data) or memory + size > 256 << 20:
            raise ValueError('invalid NSO segment bounds')
        raw = data[offset:offset + stored]
        if flags & (1 << i):
            raw = lz4.block.decompress(raw, uncompressed_size=size)
        if len(raw) != size:
            raise ValueError('NSO segment size mismatch')
        if flags & (8 << i) and hashlib.sha256(raw).digest() != data[0xa0 + 32 * i:0xc0 + 32 * i]:
            raise ValueError('NSO segment hash mismatch')
        segments.append({'offset': memory, 'size': size})
    if any(a['offset'] + a['size'] > b['offset'] for a, b in zip(segments, segments[1:])):
        raise ValueError('overlapping NSO segments')
    bss = struct.unpack_from('<I', data, 0x3c)[0]
    if segments[-1]['offset'] + segments[-1]['size'] + bss > 256 << 20:
        raise ValueError('NSO image too large')
    return {'build_id': data[0x40:0x60].hex(), 'segments': segments, 'bss': bss}


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def extract(package, output):
    stat = package.stat()
    identity = {'path': str(package.resolve()), 'size': stat.st_size, 'mtime_ns': stat.st_mtime_ns}
    manifest_path = output / 'manifest.json'
    if manifest_path.exists():
        old = json.loads(manifest_path.read_text())
        if old['source'] == identity and all(digest(output / n) == info['sha256'] for n, info in old['files'].items()):
            print('ExeFS already verified:', output)
            return
    if output.exists():
        raise ValueError('output exists and differs; use a new output directory')
    with package.open('rb') as stream:
        entries = pfs_entries(stream, 0, stat.st_size)
        candidates = [(n, off, size) for n, (off, size) in entries.items() if n.endswith('.ncz')]
        if len(candidates) != 1:
            raise ValueError('expected exactly one program NCZ')
        name, off, size = candidates[0]
        files = exefs_from_ncz(stream, off, size)
    manifest = {'source': identity, 'package_sha256': digest(package), 'ncz': name, 'files': {}}
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='extract-', dir=output.parent) as temp:
        staged = Path(temp) / 'exefs'
        staged.mkdir()
        for name, data in files.items():
            info = {} if name.endswith('.npdm') else nso_info(data)
            info.update(size=len(data), sha256=hashlib.sha256(data).hexdigest())
            manifest['files'][name] = info
            (staged / name).write_bytes(data)
        (staged / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        staged.rename(output)
    print('Verified ExeFS:', output)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('package', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    try:
        extract(args.package, args.output)
    except (ValueError, OSError) as error:
        parser.exit(1, f'{error}\n')
