"""Validate a user-supplied, unmodified merged RomFS without copying its contents."""
try:
    import fcntl
except ImportError:
    fcntl = None
import hashlib
from contextlib import contextmanager
import json
import os
from pathlib import Path
import stat
import re

from build import write_json
from recompile import ROOT

PROFILE = ROOT / 'config/romfs-1.4.3.json'


def stamp(value):
    # Windows ctime is deprecated and can differ between stat and fstat (birth
    # versus metadata-change time). Use the explicit birth time on that host.
    identity_time = value.st_birthtime_ns if os.name == 'nt' else value.st_ctime_ns
    return [value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns, identity_time]


def stream_hash(stream):
    if hasattr(fcntl, 'F_NOCACHE'):
        fcntl.fcntl(stream.fileno(), fcntl.F_NOCACHE, 1)
    return hashlib.file_digest(stream, 'sha256').hexdigest()


def resource_reader(path):
    if os.name != 'nt':
        return path.open('rb')
    import ctypes as C
    from ctypes import wintypes as W
    import msvcrt
    library = C.WinDLL('kernel32', use_last_error=True)
    create = library.CreateFileW
    create.argtypes = [W.LPCWSTR, W.DWORD, W.DWORD, C.c_void_p, W.DWORD, W.DWORD, W.HANDLE]
    create.restype = W.HANDLE
    close = library.CloseHandle
    close.argtypes = [W.HANDLE]
    close.restype = W.BOOL
    # Share reads only: concurrent writers, writable mappings and replacement
    # must be excluded for the entire hash/identity check, including cache checks.
    handle = create(str(path), 0x80000000, 1, None, 3, 0x08000000, None)
    if handle == C.c_void_p(-1).value:
        raise C.WinError(C.get_last_error())
    try:
        fd = msvcrt.open_osfhandle(handle, os.O_RDONLY | os.O_BINARY)
    except BaseException:
        close(handle)
        raise
    try:
        return os.fdopen(fd, 'rb')
    except BaseException:
        os.close(fd)
        raise


@contextmanager
def verified_romfs(path, profile=None):
    """Keep the verified stream alive until the native consumer has stopped."""
    path = Path(path).resolve(strict=True)
    expected = json.loads(Path(profile or PROFILE).read_text(encoding='utf-8'))
    if (not isinstance(expected, dict) or type(expected.get('size')) is not int
            or not 0x50 <= expected['size'] <= 64 << 30
            or not isinstance(expected.get('sha256'), str)
            or not re.fullmatch('[0-9a-f]{64}', expected['sha256'])):
        raise ValueError('invalid RomFS profile: expected size and lowercase SHA-256')
    output = ROOT / 'work/resources'
    if output.parent.is_symlink() or output.is_symlink(): raise ValueError('resource cache must not be a symlink')
    output.mkdir(parents=True, exist_ok=True)
    cache = output / 'verified.json'
    if cache.is_symlink() or cache.with_suffix('.json.partial').is_symlink():
        raise ValueError('resource cache file must not be a symlink')
    previous = json.loads(cache.read_text()) if cache.exists() else {}
    if previous and previous.get('format') != 'totk-romfs-v1':
        raise ValueError('unrecognized resource cache; refusing to overwrite')
    with resource_reader(path) as stream:
        before = os.fstat(stream.fileno())
        if not stat.S_ISREG(before.st_mode) or before.st_size != expected['size']:
            raise ValueError('RomFS size does not match the selected resource profile')
        record = {'format': 'totk-romfs-v1', 'path': str(path), 'stat': stamp(before), 'profile': expected}
        # Rapid same-size writes can retain the same Windows metadata stamp.
        # A fresh streaming hash under the read lock is required on that host.
        cached = os.name != 'nt' and previous == record
        if not cached and stream_hash(stream) != expected['sha256']:
            raise ValueError('RomFS fingerprint does not match the selected resource profile')
        if stamp(before) != stamp(os.fstat(stream.fileno())) or stamp(before) != stamp(path.stat()):
            raise ValueError('RomFS changed during verification')
        if not cached: write_json(cache, record)
        yield {**expected, 'path': str(path), 'cached': cached}, stream


def verify_romfs(path, profile=None):
    with verified_romfs(path, profile) as (resource, _):
        return resource


@contextmanager
def resource_child_options(stream, environment):
    """Inherit only the verified read handle, without reopening a mutable path."""
    environment.pop('NSW_ROMFS_HANDLE', None)
    if os.name != 'nt' or stream is None:
        yield {}
        return
    import msvcrt
    import subprocess
    handle = msvcrt.get_osfhandle(stream.fileno())
    previous = os.get_handle_inheritable(handle)
    info = subprocess.STARTUPINFO()
    info.lpAttributeList = {'handle_list': [handle]}
    try:
        os.set_handle_inheritable(handle, True)
        environment['NSW_ROMFS_HANDLE'] = str(handle)
        yield {'startupinfo': info, 'close_fds': True}
    finally:
        os.set_handle_inheritable(handle, previous)
        environment.pop('NSW_ROMFS_HANDLE', None)
