"""Execute a bounded SDK startup path using the already compiled guest functions."""
import argparse
import fcntl
import json
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

from bounded import run
from build import BUILD, FLAGS, RUNTIME, object_list, write_json
from load import ADDRESS_UNAVAILABLE, Module, symbol_scope, verify_build, write_plan
from recompile import ROOT, VENDOR, Nso, digest, prepare_work, verify

STAGES = ('_init_libc0', 'nnosInitialize', '_init_libc1', 'nninitInitializeSdkModule',
          'nninitStartup', '_init_libc2', 'nnMain')
SOURCES = (*RUNTIME, 'startup.c', 'kernel.c', 'kernel.h', 'threads.c', 'threads.h')


def entries(modules):
    scope = symbol_scope(modules)
    addresses = []
    for name in STAGES:
        if name.encode() not in scope:
            raise ValueError(f'missing startup export: {name}')
        owner, symbol = scope[name.encode()]
        if symbol[2] != 2 or not owner.nso.text[0] <= symbol[5] < sum(owner.nso.text):
            raise ValueError('startup export is not executable code')
        addresses.append(owner.defined_value(symbol))
    return addresses


def worker(exefs, through, executable, output):
    infos = verify(exefs)
    modules = []
    for name, info in infos.items():
        n = Nso(exefs / name)
        if digest(exefs / name) != info['sha256']:
            raise ValueError('startup input changed while reading')
        modules.append(Module(name, int(info['base'], 0), n))
    addresses = entries(modules)
    plan = output.parent / 'image.plan'
    write_plan(plan, modules)
    command = [str(executable), str(plan), str(STAGES.index(through) + 1), *map(hex, addresses)]
    for attempt in range(1, 9):
        result = subprocess.run(command, capture_output=True, text=True, timeout=35)
        if result.returncode != ADDRESS_UNAVAILABLE:
            break
    print(result.stderr, end='', flush=True)
    native = json.loads(result.stdout) if result.returncode == 0 else None
    write_json(output, {'status': 'startup_checkpoint' if native else 'startup_stopped',
                       'exit_code': result.returncode, 'through': through, 'native': native,
                       'address_space_attempts': attempt, 'title_verified': False,
                       'input_sha256': {name: info['sha256'] for name, info in infos.items()}})


def startup(exefs, through):
    prepare_work()
    directory = ROOT / 'work/startup'
    if directory.is_symlink():
        raise ValueError('startup output must not be a symlink')
    directory.mkdir(exist_ok=True)
    if any(p.is_symlink() or p.name not in ('probe', 'result.json', 'result.json.partial') for p in directory.iterdir()):
        raise ValueError('unrecognized startup output')
    with (BUILD / 'build.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        verify_build(verify(exefs))
        build = json.loads((BUILD / 'result.json').read_text())
        objects = object_list(build['generation'], build['context'])
        with tempfile.TemporaryDirectory(prefix='startup-', dir=ROOT / 'work/tmp') as temp:
            temp = Path(temp)
            candidate = temp / 'probe'
            response = temp / 'link.rsp'
            command = [*FLAGS, '-DTOTK_LINK_ALL', '-DTOTK_STARTUP', '-I', str(VENDOR / 'runtime'),
                       *[str(ROOT / 'runtime' / name) for name in SOURCES if name.endswith('.c')],
                       *map(str, objects), '-lm', '-o', str(candidate)]
            response.write_text(shlex.join(command) + '\n')
            link = run(['clang', '@' + str(response)], cwd=ROOT, log=ROOT / 'work/logs/startup-link.log', seconds=120)
            candidate.replace(directory / 'probe')
            output = temp / 'result.json'
            execution = run([sys.executable, str(Path(__file__).resolve()), '--exefs', str(exefs),
                             '--through', through, '--worker-result', str(output)],
                            cwd=ROOT, log=ROOT / 'work/logs/startup.log', seconds=120)
            result = json.loads(output.read_text())
            result.update(link=link, execution=execution, executable_sha256=digest(directory / 'probe'),
                          runtime_sources={name: digest(ROOT / 'runtime' / name) for name in SOURCES},
                          planner_sha256=digest(Path(__file__)))
            write_json(directory / 'result.json', result)
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exefs', type=Path, required=True)
    parser.add_argument('--through', choices=STAGES, default='nnMain')
    parser.add_argument('--worker-result', type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    try:
        if args.worker_result:
            worker(args.exefs, args.through, ROOT / 'work/startup/probe', args.worker_result)
        else:
            result = startup(args.exefs.resolve(), args.through)
            print(json.dumps(result, indent=2))
            if result['exit_code']:
                parser.exit(2, 'startup stopped before its requested checkpoint; see work/logs/startup.log\n')
    except (ValueError, KeyError, OSError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, f'{error}\n')
