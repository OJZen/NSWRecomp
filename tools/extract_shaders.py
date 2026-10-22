"""Extract validated static shader archives into a new, isolated directory under work/."""
import argparse
from collections import Counter
from compression.zstd import ZstdDecompressor, ZstdDict, get_frame_info
from functools import cache
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import struct
import tempfile

from build import write_json
from index_shaders import archive, index, span
from resources import ROOT, stamp, verify_romfs
from recompile import Nso, verify

MAX_FILE = 128 << 20
SHADER_MAGIC = (b'BNSH', b'FSHA', b'BAHS')


def safe_name(raw):
    name = raw.decode('utf-8')
    parts = name.split('/')
    if not name or '\\' in name or '\0' in name or any(p in ('', '.', '..') for p in parts):
        raise ValueError('unsafe archive name')
    return name


def romfs_entries(stream):
    size = os.fstat(stream.fileno()).st_size
    header = struct.unpack('<10Q', span(stream.read(80), 0, 80))
    if header[0] != 80 or header[9] > size:
        raise ValueError('invalid RomFS header')
    tables = []
    for offset, length in ((header[3], header[4]), (header[7], header[8])):
        if length > 64 << 20 or offset < 80 or offset + length > size:
            raise ValueError('invalid RomFS metadata range')
        stream.seek(offset); tables.append(span(stream.read(length), 0, length))
    directories, files = tables

    @cache
    def directory(offset, ancestors=()):
        if offset in ancestors or len(ancestors) >= 64:
            raise ValueError('RomFS directory cycle or excessive depth')
        parent, _, _, _, _, length = struct.unpack('<6I', span(directories, offset, 24))
        raw = span(directories, offset + 24, length)
        if offset == 0:
            if raw: raise ValueError('invalid RomFS root name')
            return ''
        name = safe_name(raw)
        if '/' in name: raise ValueError('invalid RomFS directory name')
        return directory(parent, ancestors + (offset,)) + '/' + name

    offset = 0
    while offset < len(files):
        parent, _, data, length, _, namesize = struct.unpack('<IIQQII', span(files, offset, 32))
        name = safe_name(span(files, offset + 32, namesize))
        if '/' in name or header[9] + data + length > size:
            raise ValueError('invalid RomFS file range or name')
        yield directory(parent) + '/' + name, header[9] + data, length
        offset = (offset + 32 + namesize + 3) & ~3
    if offset != len(files): raise ValueError('invalid RomFS file table padding')


def sarc_entries(data):
    if data[:4] != b'SARC': raise ValueError('expected SARC')
    endian = {b'\xff\xfe': '<', b'\xfe\xff': '>'}.get(span(data, 6, 2))
    if not endian: raise ValueError('invalid SARC byte order')
    header, _, size, payload, version, reserved = struct.unpack(endian + 'HHIIHH', span(data, 4, 16))
    if header != 20 or size != len(data) or version != 0x100 or reserved or payload > size:
        raise ValueError('invalid SARC header')
    magic, fat_size, count, _ = struct.unpack(endian + '4sHHI', span(data, header, 12))
    if magic != b'SFAT' or fat_size != 12: raise ValueError('invalid SARC file table')
    strings = header + fat_size + count * 16
    magic, name_size, reserved = struct.unpack(endian + '4sHH', span(data, strings, 8))
    if magic != b'SFNT' or name_size != 8 or reserved or strings + 8 > payload:
        raise ValueError('invalid SARC name table')
    seen = set()
    for i in range(count):
        hashed, attr, start, end = struct.unpack(endian + '4I', span(data, header + fat_size + i * 16, 16))
        if attr >> 24 == 1:
            name_at = strings + 8 + (attr & 0xffffff) * 4
            if name_at >= payload: raise ValueError('SARC name outside name table')
            stop = data.find(b'\0', name_at, payload)
            if stop < 0: raise ValueError('unterminated SARC name')
            name = safe_name(data[name_at:stop])
        elif attr == 0:
            name = f'{hashed:08x}.bin'
        else:
            raise ValueError('unsupported SARC name attributes')
        if name in seen or start > end or payload + end > len(data):
            raise ValueError('duplicate SARC name or invalid payload range')
        seen.add(name)
        yield name, span(data, payload + start, end - start)


def decompress(data, dictionaries):
    info = get_frame_info(data)
    if info.decompressed_size is None or info.decompressed_size > MAX_FILE:
        raise ValueError('unknown or excessive decompressed size')
    if info.dictionary_id and info.dictionary_id not in dictionaries:
        raise ValueError(f'missing Zstandard dictionary {info.dictionary_id}')
    decoder = ZstdDecompressor(zstd_dict=dictionaries.get(info.dictionary_id))
    result = decoder.decompress(data, max_length=info.decompressed_size + 1)
    if not decoder.eof or decoder.unused_data or len(result) != info.decompressed_size:
        raise ValueError('truncated, concatenated or oversized Zstandard frame')
    return result


def embedded_archives(image):
    for magic in SHADER_MAGIC:
        offset = 0
        while (offset := image.find(magic, offset)) >= 0:
            at = offset; offset += 4
            if at + 32 > len(image): continue
            size = struct.unpack_from('<I', image, at + (8 if magic == b'BAHS' else 28))[0]
            if not 32 <= size <= min(MAX_FILE, len(image) - at): continue
            data = bytes(image[at:at + size])
            try:
                count = sum(1 for _ in archive(data))
            except (ValueError, struct.error):
                continue
            if count: yield at, data


