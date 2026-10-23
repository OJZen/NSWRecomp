"""Hash-locked executable analysis and portable C generation. No assets are embedded."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from bounded import run
from host import cpu_flags, library_suffix, response_args, response_command

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / 'third_party/lasrecomp'
PROFILE = Path(os.environ.get('NSWRECOMP_PROFILE', ROOT / 'config/totk-1.4.3.json')).resolve()
HOOKS = ROOT / 'config/totk-1.4.3-hooks.json'
HLE_SYMBOLS = ROOT / 'runtime/hle_symbols.inc'
WINDOWS_HLE_SYMBOLS = ROOT / 'runtime/hle_windows_symbols.inc'
sys.path.insert(0, str(VENDOR / 'tools'))
from analyze import eh_frame_functions
from nso import Nso


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def profile():
    value = json.loads(PROFILE.read_text())
    modules = value['modules']
    if (not 1 <= len(modules) <= 13 or
            any(not re.fullmatch(r'rtld|main|sdk|subsdk[0-9]', name) for name in modules)):
        raise ValueError('profile must specify 1..13 supported NSO module names')
    return value


def hooks():
    # An experimental local profile must opt in to its own hooks. Never apply
    # the default adapter's addresses to a different executable.
    if PROFILE == ROOT / 'config/totk-1.4.3.json':
        return HOOKS
    path = profile().get('hooks')
    return (PROFILE.parent / path).resolve() if path else None


def verify(exefs):
    modules = profile()['modules']
    for name, info in modules.items():
        path = exefs / name
        if not path.is_file() or path.stat().st_size > 64 << 20 or digest(path) != info['sha256']:
            raise ValueError(f'{name}: expected the unmodified v{profile()["version"]} NSO; input rejected')
    return modules


def prepare_work():
    if (ROOT / 'work').is_symlink():
        raise ValueError('work must not be a symlink')
    for name in ('tmp', 'logs', 'generated', 'probe'):
        path = ROOT / 'work' / name
        if path.is_symlink():
            raise ValueError('work directories must stay on the workspace volume')
        path.mkdir(parents=True, exist_ok=True)
    tempfile.tempdir = str(ROOT / 'work/tmp')


def inspect(exefs):
    report = {'version': profile()['version'], 'modules': {}}
    previous_end = 0
    for name, info in verify(exefs).items():
        n = Nso(exefs / name)
        base = int(info['base'], 0)
        if base < previous_end:
            raise ValueError('module address ranges overlap')
        previous_end = base + n.size
        report['modules'][name] = dict(info, image_bytes=n.size, text_bytes=n.text[1],
                                      symbols=len(n.syms), imports=len(n.imports()),
                                      exports=len(n.exports()), relocations=len(n.relocs),
                                      relr_relocations=n.relr_count,
                                      unwind_functions=len(eh_frame_functions(n)))
    (ROOT / 'work/inspection.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def generation_inputs(info):
    sources = sorted(p for p in (VENDOR / 'tools').glob('*.py') if not p.name.startswith('test_'))
    return {'schema': 1, 'module': info, 'simd': 'c', 'game_patches': False,
            'generator': {p.name: digest(p) for p in sources}, 'profile': digest(PROFILE),
            'hle_symbols': digest(HLE_SYMBOLS),
            **({'windows_hle_symbols': digest(WINDOWS_HLE_SYMBOLS)} if os.name == 'nt' else {}),
            'hooks': digest(hooks()) if hooks() and info == profile()['modules'].get('main') else None}


def hle_symbols():
    names = []
    text = HLE_SYMBOLS.read_text()
    if os.name == 'nt':
        text += '\n' + WINDOWS_HLE_SYMBOLS.read_text()
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith('//'):
            continue
        match = re.fullmatch(r'HLE\(([A-Za-z_][A-Za-z0-9_]*)\)', line)
        if not match or match[1] in names:
            raise ValueError('invalid or duplicate host API symbol')
        names.append(match[1])
    return names


def diagnostics(directory, name):
    unsupported = {}
    pattern = re.compile(r'rt_udf\(c, (0x[0-9a-f]+)ull\); /\* (.*?) \*/')
    for path in directory.glob('*.c'):
        with path.open() as stream:
            for line in stream:
                if 'rt_udf' in line and (match := pattern.search(line)):
                    unsupported[match[1]] = match[2]
    table = (directory / f'{name}_table.c').read_text()
    count = re.search(rf'const uint64_t {name}_fn_count = (\d+);', table)
    if count is None:
        raise ValueError('generated function table is incomplete')
    return {'functions': int(count[1]), 'unsupported_instructions': unsupported,
            'status': 'needs_instruction_support' if unsupported else 'generated'}


def generate(exefs, name):
    info = verify(exefs)[name]
    inputs = generation_inputs(info)
    final = ROOT / 'work/generated' / name
    receipt = final / 'receipt.json'
    if final.is_symlink():
        raise ValueError('generated output must not be a symlink')
    if final.exists():
        if not receipt.is_file() or receipt.is_symlink():
            raise ValueError('unrecognized generated directory; refusing to replace it')
        old = json.loads(receipt.read_text())
        expected = set(old['files']) | {'receipt.json'}
        if expected != {p.name for p in final.iterdir()}:
            raise ValueError('unexpected files in generated directory; refusing to replace it')
        if (old['inputs'] == inputs and all(Path(file).name == file and not (final / file).is_symlink()
                                           and digest(final / file) == sha for file, sha in old['files'].items())):
            return old
    with tempfile.TemporaryDirectory(prefix=f'generate-{name}-') as temp:
        staged = Path(temp) / name
        bindings = Path(temp) / 'hle.txt'
        bindings.write_text('\n'.join(hle_symbols()) + '\n')
        log = ROOT / 'work/logs' / f'generate-{name}.log'
        metrics = run([sys.executable, str(VENDOR / 'tools/gen.py'), name, str(exefs / name),
                       info['base'], str(staged), '--simd', 'c', '--no-game-patches',
                       '--cache-limit', '32768', '--chunk-bytes', '500000', '--hle', str(bindings),
                       *(['--hooks', str(hooks())] if name == 'main' and hooks() else [])], cwd=ROOT, log=log)
        files = {p.name: digest(p) for p in sorted(staged.glob('*.c'))}
        result = {'inputs': inputs, 'files': files,
                  'generated_bytes': sum(p.stat().st_size for p in staged.glob('*.c')),
                  **diagnostics(staged, name), **metrics}
        (staged / 'receipt.json').write_text(json.dumps(result, indent=2) + '\n')
        if final.exists():
            final.rename(Path(temp) / 'previous')
        try:
            staged.rename(final)
        except OSError:
            previous = Path(temp) / 'previous'
            if previous.exists():
                previous.rename(final)
            raise
    return result


def check_c(exefs, name):
    generation = generate(exefs, name)
    sources = sorted((ROOT / 'work/generated' / name).glob('*.c'))
    with tempfile.TemporaryDirectory(prefix='check-c-') as temp:
        response = Path(temp) / 'syntax.rsp'
        response.write_text(response_args(['-std=gnu11', *cpu_flags(), '-fsyntax-only',
                                          '-I', str(VENDOR / 'runtime'), *map(str, sources)]), encoding='utf-8')
        metrics = run(response_command(response), cwd=ROOT, log=ROOT / 'work/logs' / f'check-c-{name}.log')
    result = {'module': name, 'translation_units': len(sources), 'status': 'syntax_checked',
              'unsupported_instructions': generation['unsupported_instructions'],
              'generated_files': generation['files'],
              'runtime_headers': {p.name: digest(p) for p in (VENDOR / 'runtime').glob('*.h')},
              **metrics}
    (ROOT / 'work' / f'check-c-{name}.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def probe(exefs):
    generation = generate(exefs, 'rtld')
    output = ROOT / 'work/probe'
    library = output / ('rtld' + library_suffix())
    sources = sorted((ROOT / 'work/generated/rtld').glob('*.c'))
    with tempfile.TemporaryDirectory(prefix='probe-') as temp:
        candidate = Path(temp) / library.name
        build = run(['clang', '-std=gnu11', '-O2', '-fPIC', '-shared', *cpu_flags(), '-I', str(VENDOR / 'runtime'),
                     *map(str, sources), str(ROOT / 'runtime/probe.c'), str(ROOT / 'runtime/hle.c'), '-lm', '-o', str(candidate)],
                    cwd=ROOT, log=ROOT / 'work/logs/probe-build.log', seconds=120)
        result_path = Path(temp) / 'result.json'
        execution = run([sys.executable, str(ROOT / 'tools/probe_rtld.py'), '--exefs', str(exefs),
                         '--library', str(candidate), '--result', str(result_path)], cwd=ROOT,
                        log=ROOT / 'work/logs/probe-run.log', seconds=120)
        result = json.loads(result_path.read_text())
        result.update(build=build, execution=execution, library_sha256=digest(candidate),
                      generation_inputs=generation['inputs'])
        candidate.replace(library)
        (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('inspect', 'generate', 'check-c', 'probe'))
    parser.add_argument('--exefs', required=True, type=Path)
    parser.add_argument('--module', choices=(*profile()['modules'], 'all'), default='rtld')
    args = parser.parse_args()
    try:
        prepare_work()
        exefs = args.exefs.resolve()
        if args.command == 'inspect':
            result = inspect(exefs)
        elif args.command in ('generate', 'check-c'):
            names = profile()['modules'] if args.module == 'all' else (args.module,)
            action = generate if args.command == 'generate' else check_c
            result = {name: action(exefs, name) for name in names}
            result = {name: {k: v for k, v in record.items() if k not in ('inputs', 'files', 'generated_files', 'runtime_headers')}
                      for name, record in result.items()}
        else:
            result = probe(exefs)
            result = {k: v for k, v in result.items() if k != 'generation_inputs'}
        print(json.dumps(result, indent=2))
        if args.command in ('generate', 'check-c') and any(r['unsupported_instructions'] for r in result.values()):
            parser.exit(2, 'generated code contains explicit unsupported-instruction traps; see report\n')
    except (ValueError, OSError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
