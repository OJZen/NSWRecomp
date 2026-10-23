"""Prepare pinned libvpx and Vorbis builds for optional Windows movie decoding."""
import hashlib
import ctypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import time
import urllib.request

from bounded import run
from recompile import ROOT, digest, prepare_work

VERSION = '1.16.0'
URL = f'https://storage.googleapis.com/downloads.webmproject.org/releases/webm/libvpx-{VERSION}.tar.gz'
SHA256 = 'b19c48b6384c5f9352d4c861a9659e3e7041918aad23da63e84559674816adac'
SOURCE = ROOT / f'work/tools/libvpx-{VERSION}'
BUILD = ROOT / 'work/build/libvpx-win'
RECEIPT = BUILD / 'receipt.json'


def verified_dependency():
    if not RECEIPT.is_file(): raise ValueError('run tools/prepare_vulkan_video.py before enabling Vulkan video')
    record = json.loads(RECEIPT.read_text(encoding='utf-8'))
    if record.get('archive_sha256') != SHA256 or record.get('version') != VERSION:
        raise ValueError('unexpected Windows libvpx dependency')
    for name, sha in record['files'].items():
        path = (ROOT / name).resolve()
        if not path.is_relative_to((ROOT / 'work').resolve()) or digest(path) != sha:
            raise ValueError('Windows libvpx file identity mismatch')
    if f'work/build/libvpx-win/libvpx.a' not in record['files']:
        raise ValueError('Windows libvpx receipt lacks its static library')
    from prepare_vorbis import verified_dependency as verify_audio
    audio, audio_flags = verify_audio()
    return dict(record, vorbis=audio), ['-I', str(SOURCE), str(BUILD / 'libvpx.a'), *audio_flags]


def prepare():
    if os.name != 'nt': raise ValueError('this bootstrap targets Windows')
    prepare_work()
    archive = ROOT / f'work/tmp/libvpx-{VERSION}.tar.gz'
    for path in (archive, SOURCE, BUILD, RECEIPT):
        if path.is_symlink(): raise ValueError('dependency outputs must not be symlinks')
    if not archive.is_file() or digest(archive) != SHA256:
        deadline = time.monotonic() + 300
        total = 0
        with urllib.request.urlopen(URL, timeout=20) as response, archive.open('wb') as output:
            while chunk := response.read(65536):
                total += len(chunk)
                if total > 16 << 20 or time.monotonic() > deadline: raise RuntimeError('libvpx download budget exceeded')
                output.write(chunk)
        if digest(archive) != SHA256: raise ValueError('libvpx archive checksum mismatch')
    with tarfile.open(archive) as source:
        members = source.getmembers()
        if sum(m.size for m in members) > 128 << 20: raise ValueError('libvpx expanded size bound')
        if SOURCE.exists():
            for member in members:
                if not member.isfile(): continue
                path = (SOURCE.parent / member.name).resolve()
                if not path.is_relative_to(SOURCE.resolve()) or not path.is_file() or digest(path) != hashlib.sha256(source.extractfile(member).read()).hexdigest():
                    raise ValueError('existing libvpx source differs from its pinned archive')
        else: source.extractall(SOURCE.parent, filter='data')
    compiler = shutil.which('clang')
    if not compiler: raise ValueError('put LLVM MinGW clang on PATH')
    bin_dir = Path(compiler).parent
    busybox = bin_dir.parent / 'busybox/bin'
    perl = shutil.which('perl')
    if not perl:
        candidate = Path(os.environ.get('ProgramFiles', 'C:/Program Files')) / 'Git/usr/bin/perl.exe'
        if candidate.is_file(): perl = str(candidate)
    if not perl or not (busybox / 'sh.exe').is_file():
        raise ValueError('LLVM MinGW busybox and Perl (for example Git for Windows) are required')
    shell = Path(perl).parent / 'sh.exe'
    if not shell.is_file(): raise ValueError('a POSIX shell beside Perl is required (for example Git for Windows)')
    short = ctypes.create_unicode_buffer(32768)
    if not ctypes.windll.kernel32.GetShortPathNameW(str(shell), short, len(short)):
        raise OSError('cannot resolve the POSIX shell path')
    shell_path = short.value.replace('\\', '/')
    env = dict(os.environ, CC='clang', CXX='clang++', AR='llvm-ar', LD='clang', NM='llvm-nm', STRIP='llvm-strip',
               PATH=os.pathsep.join(map(str, (bin_dir, Path(perl).parent, busybox))) + os.pathsep + os.environ['PATH'])
    BUILD.mkdir(parents=True, exist_ok=True)
    os.environ.update(env)
    configure = run([shell_path, '../../tools/libvpx-' + VERSION + '/configure', '--target=generic-gnu',
                     '--disable-examples', '--disable-tools', '--disable-docs', '--disable-unit-tests',
                     '--disable-dependency-tracking', '--enable-static', '--disable-shared'],
                    cwd=BUILD, log=ROOT / 'work/logs/vulkan-video-configure.log', seconds=120)
    build = run([str(bin_dir / 'mingw32-make.exe'), '-j8', 'SHELL=' + shell_path, 'libvpx.a'],
                cwd=BUILD, log=ROOT / 'work/logs/vulkan-video-build.log', seconds=600, rss_limit=4 << 30)
    files = [BUILD / 'libvpx.a', *sorted((SOURCE / 'vpx').glob('*.h')), SOURCE / 'LICENSE', SOURCE / 'PATENTS']
    record = dict(version=VERSION, url=URL, archive_sha256=SHA256, configure=configure, build=build,
                  compiler=subprocess.check_output([compiler, '--version'], text=True, timeout=10),
                  files={p.relative_to(ROOT).as_posix(): digest(p) for p in files})
    RECEIPT.write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
    from prepare_vorbis import prepare as prepare_audio
    record['vorbis'] = prepare_audio()
    return record


if __name__ == '__main__':
    try: print(json.dumps(prepare(), indent=2))
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error: raise SystemExit(str(error))
