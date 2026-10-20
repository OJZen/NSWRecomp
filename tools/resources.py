"""Validate a user-supplied, unmodified merged RomFS without copying its contents."""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import stat

from build import write_json
from recompile import ROOT

PROFILE = ROOT / 'config/romfs-1.4.3.json'


def stamp(value):
    return [value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns, value.st_ctime_ns]


def stream_hash(stream):
    if hasattr(fcntl, 'F_NOCACHE'):
        fcntl.fcntl(stream.fileno(), fcntl.F_NOCACHE, 1)
    return hashlib.file_digest(stream, 'sha256').hexdigest()


def verify_romfs(path):
    path = Path(path).resolve(strict=True)
    expected = json.loads(PROFILE.read_text())
    output = ROOT / 'work/resources'
    if output.parent.is_symlink() or output.is_symlink(): raise ValueError('resource cache must not be a symlink')
    output.mkdir(parents=True, exist_ok=True)
    cache = output / 'verified.json'
    if cache.is_symlink() or cache.with_suffix('.json.partial').is_symlink():
        raise ValueError('resource cache file must not be a symlink')
    previous = json.loads(cache.read_text()) if cache.exists() else {}
    if previous and previous.get('format') != 'totk-romfs-v1':
        raise ValueError('unrecognized resource cache; refusing to overwrite')
    with path.open('rb') as stream:
        before = os.fstat(stream.fileno())
        if not stat.S_ISREG(before.st_mode) or before.st_size != expected['size']:
            raise ValueError('expected an unmodified merged v1.4.3 RomFS')
        record = {'format': 'totk-romfs-v1', 'path': str(path), 'stat': stamp(before), 'profile': expected}
        cached = previous == record
        if not cached and stream_hash(stream) != expected['sha256']:
            raise ValueError('RomFS fingerprint does not match v1.4.3')
        if stamp(before) != stamp(os.fstat(stream.fileno())) or stamp(before) != stamp(path.stat()):
            raise ValueError('RomFS changed during verification')
    if not cached: write_json(cache, record)
    return {'path': str(path), **expected, 'cached': cached}
