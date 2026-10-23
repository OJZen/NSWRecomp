"""Build the pinned, patched hactool locally with Clang (including LLVM MinGW)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

from bounded import run
from host import executable, response_args, response_command
from recompile import ROOT, prepare_work

COMMIT = '1d64a83450e025622f3468c28fc4164dad2c5ef6'
PATCH = ROOT / 'tools/hactool.patch'
MPI_CHECK = ROOT / 'tests/native_hactool_mpi.c'


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def tool():
    binary = ROOT / 'work/tools' / executable('hactool')
    receipt = ROOT / 'work/tools/hactool.json'
    if not binary.is_file() or not receipt.is_file():
        raise ValueError('prepare the pinned hactool with tools/prepare_hactool.py first')
    record = json.loads(receipt.read_text())
    if (record.get('commit') != COMMIT or record.get('patch_sha256') != digest(PATCH)
            or record.get('binary_sha256') != digest(binary)):
        raise ValueError('prepared hactool does not match its pinned receipt')
    return binary


def build():
    prepare_work()
    target = ROOT / 'work/tools'
    target.mkdir(exist_ok=True)
    binary = target / executable('hactool')
    receipt = target / 'hactool.json'
    context = dict(commit=COMMIT, patch_sha256=digest(PATCH), builder_sha256=digest(Path(__file__)),
                   mpi_check_sha256=digest(MPI_CHECK))
    if binary.is_file() and receipt.is_file():
        previous = json.loads(receipt.read_text())
        if all(previous.get(k) == v for k, v in context.items()) and previous.get('binary_sha256') == digest(binary):
            return previous
    with tempfile.TemporaryDirectory(prefix='hactool-', dir=ROOT / 'work/tmp') as temporary:
        temp = Path(temporary)
        archive = temp / 'source.tar.gz'
        with urllib.request.urlopen(f'https://codeload.github.com/SciresM/hactool/tar.gz/{COMMIT}', timeout=45) as response:
            with archive.open('wb') as output:
                shutil.copyfileobj(response, output)
        with tarfile.open(archive) as source:
            source.extractall(temp, filter='data')
        source = temp / f'hactool-{COMMIT}'
        subprocess.run(['git', 'apply', str(PATCH)], cwd=source, check=True, timeout=15)
        files = sorted(source.glob('*.c')) + sorted((source / 'mbedtls/library').glob('*.c'))
        candidate = temp / executable('hactool')
        flags = ['-O2', '-std=gnu11', '-D_DEFAULT_SOURCE', '-D_FILE_OFFSET_BITS=64',
                 '-D__USE_MINGW_ANSI_STDIO=1', '-I', str(source / 'mbedtls/include')]
        libraries = ['-lws2_32', '-lwinmm', '-lbcrypt'] if os.name == 'nt' else []
        mpi = temp / executable('mpi-check')
        subprocess.run(['clang', *flags, str(MPI_CHECK), str(source / 'mbedtls/library/bignum.c'),
                        '-o', str(mpi)], check=True, timeout=30)
        vectors = bytearray()
        for index in range(8):
            n = int.from_bytes(hashlib.shake_256(f'synthetic modulus {index}'.encode()).digest(256), 'big') | 1 | (1 << 2047)
            value = int.from_bytes(hashlib.shake_256(f'synthetic value {index}'.encode()).digest(255), 'big')
            vectors += n.to_bytes(256, 'big') + value.to_bytes(256, 'big') + pow(value, 65537, n).to_bytes(256, 'big')
        subprocess.run([str(mpi)], input=vectors, check=True, timeout=15)
        vectors[-1] ^= 1
        if subprocess.run([str(mpi)], input=vectors, timeout=15).returncode != 2:
            raise ValueError('hactool MPI check did not reject a corrupted result')
        response = temp / 'compile.rsp'
        response.write_text(response_args([*flags, *map(str, files), *libraries, '-o', str(candidate)]), encoding='utf-8')
        # One compiler driver: compilation is serial, with the same bounded Job
        # containment as guest builds. No system compiler or installation changes.
        metrics = run(response_command(response), cwd=source, log=ROOT / 'work/logs/hactool-build.log',
                      seconds=600, rss_limit=4 << 30)
        for original, name in [('LICENSE', 'hactool.LICENSE'), ('mbedtls/LICENSE', 'mbedtls.LICENSE'),
                               ('mbedtls/apache-2.0.txt', 'mbedtls.apache-2.0.txt')]:
            shutil.copyfile(source / original, target / name)
        candidate.replace(binary)
        record = dict(context, upstream='https://github.com/SciresM/hactool',
                      archive_sha256=digest(archive), binary_sha256=digest(binary), build=metrics,
                      synthetic_mpi_vectors=8, synthetic_mpi_corruption_rejected=True)
        receipt.write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
        return record


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.worker:
        print(json.dumps(build(), indent=2))
    else:
        prepare_work()
        print(json.dumps(run([sys.executable, str(Path(__file__)), '--worker'], cwd=ROOT,
                             log=ROOT / 'work/logs/hactool-prepare.log', seconds=660, rss_limit=4 << 30)))
