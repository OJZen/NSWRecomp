"""Prepare at most 32 default compute pipelines for this device from verified offline Metal IR."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time

from build import write_json
from prepare_metal_ir import build_helper, metal_tools, same_compiler
from recompile import ROOT, digest


def prepare(directory, keys, report_path):
    directory = Path(os.path.abspath(directory)); report_path = Path(os.path.abspath(report_path))
    for path in (directory, report_path):
        if not path.is_relative_to(ROOT / 'work') or any(p.is_symlink() for p in (path, *path.parents)):
            raise ValueError('compute preparation paths must stay under work without symlinks')
    if report_path.exists(): raise ValueError('report already exists; refusing to overwrite')
    if (not isinstance(keys, list) or not 1 <= len(keys) <= 32 or
            any(not isinstance(k, str) or not re.fullmatch(r'[A-Za-z0-9_-]{1,128}', k) for k in keys) or len(set(keys)) != len(keys)):
        raise ValueError('select 1 to 32 unique compute library names')
    plan = []
    for key in keys:
        source = directory / (key + '.metallib'); reflection = source.with_suffix('.json')
        for path in (source, reflection):
            if path.is_symlink() or not path.is_file() or not 0 < path.stat().st_size <= 64 << 20:
                raise ValueError('missing or oversized compute source/reflection')
        record = json.loads(reflection.read_text())
        if (record.get('stage') != 'cs' or not isinstance(record.get('entry'), str) or
                not re.fullmatch(r'[A-Za-z_]\w*', record['entry'], re.ASCII)):
            raise ValueError('expected a compute reflection with a valid entry point')
        plan.append(dict(source=str(source), function=record['entry']))
    metal, _, compiler = metal_tools(); translator = metal.with_name('metal-tt')
    compiler['metal_tt_sha256'] = digest(translator)
    helper = build_helper(); cache = directory / 'compute-v1'
    if cache.is_symlink(): raise ValueError('compute cache must not be a symlink')
    cache.mkdir(exist_ok=True)
    started = time.perf_counter(); results = []
    with tempfile.TemporaryDirectory(prefix='compute-pipelines-', dir=ROOT / 'work/tmp') as temp:
        temporary = Path(temp); plan_path = temporary / 'plan.json'; write_json(plan_path, plan)
        identities = json.loads(subprocess.check_output([str(helper), 'compute-plan', str(plan_path)], text=True, timeout=60))
        if len(identities) != len(plan): raise ValueError('compute plan count mismatch')
        pending = []
        for name, identity in zip(keys, identities):
            if any(not re.fullmatch('[0-9a-f]{64}', identity[k]) for k in ('key', 'ir_key')):
                raise ValueError('invalid compute identity')
            ir = directory / 'ir-v1' / (identity['ir_key'] + '.metallib')
            ir_metadata = ir.with_suffix('.json')
            if ir.parent.is_symlink() or ir.is_symlink() or ir_metadata.is_symlink(): raise ValueError('IR must not be a symlink')
            ir_record = json.loads(ir_metadata.read_text())
            if digest(ir) != ir_record['library_sha256']: raise ValueError('IR changed during preparation')
            binary = cache / (identity['key'] + '.metallib'); metadata = binary.with_suffix('.json')
            if binary.is_symlink() or metadata.is_symlink(): raise ValueError('compute output must not be a symlink')
            old = json.loads(metadata.read_text()) if metadata.exists() else {}
            if (all(old.get(k) == v for k,v in identity.items()) and same_compiler(old.get('compiler'), compiler) and
                    binary.is_file() and old.get('binary_sha256') == digest(binary)):
                results.append(dict(library=name, key=identity['key'], cached=True, seconds=0)); continue
            pending.append((name, identity, ir, ir_record['os'], binary, metadata))
        if pending:
            begin = time.perf_counter(); output = temporary / 'pipeline.metallib'; script = temporary / 'pipeline.mtlp-json'
            unique = list({item[1]['key']:item for item in pending}.values())
            targets = {(identity['architecture'], os_version) for _,identity,_,os_version,_,_ in unique}
            if len(targets) != 1: raise ValueError('compute batch mixes device or OS targets')
            architecture, os_version = targets.pop()
            paths = [dict(label=f'library{i}', path=str(item[2])) for i,item in enumerate(unique)]
            pipelines = [dict(compute_function=f"alias:library{i}#{item[1]['function']}") for i,item in enumerate(unique)]
            write_json(script, dict(libraries=dict(paths=paths), pipelines=dict(compute_pipelines=pipelines)))
            command = [str(translator), str(script), '-arch', architecture,
                       '-target', 'air64-apple-macos' + os_version, '-disable-disk-cache', '-o', str(output)]
            compiled = subprocess.run(command, capture_output=True, text=True, timeout=120)
            if compiled.returncode:
                results.extend(dict(library=name, error=compiled.stderr[:2000]) for name,_,_,_,_,_ in pending)
            else:
                if not 0 < output.stat().st_size <= 32 << 20: raise ValueError('compute binary exceeds 32 MiB limit')
                binary_hash = digest(output)
                for name,identity,_,_,binary,metadata in unique:
                    record = dict(identity, binary_sha256=binary_hash, compiler=compiler)
                    candidate = temporary / (identity['key'] + '.metallib')
                    os.link(output, candidate); candidate.replace(binary); write_json(metadata, record)
                results.extend(dict(library=item[0], key=item[1]['key'], cached=False) for item in pending)
        verified = []
        if len(results) == len(keys) and not any('error' in r for r in results):
            verification = subprocess.run([str(helper), 'compute-verify', str(plan_path)], capture_output=True, text=True, timeout=60)
            if verification.returncode: raise ValueError('strict compute binary verification failed: ' + verification.stderr[:2000])
            verified = json.loads(verification.stdout)
        report = dict(format='totk-compute-preparation-v1', compiler=compiler, results=results, verification=verified,
                      wall_seconds=time.perf_counter() - started, strict_binary_hits=len(verified),
                      compiler_invocations=int(bool(pending)),
                      compute_pipelines_prepared=len(verified) == len(keys), render_pipelines_prepared=False, gameplay_verified=False)
        report_path.parent.mkdir(parents=True, exist_ok=True); write_json(report_path, report)
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--keys', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    report = prepare(args.directory, json.loads(args.keys.read_text()), args.report)
    print(json.dumps({k:v for k,v in report.items() if k not in ('compiler','results','verification')}))
    raise SystemExit(0 if report['compute_pipelines_prepared'] else 1)
