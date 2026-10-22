"""Prepare up to 256 explicitly selected shaders in a new isolated workspace under work/."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import tempfile
import time

import build_shaders as build
import compile_runtime_shaders as runtime
import translate_shaders as translate

ROOT = Path(__file__).resolve().parents[1]


def prepare(catalog, output, keys):
    if (not isinstance(keys, list) or not 1 <= len(keys) <= 256 or
            any(not isinstance(key, str) for key in keys) or len(set(keys)) != len(keys)):
        raise ValueError('select 1 to 256 unique shader keys')
    catalog = Path(catalog)
    entries = build.shader_entries(catalog, keys)
    base_keys = {e.get('base', e['key']) for e in entries} | {e['next']['key'] for e in entries if 'next' in e}
    bases = build.shader_entries(catalog, base_keys)
    version = build.chain()
    if version == 'unprepared': raise ValueError('shader toolchain is not prepared')
    output = Path(os.path.abspath(output))
    work = ROOT / 'work'
    if not output.is_relative_to(work) or output == work or any(p.is_symlink() for p in (output, *output.parents)):
        raise ValueError('output must be a new directory under work without symlinks')
    if output.exists(): raise ValueError('output already exists; refusing to overwrite')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='shader-batch-', dir=work / 'tmp') as temp:
        root = Path(temp); directory = root / 'work/shaders'
        for name in ('code', 'source', 'variants'): (directory / name).mkdir(parents=True)
        (root / 'work/tmp').mkdir()
        for e in bases:
            source = catalog / 'code' / (e['key'] + '.bin')
            if source.is_symlink(): raise ValueError('shader input must not be a symlink')
            data = source.read_bytes()
            if hashlib.sha256(data).hexdigest() != e['code_sha256']:
                raise ValueError('shader input fingerprint mismatch or symlink')
            (directory / 'code' / source.name).write_bytes(data)
        for e in entries:
            if 'base' in e:
                source = catalog / 'variants' / (e['key'] + '.json')
                if source.is_symlink(): raise ValueError('shader variant must not be a symlink')
                shutil.copyfile(source, directory / 'variants' / source.name)
        build.write_json(directory / 'index.json', bases)
        (directory / 'shader_map.tsv').write_text('package\tindex\tstage\tcontrol\n' + ''.join(
            f"{e['source']}\t0\t{runtime.LABELS[e['stage']]}\t{e['key']}\n" for e in bases))
        old = (build.ROOT, translate.ROOT, runtime.SHADERS, build.chain, runtime.translate, build.build)
        timing = {}
        def timed(name, operation):
            start = time.perf_counter()
            try: return operation()
            finally: timing[name] = time.perf_counter() - start
        start = time.perf_counter()
        try:
            # Reuse the production compiler in one process, with independent output and the original tool fingerprint.
            build.ROOT = translate.ROOT = root; runtime.SHADERS = directory
            build.chain = lambda: version
            runtime.translate = lambda: timed('spirv_seconds', old[4])
            build.build = lambda: timed('msl_seconds', old[5])
            blocked = runtime.compile_pending(keys)
        finally:
            build.ROOT, translate.ROOT, runtime.SHADERS, build.chain, runtime.translate, build.build = old
        hashes = {}
        for e in entries:
            for suffix in ('_b.metallib', '_b.json'):
                path = directory / ('b-' + version) / (e['key'] + suffix)
                if path.exists(): hashes[str(path.relative_to(root))] = hashlib.sha256(path.read_bytes()).hexdigest()
        if not blocked and len(hashes) != len(entries) * 2:
            raise ValueError('compiler returned without all requested shader outputs')
        report = dict(format='totk-shader-batch-v1', chain=version, requested=keys, base_programs=len(bases),
                      source_translation_completed=not blocked, stages=timing, wall_seconds=time.perf_counter() - start,
                      artifacts=hashes, metal_ir_compiled=False, pso_compiled=False, gameplay_verified=False)
        build.write_json(root / 'preparation.json', report)
        root.rename(output)
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--catalog', type=Path, required=True)
    parser.add_argument('--keys', type=Path, required=True, help='JSON array of shader keys from that catalog')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = prepare(args.catalog, args.output, json.loads(args.keys.read_text()))
    print(json.dumps({k: v for k, v in report.items() if k not in ('requested', 'artifacts')}))
    raise SystemExit(0 if report['source_translation_completed'] else 1)
