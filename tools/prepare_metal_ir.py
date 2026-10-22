"""Compile selected MSL libraries to verified Metal IR without replacing their source files."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time

from build import signature, write_json
from recompile import ROOT, digest


def same_compiler(left, right):
    if not isinstance(left, dict) or not isinstance(left.get('version'), str): return False
    # The installed component's mount directory changes across boots; its version and binary hashes identify it.
    def stable(info):
        return dict(info, version='\n'.join(line for line in info['version'].splitlines() if not line.startswith('InstalledDir:')))
    return stable(left) == stable(right)


def metal_tools():
    probe = subprocess.run(['xcrun', 'metal', '--version'], capture_output=True, text=True, timeout=15)
    if probe.returncode == 0:
        metal = Path(subprocess.check_output(['xcrun', '--find', 'metal'], text=True, timeout=15).strip())
        linker = Path(subprocess.check_output(['xcrun', '--find', 'metallib'], text=True, timeout=15).strip())
    else:
        # Xcode's wrapper can miss an installed cryptex; use the official component's reported location.
        state = json.loads(subprocess.check_output(['xcodebuild', '-showComponent', 'MetalToolchain', '-json'], stderr=subprocess.PIPE, text=True, timeout=30))
        if state.get('status') != 'installed': raise ValueError('Metal Toolchain is not installed')
        directory = Path(state['toolchainSearchPath']) / 'Metal.xctoolchain/usr/bin'
        metal, linker = directory / 'metal', directory / 'metallib'
    version = subprocess.check_output([str(metal), '--version'], text=True, timeout=15)
    return metal, linker, dict(version=version.strip(), metal_sha256=digest(metal), metallib_sha256=digest(linker))


def build_helper():
    binary = ROOT / 'work/tools/metal-ir-helper'
    receipt = binary.with_suffix('.json')
    paths = ['tools/metal_ir.m', 'runtime/graphics/shader_prep.m', 'runtime/graphics/include/shader_prep.h',
             'runtime/graphics/include/shader_ir_source.h', 'runtime/graphics/include/nvn_shader_math.h',
             'runtime/graphics/include/nvn_cube_source.h', 'runtime/graphics/include/shader_status.h']
    flags = ['-std=gnu11', '-O2', '-fobjc-arc', '-Wno-deprecated-declarations', '-I', str(ROOT / 'runtime/graphics/include'),
             '-framework', 'Foundation', '-framework', 'Metal']
    context = signature(dict(sources={p:digest(ROOT / p) for p in paths}, flags=flags,
                             clang=subprocess.check_output(['clang', '--version'], text=True, timeout=10)))
    old = json.loads(receipt.read_text()) if receipt.exists() else {}
    if binary.is_symlink() or receipt.is_symlink(): raise ValueError('Metal helper must not be a symlink')
    if old.get('context') != context or not binary.is_file() or old.get('sha256') != digest(binary):
        binary.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='metal-helper-', dir=ROOT / 'work/tmp') as temp:
            output = Path(temp) / 'helper'
            subprocess.run(['clang', *flags, str(ROOT / paths[0]), str(ROOT / paths[1]), '-o', str(output)], check=True, timeout=60)
            output.replace(binary)
        write_json(receipt, dict(context=context, sha256=digest(binary)))
    return binary


def prepare(directory, keys, jobs, report_path):
    directory = Path(os.path.abspath(directory)); report_path = Path(os.path.abspath(report_path))
    for path in (directory, report_path):
        if not path.is_relative_to(ROOT / 'work') or any(p.is_symlink() for p in (path, *path.parents)):
            raise ValueError('Metal preparation paths must stay under work without symlinks')
    if report_path.exists(): raise ValueError('report already exists; refusing to overwrite')
    if (not isinstance(keys, list) or not 1 <= len(keys) <= 256 or
            any(not isinstance(k, str) or not re.fullmatch(r'[A-Za-z0-9_-]{1,128}', k) for k in keys) or len(set(keys)) != len(keys)):
        raise ValueError('select 1 to 256 unique library names')
    if type(jobs) is not int or not 1 <= jobs <= 8: raise ValueError('expected 1 to 8 compiler workers')
    for key in keys:
        path = directory / (key + '.metallib')
        if path.is_symlink() or not path.is_file() or not 0 < path.stat().st_size <= 64 << 20:
            raise ValueError('missing or oversized MSL library')
    metal, linker, compiler = metal_tools(); helper = build_helper()
    cache = directory / 'ir-v1'
    if cache.is_symlink(): raise ValueError('Metal IR cache must not be a symlink')
    cache.mkdir(exist_ok=True)
    started = time.perf_counter()
    with tempfile.TemporaryDirectory(prefix='metal-ir-', dir=ROOT / 'work/tmp') as temp:
        temporary = Path(temp)
        plan = [dict(source=str(directory / (key + '.metallib')), prepared=str(temporary / (key + '.metal')), layered=False) for key in keys]
        plan_path = temporary / 'plan.json'; write_json(plan_path, plan)
        descriptions = json.loads(subprocess.check_output([str(helper), 'prepare', str(plan_path)], text=True, timeout=60))
        if len(descriptions) != len(plan): raise ValueError('source preparation count mismatch')
        unique = {}; aliases = {}
        for name, item, description in zip(keys, plan, descriptions):
            key = description['key']
            if not re.fullmatch('[0-9a-f]{64}', key): raise ValueError('invalid prepared source key')
            aliases[name] = key; unique.setdefault(key, (item, description))

        def compile_one(pair):
            item, description = pair; key = description['key']; output = cache / (key + '.metallib'); metadata = cache / (key + '.json')
            if output.is_symlink() or metadata.is_symlink(): raise ValueError('Metal IR output must not be a symlink')
            old = json.loads(metadata.read_text()) if metadata.exists() else {}
            if (all(old.get(k) == v for k,v in description.items()) and same_compiler(old.get('compiler'), compiler) and
                    output.is_file() and old.get('library_sha256') == digest(output)):
                return dict(key=key, cached=True, seconds=0)
            begin = time.perf_counter(); air = temporary / (key + '.air'); library = temporary / (key + '.metallib')
            for command in ([str(metal), *description['flags'], '-c', item['prepared'], '-o', str(air)],
                            [str(linker), str(air), '-o', str(library)]):
                result = subprocess.run(command, capture_output=True, text=True, timeout=120)
                if result.returncode: return dict(key=key, error=result.stderr[:2000], seconds=time.perf_counter() - begin)
            with library.open('rb') as stream:
                if stream.read(4) != b'MTLB': raise ValueError('compiler did not produce a Metal library')
            record = dict(description, compiler=compiler, library_sha256=digest(library))
            library.replace(output); write_json(metadata, record)
            air.unlink()
            return dict(key=key, cached=False, seconds=time.perf_counter() - begin)

        with ThreadPoolExecutor(max_workers=jobs) as executor: results = list(executor.map(compile_one, unique.values()))
        report = dict(format='totk-metal-ir-preparation-v1', compiler=compiler, libraries=aliases, results=results,
                      wall_seconds=time.perf_counter() - started, workers=jobs,
                      metal_ir_compiled=not any('error' in r for r in results), pso_compiled=False, gameplay_verified=False)
        report_path.parent.mkdir(parents=True, exist_ok=True); write_json(report_path, report)
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--keys', type=Path, required=True)
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    report = prepare(args.directory, json.loads(args.keys.read_text()), args.jobs, args.report)
    print(json.dumps({k:v for k,v in report.items() if k not in ('compiler','libraries','results')}))
    raise SystemExit(0 if report['metal_ir_compiled'] else 1)
