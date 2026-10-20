"""Incremental single-compiler build and linkage verification; never executes a guest function."""
import argparse
import fcntl
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys

from bounded import run
from recompile import ROOT, VENDOR, digest, generate, prepare_work, profile

FLAGS = ['-std=gnu11', '-O1', '-g0', '-fPIC', '-fno-strict-aliasing']
BUILD = ROOT / 'work/build'
RUNTIME = ('probe.c', 'dispatch.h', 'load.c', 'load.h', 'hle.c', 'hle_symbols.inc')


def write_json(path, value):
    partial = path.with_suffix(path.suffix + '.partial')
    if path.is_symlink() or partial.is_symlink(): raise ValueError('JSON output must not be a symlink')
    partial.write_text(json.dumps(value, indent=2) + '\n')
    partial.replace(path)


def context():
    compiler = subprocess.check_output(['clang', '--version'], text=True, timeout=10)
    return {'compiler': compiler, 'flags': FLAGS,
            'headers': {p.name: digest(p) for p in sorted((VENDOR / 'runtime').glob('*.h'))}}


def signature(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True).encode()).hexdigest()


def ready(object_path, source_sha, context_sha):
    receipt = object_path.with_suffix('.json')
    if not object_path.is_file() or object_path.is_symlink() or not receipt.is_file() or receipt.is_symlink():
        return False
    try:
        record = json.loads(receipt.read_text())
        return (record['source_sha256'] == source_sha and record['context_sha256'] == context_sha
                and digest(object_path) == record['object_sha256'])
    except (ValueError, KeyError, OSError):
        return False


def compile_unit(source, output, source_sha, context_sha, flags):
    if digest(source) != source_sha:
        raise ValueError('source changed during compilation')
    partial = output.with_suffix('.o.partial')
    try:
        subprocess.run(['clang', *flags, '-I', str(VENDOR / 'runtime'), '-c', str(source),
                        '-o', str(partial)], check=True, timeout=120)
        object_sha = digest(partial)
        partial.replace(output)
        write_json(output.with_suffix('.json'), {'source_sha256': source_sha,
                   'context_sha256': context_sha, 'object_sha256': object_sha})
    finally:
        partial.unlink(missing_ok=True)


def batch(name, limit=128):
    current = json.loads((BUILD / 'context.json').read_text())
    if current != context():
        raise ValueError('compiler or headers changed during build')
    context_sha = signature(current)
    source = ROOT / 'work/generated' / name
    generation = json.loads((source / 'receipt.json').read_text())
    directory = BUILD / name
    directory.mkdir(exist_ok=True)
    if directory.is_symlink():
        raise ValueError('object directory must not be a symlink')
    compiled = remaining = reused = 0
    for file, sha in generation['files'].items():
        if not re.fullmatch(re.escape(name) + r'_(?:\d{3,}|table)\.c', file):
            raise ValueError('unexpected generated filename')
        output = directory / Path(file).with_suffix('.o')
        if ready(output, sha, context_sha):
            reused += 1
        elif compiled < limit:
            compile_unit(source / file, output, sha, context_sha, current['flags'])
            compiled += 1
        else:
            remaining += 1
    result = {'module': name, 'compiled': compiled, 'reused': reused, 'remaining': remaining}
    write_json(directory / 'batch.json', result)
    return result


def object_list(generations, current):
    objects = []
    for name, generation in generations.items():
        directory = BUILD / name
        keep = {'batch.json'}
        for file, sha in generation['files'].items():
            obj = directory / Path(file).with_suffix('.o')
            if not ready(obj, sha, signature(current)):
                raise ValueError('missing or stale object; rerun build to resume')
            objects.append(obj)
            keep.update((obj.name, obj.with_suffix('.json').name))
        for old in directory.iterdir():
            if old.name not in keep:
                if old.is_symlink() or not old.is_file() or not re.fullmatch(
                        re.escape(name) + r'_(?:\d{3,}|table)\.(?:o|json)(?:\.partial)?', old.name):
                    raise ValueError('unexpected object output; refusing cleanup')
                old.unlink()
    return objects


def build(exefs):
    prepare_work()
    BUILD.mkdir(exist_ok=True)
    if BUILD.is_symlink():
        raise ValueError('build directory must not be a symlink')
    marker = BUILD / 'format.json'
    if not marker.exists():
        if any(BUILD.iterdir()):
            raise ValueError('unrecognized build output directory')
        write_json(marker, {'format': 'totk-objects-v1'})
    elif json.loads(marker.read_text()) != {'format': 'totk-objects-v1'}:
        raise ValueError('unrecognized build format')
    with (BUILD / 'build.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        generations = {name: generate(exefs, name) for name in profile()['modules']}
        current = context()
        write_json(BUILD / 'context.json', current)
        metrics = []
        for name in generations:
            index = 0
            while True:
                metrics.append(run([sys.executable, str(Path(__file__).resolve()), '--worker', name],
                                   cwd=ROOT, log=ROOT / 'work/logs' / f'build-{name}-{index:03}.log'))
                result = json.loads((BUILD / name / 'batch.json').read_text())
                print(json.dumps(result), flush=True)
                if not result['remaining']:
                    break
                index += 1
        objects = object_list(generations, current)
        # This C main validates linked tables without invoking game code or constructors.
        response = BUILD / 'link.rsp'
        candidate = BUILD / 'link-probe.partial'
        command = ['clang', *FLAGS, '-DTOTK_LINK_ALL', '-I', str(VENDOR / 'runtime'),
                   *[str(ROOT / 'runtime' / name) for name in RUNTIME if name.endswith('.c')],
                   *map(str, objects), '-lm', '-o', str(candidate)]
        response.write_text(shlex.join(command[1:]) + '\n')
        try:
            link = run(['clang', '@' + str(response)], cwd=ROOT, log=ROOT / 'work/logs/link.log')
            execution = run([str(candidate)], cwd=ROOT, log=ROOT / 'work/logs/link-probe.log', seconds=30)
            result = json.loads((ROOT / 'work/logs/link-probe.log').read_text())
            expected = {name: record['functions'] for name, record in generations.items()}
            if result != {'modules': expected, 'game_executed': False}:
                raise ValueError('linked function tables differ from generation receipts')
            result.update(status='linked_tables_verified', context=current, compile_batches=metrics,
                          link=link, execution=execution, executable_sha256=digest(candidate),
                          runtime_sources={name: digest(ROOT / 'runtime' / name) for name in RUNTIME},
                          generation={name: {'inputs': r['inputs'], 'files': r['files'],
                                      'unsupported_instructions': r['unsupported_instructions']}
                                      for name, r in generations.items()})
            candidate.replace(BUILD / 'link-probe')
            write_json(BUILD / 'result.json', result)
        finally:
            candidate.unlink(missing_ok=True)
            response.unlink(missing_ok=True)
        return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exefs', type=Path)
    parser.add_argument('--worker', choices=tuple(profile()['modules']), help=argparse.SUPPRESS)
    args = parser.parse_args()
    try:
        if args.worker:
            print(json.dumps(batch(args.worker)))
        else:
            if args.exefs is None:
                parser.error('--exefs is required')
            result = build(args.exefs.resolve())
            print(json.dumps({k: result[k] for k in ('status', 'modules', 'game_executed')}))
            if any(r['unsupported_instructions'] for r in result['generation'].values()):
                parser.exit(2, 'linkage passed; unsupported guest instructions still remain\n')
    except (ValueError, OSError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, f'{error}\n')
