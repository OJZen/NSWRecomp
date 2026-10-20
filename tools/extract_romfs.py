"""Restore solid NCZ program archives, then merge their RomFS with hactool.

Reuses extract_exefs.py container validation and d2r-mobile's --basenca workflow.
Keys remain local; temporary archives and the private title-key file are removed.
"""
import argparse
from compression.zstd import ZstdFile
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import stat
import subprocess
import sys
import tempfile

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from extract_exefs import exact, pfs_entries, LimitedReader, digest


def read_keys(path):
    info = path.stat()
    if not stat.S_ISREG(info.st_mode) or info.st_size > 1 << 20:
        raise ValueError('key file must be regular and at most 1 MiB')
    result = {}
    for line in path.read_text().splitlines():
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        if '=' not in line:
            raise ValueError('invalid key-file syntax')
        name, value = (x.strip().lower() for x in line.split('=', 1))
        if not re.fullmatch('[a-z0-9_]+', name) or not re.fullmatch('(?:[0-9a-f]{2})+', value):
            raise ValueError('invalid key-file entry')
        result[name] = bytes.fromhex(value)
    if len(result.get('header_key', b'')) != 32:
        raise ValueError('a 32-byte header_key is required')
    return result


def decrypt_header(raw, key):
    if len(raw) != 0xc00:
        raise ValueError('truncated NCA header')
    out = bytearray()
    for sector in range(6):
        cipher = Cipher(algorithms.AES(key), modes.XTS(sector.to_bytes(16, 'big'))).decryptor()
        out += cipher.update(raw[sector * 512:(sector + 1) * 512]) + cipher.finalize()
    if out[0x200:0x204] != b'NCA3':
        raise ValueError('header_key did not decrypt an NCA3 header')
    return out


def inspect_package(path, keys, update):
    with path.open('rb') as stream:
        entries = pfs_entries(stream, 0, path.stat().st_size)
        candidates = [(name, *bounds) for name, bounds in entries.items() if name.endswith('.ncz')]
        if len(candidates) != 1:
            raise ValueError('expected exactly one program NCZ per package')
        name, offset, length = candidates[0]
        if not re.fullmatch(r'[0-9a-f]{32}\.ncz', name):
            raise ValueError('NCZ must have a content-hash filename')
        stream.seek(offset)
        raw = exact(stream, 0x4000)
        header = decrypt_header(raw[:0xc00], keys['header_key'])
        nca_size, title = struct.unpack_from('<QQ', header, 0x208)
        if header[0x205] != 0 or title != 0x0100f2c0115b6000:
            raise ValueError('package does not contain the expected Zelda program')
        generation = max(0, max(header[0x206], header[0x220]) - 1)
        titlekek = keys.get(f'titlekek_{generation:02x}', b'')
        if len(titlekek) != 16:
            raise ValueError(f'missing titlekek generation {generation:02x}')
        romfs_index, romfs_size, metadata_hash = None, None, None
        for index in range(4):
            start, end = struct.unpack_from('<II', header, 0x240 + 16 * index)
            if not start:
                continue
            if start >= end or end * 512 > nca_size:
                raise ValueError('invalid NCA section bounds')
            fs = header[0x400 + 512 * index:0x600 + 512 * index]
            if hashlib.sha256(fs).digest() != header[0x280 + 32 * index:0x2a0 + 32 * index]:
                raise ValueError('NCA filesystem-header hash mismatch')
            if fs[2] == 0 and fs[3] == 3:
                if romfs_index is not None or fs[4] != (4 if update else 3) or fs[8:12] != b'IVFC' or fs[5] not in (0, 1):
                    raise ValueError('unsupported RomFS/update layout')
                romfs_index = index
                metadata_hash = fs[5]
                romfs_size = struct.unpack_from('<Q', fs, 8 + 16 + 5 * 24 + 8)[0]
        if romfs_index is None or not romfs_size or romfs_size > 64 << 30:
            raise ValueError('invalid RomFS size')
        magic, count = struct.unpack('<8sQ', exact(stream, 16))
        if magic != b'NCZSECTN' or not 0 < count <= 65536 or 0x4010 + count * 64 >= length:
            raise ValueError('invalid NCZ section table')
        sections, end, content_key = [], 0x4000, None
        for _ in range(count):
            start, size, crypto, reserved, key, counter = struct.unpack('<QQQQ16s16s', exact(stream, 64))
            if start != end or start % 16 or not size or size > nca_size - start or crypto not in (1, 3, 4) or reserved:
                raise ValueError('unsupported NCZ section layout')
            if crypto in (3, 4):
                if content_key is not None and content_key != key:
                    raise ValueError('multiple content keys in one program archive')
                content_key = key
            sections.append((start, size, crypto, key, counter))
            end = start + size
        if end != nca_size or content_key is None or not any(header[0x230:0x240]):
            raise ValueError('unsupported NCZ program encryption layout')
        compressed = stream.tell()
        if exact(stream, 4) != b'\x28\xb5\x2f\xfd':
            raise ValueError('only solid Zstandard NCZ is supported')
        # Recreate the title-key mapping from the key already embedded in this NCZ.
        cipher = Cipher(algorithms.AES(titlekek), modes.ECB()).encryptor()
        encrypted_key = cipher.update(content_key) + cipher.finalize()
        return dict(path=path, raw=raw, offset=offset, length=length, compressed=compressed,
                    sections=sections, nca_size=nca_size, name=name, generation=generation,
                    romfs_index=romfs_index, romfs_size=romfs_size, metadata_hash=metadata_hash,
                    rights=bytes(header[0x230:0x240]), encrypted_key=encrypted_key)


