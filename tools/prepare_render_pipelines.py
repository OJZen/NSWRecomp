"""Recompile one bounded capture of real render descriptors using their exact offline IR libraries."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time

from build import write_json
from prepare_metal_ir import metal_tools
from recompile import ROOT, digest


def checked_file(path, limit):
    if path.is_symlink() or not path.is_file() or not 0 < path.stat().st_size <= limit:
        raise ValueError('missing, linked, or oversized render preparation input: ' + str(path))
    return path


def bind_libraries(script, directory, count):
    if not isinstance(script, dict) or not isinstance(script.get('libraries'), dict) or not isinstance(script.get('pipelines'), dict):
        raise ValueError('invalid exported render script')
    pipelines = script['pipelines']; libraries = script['libraries']; paths = libraries.get('paths'); functions = script.get('functions', {})
    if not isinstance(functions, dict): raise ValueError('invalid exported function table')
    if (not isinstance(pipelines.get('render_pipelines'), list) or not 1 <= len(pipelines['render_pipelines']) <= count or
            any(value for key,value in pipelines.items() if key != 'render_pipelines') or
            any(value for key,value in libraries.items() if key != 'paths') or any(functions.values())):
        raise ValueError('capture contains unsupported or excessive pipeline types')
    if not isinstance(paths, list) or not 1 <= len(paths) <= 64: raise ValueError('expected 1 to 64 captured IR libraries')
    wanted = {}
    for item in paths:
        if (not isinstance(item, dict) or not isinstance(item.get('label'), str) or
                not re.fullmatch(r'[0-9A-Fa-f]{64}', item['label']) or item['label'].lower() in wanted):
            raise ValueError('invalid or duplicate captured library hash')
        wanted[item['label'].lower()] = item
    ir_directory = directory / 'ir-v1'
    if ir_directory.is_symlink() or not ir_directory.is_dir(): raise ValueError('verified IR directory is missing')
    inputs = {}; versions = set()
    for metadata_path in sorted(ir_directory.glob('*.json')):
        if not re.fullmatch(r'[0-9a-f]{64}', metadata_path.stem): continue
        record = json.loads(checked_file(metadata_path, 64 << 10).read_text())
        if not isinstance(record, dict): raise ValueError('invalid IR metadata')
        hashed = record.get('library_sha256')
        if hashed not in wanted: continue
        if record.get('format') != 'totk-metal-ir-v1' or not re.fullmatch(r'\d+\.\d+\.\d+', record.get('os', '')):
            raise ValueError('captured library lacks validated IR identity')
        path = checked_file(metadata_path.with_suffix('.metallib'), 64 << 20)
        if digest(path) != hashed: raise ValueError('captured IR library hash mismatch')
        wanted.pop(hashed)['path'] = str(path)
        inputs[str(path)] = hashed; versions.add(record['os'])
        if not wanted: break
    if wanted: raise ValueError('capture references IR libraries that have not been prepared: ' + ', '.join(sorted(wanted)))
    if len(versions) != 1: raise ValueError('capture mixes OS targets')
    return inputs, versions.pop()


def prepare(capture, directory, output):
    capture, directory, output = [Path(os.path.abspath(p)) for p in (capture, directory, output)]
    for path in (capture, directory, output):
        if path == ROOT / 'work' or not path.is_relative_to(ROOT / 'work') or any(p.is_symlink() for p in (path, *path.parents)):
            raise ValueError('render preparation paths must stay under work without symlinks')
    if output.exists(): raise ValueError('output already exists; refusing to overwrite')
    manifest_path = checked_file(capture / 'capture.json', 64 << 10)
    manifest_hash = digest(manifest_path); manifest = json.loads(manifest_path.read_text())
    if not isinstance(manifest, dict) or manifest.get('format') != 'totk-render-capture-v1': raise ValueError('invalid render capture manifest')
    keys = manifest.get('keys')
    if (not isinstance(keys, list) or not 1 <= len(keys) <= 32 or
            any(not isinstance(key, str) or not re.fullmatch('[0-9a-f]{64}', key) for key in keys) or len(set(keys)) != len(keys)):
        raise ValueError('expected 1 to 32 unique pipeline keys')
    if (manifest.get('chain') != directory.name or type(manifest.get('device')) is not int or manifest['device'] <= 0 or
            any(not isinstance(manifest.get(k), str) or not 0 < len(manifest[k]) <= 256 for k in ('architecture','os_build','contract')) or
            not re.fullmatch('[a-z0-9_]+', manifest['architecture'])):
        raise ValueError('invalid or mismatched render capture context')
    binary = checked_file(capture / 'capture.metallib', 32 << 20)
    if digest(binary) != manifest.get('binary_sha256'): raise ValueError('render capture hash mismatch')
    started = time.perf_counter(); metal, _, compiler = metal_tools(); extractor = metal.with_name('metal-source'); translator = metal.with_name('metal-tt')
    compiler.update(metal_source_sha256=digest(extractor), metal_tt_sha256=digest(translator))
    with tempfile.TemporaryDirectory(prefix='render-pipelines-', dir=ROOT / 'work/tmp') as temp:
        temporary = Path(temp); extracted = temporary / 'extracted'
        subprocess.run([str(extractor), '-flatbuffers=json', str(binary), '-o', str(extracted)], check=True, text=True, timeout=60)
        files = [p for p in extracted.rglob('*') if p.is_file()]
        if any(p.is_symlink() for p in extracted.rglob('*')) or sum(p.stat().st_size for p in files) > 128 << 20:
            raise ValueError('extracted render resources exceed bounds')
        scripts = list(extracted.rglob('*.mtlp-json'))
        if len(scripts) != 1: raise ValueError('expected exactly one exported pipeline script')
        script = json.loads(checked_file(scripts[0], 4 << 20).read_text())
        inputs, version = bind_libraries(script, directory, len(keys))
        published = temporary / 'published'; published.mkdir()
        plan = published / 'pipelines.mtlp-json'; write_json(plan, script)
        result = published / 'pipelines.metallib'
        command = [str(translator), str(plan), '-arch', manifest['architecture'], '-target', 'air64-apple-macos' + version,
                   '-disable-disk-cache', '-o', str(result)]
        subprocess.run(command, check=True, text=True, timeout=120)
        checked_file(result, 32 << 20)
        if digest(manifest_path) != manifest_hash or digest(binary) != manifest['binary_sha256'] or any(digest(Path(p)) != h for p,h in inputs.items()):
            raise ValueError('render preparation inputs changed while compiling')
        manifest.update(format='totk-render-binary-v1', binary_sha256=digest(result))
        write_json(published / 'manifest.json', manifest)
        report = dict(format='totk-render-preparation-v1', pipeline_keys=len(keys), descriptors=len(script['pipelines']['render_pipelines']),
                      input_libraries=inputs, capture_manifest_sha256=manifest_hash, compiler=compiler, target='air64-apple-macos' + version,
                      wall_seconds=time.perf_counter() - started, binary_bytes=result.stat().st_size,
                      render_binaries_compiled=True, strict_runtime_hits_verified=False, gameplay_verified=False)
        write_json(published / 'preparation.json', report)
        output.parent.mkdir(parents=True, exist_ok=True); published.rename(output)
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture', type=Path, required=True)
    parser.add_argument('--directory', type=Path, required=True, help='MSL source directory containing verified ir-v1 libraries')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = prepare(args.capture, args.directory, args.output)
    print(json.dumps({k:v for k,v in report.items() if k not in ('compiler','input_libraries')}))