def extract(romfs, output, exefs=None):
    verified = verify_romfs(romfs)
    output = Path(os.path.abspath(output))
    work = ROOT / 'work'
    if not output.is_relative_to(work) or output == work or any(p.is_symlink() for p in (output, *output.parents)):
        raise ValueError('output must be a new directory under work without symlinks')
    if output.exists(): raise ValueError('output already exists; refusing to overwrite')
    output.parent.mkdir(parents=True, exist_ok=True)
    with Path(verified['path']).open('rb') as stream, tempfile.TemporaryDirectory(prefix='shader-extract-', dir=work / 'tmp') as temp:
        before = stamp(os.fstat(stream.fileno()))
        entries = {name: (offset, size) for name, offset, size in romfs_entries(stream)
                   if name.startswith(('/Shader/', '/Lib/', '/UI/LayoutArchive/')) or name == '/Pack/ZsDic.pack.zs' or
                   (name.startswith('/Effect/') and name.endswith('.esetb.byml.zs'))}

        def read_file(name):
            offset, size = entries[name]
            if size > MAX_FILE: raise ValueError('shader archive exceeds extraction limit')
            stream.seek(offset)
            return span(stream.read(size), 0, size)

        dictionaries = {}
        dictionary_pack = '/Pack/ZsDic.pack.zs'
        if dictionary_pack in entries:
            for _, raw in sarc_entries(decompress(read_file(dictionary_pack), {})):
                dictionary = ZstdDict(raw)
                if not dictionary.dict_id or dictionary.dict_id in dictionaries:
                    raise ValueError('invalid or duplicate Zstandard dictionary')
                dictionaries[dictionary.dict_id] = dictionary
        root = Path(temp); source = root / 'source'; source.mkdir()
        packages = []; ignored = []; archives = {}; total_bytes = 0

        def unpack(data, origin, depth=0):
            nonlocal total_bytes
            if depth > 4: raise ValueError('excessive nested shader archive depth')
            if data[:4] == b'\x28\xb5\x2f\xfd': data = decompress(data, dictionaries)
            if data[:4] == b'SARC':
                for name, raw in sarc_entries(data): unpack(raw, origin + ':' + name, depth + 1)
            elif data[:4] in SHADER_MAGIC:
                content_hash = hashlib.sha256(data).hexdigest()
                suffix = {b'BNSH': '.bnsh', b'FSHA': '.bfsha', b'BAHS': '.sharcb'}[data[:4]]
                name = archives.get(content_hash, content_hash + suffix)
                stages = Counter(); keys = set(); count = 0
                for record, _ in archive(data):
                    stages[record['stage']] += 1; keys.add(record['key']); count += 1
                if content_hash not in archives:
                    total_bytes += len(data)
                    if total_bytes > 512 << 20: raise ValueError('extracted shaders exceed aggregate limit')
                    (source / name).write_bytes(data); archives[content_hash] = name
                packages.append(dict(origin=origin, file=name, size=len(data), sha256=content_hash,
                                     records=count, stages=dict(stages), keys=sorted(keys)))
            elif origin.startswith('/Effect/') and origin.endswith('.esetb.byml.zs'):
                found = False
                for offset, payload in embedded_archives(data):
                    unpack(payload, f'{origin}+{offset:#x}', depth + 1); found = True
                if not found: ignored.append(origin)
            else:
                if PurePosixPath(origin).suffix in ('.bnsh', '.bfsha', '.sharcb'):
                    raise ValueError('unrecognized shader archive')
                ignored.append(origin)

        for name in sorted(entries):
            if name.endswith(('.bfsha.zs', '.bnsh', '.sharcb', '.sarc.zs', '.bfsha', '.esetb.byml.zs', '.blarc.zs')):
                unpack(read_file(name), name)
        modules = verify(Path(exefs)) if exefs else {}
        for name in modules:
            module = Nso(Path(exefs) / name)
            for offset, data in embedded_archives(module.image): unpack(data, f'@ExeFS/{name}+{offset:#x}')
            del module
        if not packages: raise ValueError('no supported shader archives extracted')
        index(source, root / 'code')
        records = json.loads((root / 'index.json').read_text())
        current = ROOT / 'work/shaders/index.json'
        captured = json.loads(current.read_text()) if current.exists() else []
        by_key = {e['key']: e for e in records}
        matches = []
        for e in captured:
            if e['key'] in by_key:
                if any(e[k] != by_key[e['key']][k] for k in ('stage', 'code_sha256', 'c1')):
                    raise ValueError('archive and captured shader identities disagree')
                matches.append(e['key'])
        report = dict(format='totk-static-shaders-v1', romfs=verified, exefs=modules, packages=packages, ignored_members=ignored,
                      unique_archives=len(archives),
                      unique_shaders=len(records), stages=dict(Counter(e['stage'] for e in records)),
                      captured_shaders=len(captured), captured_matches=len(matches),
                      captured_missing=sorted({e['key'] for e in captured} - set(matches)),
                      extracted_bytes=total_bytes, compiled=False, gameplay_verified=False)
        write_json(root / 'extraction.json', report)
        if before != stamp(os.fstat(stream.fileno())) or before != stamp(Path(verified['path']).stat()):
            raise ValueError('RomFS changed during extraction')
        root.rename(output)
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--romfs', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=ROOT / 'work/shader-preparation-v1/extracted')
    parser.add_argument('--exefs', type=Path, help='also inspect the verified modules for embedded shader archives')
    args = parser.parse_args()
    report = extract(args.romfs, args.output, args.exefs)
    print(json.dumps({k: report[k] for k in ('unique_shaders', 'stages', 'captured_shaders', 'captured_matches', 'extracted_bytes')}))
