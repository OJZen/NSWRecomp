"""Build pinned Xiph Ogg/Vorbis sources with LLVM MinGW for local video playback."""
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import ssl
import subprocess
import sys
import tarfile
import time
import urllib.request

from bounded import run
from recompile import ROOT, digest, prepare_work

PACKAGES = {'ogg': ('1.3.6', '5c8253428e181840cd20d41f3ca16557a9cc04bad4a3d04cce84808677fa1061'),
            'vorbis': ('1.3.7', 'b33cc4934322bcbf6efcbacf49e3ca01aadbea4114ec9589d1b1e9d20f72954b')}
BUILD = ROOT / 'work/build/vorbis-win'


def source(kind): return ROOT / f'work/tools/lib{kind}-{PACKAGES[kind][0]}'


def verified_dependency():
    path = BUILD / 'receipt.json'
    if not path.is_file(): raise ValueError('run tools/prepare_vulkan_video.py to prepare Vorbis')
    record = json.loads(path.read_text())
    if record.get('archives') != {k: sha for k, (_, sha) in PACKAGES.items()}:
        raise ValueError('unexpected Vorbis dependency identity')
    for name, sha in record['files'].items():
        p = (ROOT / name).resolve()
        if not p.is_relative_to((ROOT / 'work').resolve()) or digest(p) != sha: raise ValueError('Vorbis dependency file mismatch')
    for kind in PACKAGES:
        if (BUILD / f'lib{kind}.a').relative_to(ROOT).as_posix() not in record['files']: raise ValueError('missing Vorbis library identity')
    return record, ['-I', str(source('vorbis') / 'include'), '-I', str(source('ogg') / 'include'),
                    str(BUILD / 'libvorbis.a'), str(BUILD / 'libogg.a')]


def worker():
    groups = {'ogg': ['bitwise', 'framing'], 'vorbis':
              'mdct smallft block envelope window lsp lpc analysis synthesis psy info floor1 floor0 res0 mapping0 registry codebook sharedbook lookup bitrate vorbisenc'.split()}
    includes = ['-I', str(source('ogg') / 'include'), '-I', str(source('vorbis') / 'include'), '-I', str(source('vorbis') / 'lib')]
    def compile(item):
        kind, name = item; directory = BUILD / kind; directory.mkdir(exist_ok=True)
        obj = directory / (name + '.o')
        result = subprocess.run(['clang', '-std=gnu11', '-O2', *includes, '-c', str(source(kind) / ('src' if kind == 'ogg' else 'lib') / (name + '.c')),
                                 '-o', str(obj)], capture_output=True, text=True, timeout=60)
        if result.returncode: raise RuntimeError(result.stderr)
        return kind, obj
    with ThreadPoolExecutor(8) as pool:
        compiled = list(pool.map(compile, [(k, n) for k, names in groups.items() for n in names]))
    for kind in groups:
        subprocess.run(['llvm-ar', 'rcs', str(BUILD / f'lib{kind}.a'), *[str(obj) for k, obj in compiled if k == kind]], check=True, timeout=30)


def prepare():
    if os.name != 'nt': raise ValueError('this bootstrap targets Windows')
    prepare_work()
    context = ssl.create_default_context()
    bundle = Path(os.environ.get('ProgramFiles', 'C:/Program Files')) / 'Git/mingw64/etc/ssl/certs/ca-bundle.crt'
    if bundle.is_file(): context.load_verify_locations(str(bundle))
    for kind, (version, sha) in PACKAGES.items():
        archive = ROOT / f'work/tmp/lib{kind}-{version}.tar.xz'
        if archive.is_symlink() or source(kind).is_symlink() or BUILD.is_symlink(): raise ValueError('dependency output symlink')
        if not archive.is_file() or digest(archive) != sha:
            total, deadline = 0, time.monotonic() + 300
            with urllib.request.urlopen(f'https://downloads.xiph.org/releases/{kind}/{archive.name}', timeout=20, context=context) as response, archive.open('wb') as output:
                while chunk := response.read(65536):
                    total += len(chunk)
                    if total > 8 << 20 or time.monotonic() > deadline: raise RuntimeError('Vorbis download budget')
                    output.write(chunk)
            if digest(archive) != sha: raise ValueError('Xiph archive checksum mismatch')
        with tarfile.open(archive) as package:
            if sum(m.size for m in package.getmembers()) > 64 << 20: raise ValueError('Xiph expanded size bound')
            if not source(kind).exists(): package.extractall(source(kind).parent, filter='data')
            else:
                for member in package.getmembers():
                    if not member.isfile(): continue
                    p = (source(kind).parent / member.name).resolve()
                    if not p.is_relative_to(source(kind).resolve()) or digest(p) != hashlib.sha256(package.extractfile(member).read()).hexdigest():
                        raise ValueError('Xiph source differs from pinned archive')
    BUILD.mkdir(parents=True, exist_ok=True)
    metrics = run([sys.executable, str(Path(__file__).resolve()), '--worker'], cwd=ROOT, log=ROOT / 'work/logs/vorbis-build.log', seconds=180, rss_limit=4 << 30)
    files = [BUILD / f'lib{k}.a' for k in PACKAGES]
    for k in PACKAGES: files += [*sorted((source(k) / 'include').rglob('*.h')), source(k) / 'COPYING']
    record = dict(archives={k: sha for k, (_, sha) in PACKAGES.items()}, checksum_source='https://xiph.org/downloads/', build=metrics,
                  compiler=subprocess.check_output(['clang', '--version'], text=True, timeout=10),
                  builder_sha256=digest(Path(__file__)), files={p.relative_to(ROOT).as_posix(): digest(p) for p in files})
    (BUILD / 'receipt.json').write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
    return record


if __name__ == '__main__':
    if sys.argv[1:] == ['--worker']: worker()
    else: print(json.dumps(prepare(), indent=2))