def restore_ncz(plan, output):
    sha = hashlib.sha256(plan['raw'])
    with plan['path'].open('rb') as stream, output.open('xb') as target:
        stream.seek(plan['compressed'])
        target.write(plan['raw'])
        remaining = plan['offset'] + plan['length'] - plan['compressed']
        with ZstdFile(LimitedReader(stream, remaining), 'rb') as z:
            reported = 0
            for start, size, crypto, key, counter in plan['sections']:
                cipher = None
                if crypto in (3, 4):
                    iv = counter[:8] + (start // 16).to_bytes(8, 'big')
                    cipher = Cipher(algorithms.AES(key), modes.CTR(iv)).encryptor()
                consumed = 0
                while consumed < size:
                    data = exact(z, min(4 << 20, size - consumed))
                    consumed += len(data)
                    if cipher:
                        data = cipher.update(data)
                    target.write(data); sha.update(data)
                    progress = (start + consumed) >> 30
                    if progress > reported:
                        print(f'  restored {progress} GiB', flush=True); reported = progress
                if cipher and cipher.finalize():
                    raise ValueError('unexpected cipher tail')
            if z.read(1):
                raise ValueError('NCZ decompressed length mismatch')
        if target.tell() != plan['nca_size'] or not sha.hexdigest().startswith(plan['name'][:32]):
            raise ValueError('restored NCA content hash mismatch')
    return sha.hexdigest()


def hactool_key_subset(keys, plans):
    # Unrelated console keys are unnecessary and may use formats hactool rejects.
    names = {'header_key'} | {f'titlekek_{p["generation"]:02x}' for p in plans}
    return {name: keys[name] for name in sorted(names)}


def extract(base, update, key_path, hactool, output, expected):
    if output.exists() or output.with_suffix('.json').exists():
        raise ValueError('output already exists; refusing to overwrite')
    keys = read_keys(key_path)
    plans = [inspect_package(base, keys, False), inspect_package(update, keys, True)]
    work = ROOT / 'work'
    needed = sum(p['nca_size'] for p in plans) + plans[1]['romfs_size'] + (20 << 30)
    if shutil.disk_usage(work).free < needed:
        raise ValueError('not enough space for temporary archives and merged RomFS')
    with tempfile.TemporaryDirectory(prefix='romfs-', dir=work / 'tmp') as temporary:
        temp = Path(temporary)
        titlefile = temp / 'title.keys'
        prodfile = temp / 'prod.keys'
        titlekeys = {p['rights'].hex(): p['encrypted_key'] for p in plans}
        for path, entries in ((prodfile, hactool_key_subset(keys, plans)), (titlefile, titlekeys)):
            with os.fdopen(os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), 'w') as f:
                for name, value in entries.items():
                    f.write(name + ' = ' + value.hex() + '\n')
        archive_hashes = []
        for label, plan in zip(('base', 'update'), plans):
            print(f'Restoring {label} program archive', flush=True)
            archive_hashes.append(restore_ncz(plan, temp / (label + '.nca')))
        merged = temp / 'romfs.bin'
        print('Verifying and merging RomFS with hactool', flush=True)
        command = [str(hactool.resolve()), '-k', str(prodfile), '--titlekeysfile=' + str(titlefile),
                   '--disablekeywarns', '--suppresskeys', '--verify', '--basenca=' + str(temp / 'base.nca'),
                   f'--section{plans[1]["romfs_index"]}=' + str(merged),
                   '--exefsdir=' + str(temp / 'exefs'), str(temp / 'update.nca')]
        # hactool can print encrypted key material too; never persist its raw output.
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        failed = result.returncode or re.search(r'\(FAIL\)|(?:^|\n)(?:Error:|Failed |Invalid )|corrupted', result.stdout)
        checks = [f'Level {i} (GOOD)' for i in range(6)]
        if plans[1]['metadata_hash']: checks.append('BKTR Metadata Hashes (GOOD)')
        failed = failed or any(check not in result.stdout for check in checks)
        if failed or not merged.is_file() or merged.stat().st_size != plans[1]['romfs_size']:
            print('Extraction failed verification; diagnostic output withheld to protect keys', flush=True)
            raise ValueError('hactool verification/extraction failed; raw output withheld to protect keys')
        if not all((temp / 'exefs' / n).is_file() and digest(temp / 'exefs' / n) == expected[n]['sha256']
                   for n in ('rtld', 'main', 'subsdk0', 'sdk')):
            raise ValueError('merged archive ExeFS does not match the prepared v1.4.3 modules')
        with merged.open('rb') as f:
            if struct.unpack('<Q', exact(f, 8))[0] != 0x50:
                raise ValueError('extracted output is not raw RomFS')
        print('Hashing merged RomFS', flush=True)
        record = {'version': '1.4.3', 'size': merged.stat().st_size, 'sha256': digest(merged),
                  'nca_sha256': dict(zip(('base', 'update'), archive_hashes)),
                  'hactool_sha256': digest(hactool), 'hactool_hash_checks': 'passed',
                  'exefs_sha256': {n: expected[n]['sha256'] for n in ('rtld', 'main', 'subsdk0', 'sdk')}}
        output.parent.mkdir(parents=True, exist_ok=True)
        # A hard link fails if another producer created the destination during extraction.
        os.link(merged, output)
        output.with_suffix('.json').write_text(json.dumps(record, indent=2) + '\n')
    print('Verified merged RomFS ready; temporary archives removed', flush=True)

