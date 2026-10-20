"""Prepare a version-locked local game from user-supplied solid NSZ packages and keys."""
import argparse
import fcntl
import json
from pathlib import Path
import stat
import tempfile
import resource
import signal

from build import write_json
from extract_exefs import extract as extract_exefs
from extract_romfs import extract as extract_romfs, inspect_package, read_keys
from recompile import ROOT, digest, prepare_work, profile, verify
from resources import verify_romfs


def identity(path):
    value = path.stat()
    if not stat.S_ISREG(value.st_mode): raise ValueError('input must be a regular file')
    return dict(path=str(path.resolve()), device=value.st_dev, inode=value.st_ino, size=value.st_size,
                mtime_ns=value.st_mtime_ns, ctime_ns=value.st_ctime_ns)


def tool():
    path = ROOT / 'work/tools/hactool'
    receipt = json.loads(path.with_suffix('.json').read_text())
    if (receipt.get('commit') != '1d64a83450e025622f3468c28fc4164dad2c5ef6'
            or receipt.get('patch_sha256') != digest(ROOT / 'tools/hactool.patch')
            or receipt.get('binary_sha256') != digest(path)):
        raise ValueError('hactool does not match its pinned source; run make import-tools')
    return path


def activate(output):
    config = ROOT / 'local/inputs.json'
    if config.is_symlink(): raise ValueError('local input config must not be a symlink')
    value = json.loads(config.read_text()) if config.exists() else {}
    value.update(exefs=str(output / 'exefs'), romfs=str(output / 'romfs.bin'))
    write_json(config, value)


def import_game(base, update, keys):
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    prepare_work()
    local = ROOT / 'local'
    if local.is_symlink(): raise ValueError('local must not be a symlink')
    local.mkdir(exist_ok=True)
    if (local / 'import.lock').is_symlink(): raise ValueError('import lock must not be a symlink')
    with (local / 'import.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        sources = {'base': identity(base), 'update': identity(update)}
        identity(keys)
        # Check both headers without printing or retaining key contents.
        key_data = read_keys(keys)
        inspect_package(base, key_data, False)
        inspect_package(update, key_data, True)
        del key_data
        output = local / 'game'
        if output.is_symlink(): raise ValueError('game output must not be a symlink')
        if output.exists():
            old = json.loads((output / 'import.json').read_text())
            if old.get('format') != 'totk-user-game-v1' or old.get('sources') != sources:
                raise ValueError('local/game already exists and belongs to different inputs; it was preserved')
            verify(output / 'exefs'); verify_romfs(output / 'romfs.bin')
            activate(output)
            return dict(status='already_verified', directory=str(output), version=profile()['version'])
        hactool = tool()
        with tempfile.TemporaryDirectory(prefix='import-game-', dir=ROOT / 'work/tmp') as temp:
            staged = Path(temp) / 'game'; staged.mkdir()
            extract_exefs(update, staged / 'exefs')
            expected = verify(staged / 'exefs')
            extract_romfs(base, update, keys, hactool, staged / 'romfs.bin', expected)
            record = json.loads((staged / 'romfs.json').read_text())
            expected_romfs = json.loads((ROOT / 'config/romfs-1.4.3.json').read_text())
            if any(record[k] != expected_romfs[k] for k in ('size', 'sha256')):
                raise ValueError('merged resources do not match the supported game version')
            if sources != {'base': identity(base), 'update': identity(update)}:
                raise ValueError('input packages changed during import')
            write_json(staged / 'import.json', dict(format='totk-user-game-v1', version=profile()['version'],
                       sources=sources, romfs=record, keys_retained=False))
            if output.exists(): raise ValueError('output appeared during import; it was preserved')
            staged.rename(output)
        activate(output)
        return dict(status='imported_and_verified', directory=str(output), version=profile()['version'])


if __name__ == '__main__':
    def interrupted(number, frame):
        raise SystemExit(128 + number)
    signal.signal(signal.SIGTERM, interrupted)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', required=True, type=Path)
    parser.add_argument('--update', required=True, type=Path)
    parser.add_argument('--keys', required=True, type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(import_game(args.base.resolve(), args.update.resolve(), args.keys.resolve()), indent=2))
    except (ValueError, KeyError, OSError) as error:
        parser.exit(1, f'{error}\n')
