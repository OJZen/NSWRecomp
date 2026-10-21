"""Execute a bounded SDK startup path using the already compiled guest functions."""
import argparse
import fcntl
import json
import os
import plistlib
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

from bounded import run
from build import BUILD, FLAGS, RUNTIME, object_list, write_json
from load import ADDRESS_UNAVAILABLE, Module, symbol_scope, verify_build, write_plan
from recompile import ROOT, VENDOR, Nso, digest, prepare_work, verify
from resources import verify_romfs

STAGES = ('_init_libc0', 'nnosInitialize', '_init_libc1', 'nninitInitializeSdkModule',
          'nninitStartup', '_init_libc2', 'nnMain')
SOURCES = (*RUNTIME, 'startup.c', 'kernel.c', 'kernel.h', 'threads.c', 'threads.h',
           'services.c', 'services.h', 'filesystem.c', 'filesystem.h', 'audio.c', 'audio.h',
           'audio_mixer.c', 'audio_mixer.h', 'audio_voice.h', 'audio_output.inc', 'audio_effects.inc', 'mach_memory.h')
APP = ROOT / 'work/app/TOTK Recompiled.app'
NATIVE_APP = APP / 'Contents/MacOS/totk-recomp'
PROCESS = ROOT / 'work/session/guest.json'


def process_identity(pid):
    result = subprocess.run(['ps', '-p', str(pid), '-o', 'lstart=', '-o', 'comm='], capture_output=True,
                            text=True, timeout=5, env=dict(os.environ, LC_ALL='C'))
    return result.stdout.strip() if result.returncode == 0 else None


def bundle(executable):
    for directory in (ROOT / 'work/app', APP, APP / 'Contents', NATIVE_APP.parent):
        if directory.is_symlink(): raise ValueError('app bundle must not be a symlink')
        directory.mkdir(exist_ok=True)
    for path in (NATIVE_APP, NATIVE_APP.with_suffix('.partial'), NATIVE_APP.parent / 'launch', APP / 'Contents/Info.plist'):
        if path.is_symlink(): raise ValueError('app bundle file must not be a symlink')
    partial = NATIVE_APP.with_suffix('.partial')
    partial.unlink(missing_ok=True)
    os.link(executable, partial); partial.replace(NATIVE_APP)
    launch = NATIVE_APP.parent / 'launch'
    launch.write_text('#!/bin/bash\nset -euo pipefail\ncd "$(dirname "$0")/../../../../.."\n'
                      'exec work/venv/bin/python tools/run.py >work/logs/app-launch.log 2>&1\n')
    launch.chmod(0o755)
    with (APP / 'Contents/Info.plist').open('wb') as stream:
        plistlib.dump(dict(CFBundleIdentifier='local.totk-recomp', CFBundleName='TOTK Recompiled',
                          CFBundleExecutable='launch', CFBundlePackageType='APPL', CFBundleVersion='0.0.1',
                          LSMinimumSystemVersion='26.0', NSHighResolutionCapable=True, NSPrincipalClass='NSApplication'), stream)


def platform_flags():
    return shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'opus'], text=True, timeout=10)) + [
        '-framework', 'AudioToolbox', '-framework', 'CoreAudio']


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


def native_report(stdout, returncode, through, graphics=False):
    report = json.loads(stdout) if stdout.strip() else None
    if report is None and returncode:
        return None
    if (not isinstance(report, dict) or report.get('title_verified') is not False
            or type(report.get('completed_stages')) is not int
            or type(report.get('window_closed', False)) is not bool
            or (report.get('window_closed') and not graphics)
            or not 0 <= report['completed_stages'] <= STAGES.index(through) + 1
            or (not returncode and report['completed_stages'] != STAGES.index(through) + 1 and not report.get('window_closed'))
            or type(report.get('function_calls')) is not int or report['function_calls'] < 0
            or any(type(report.get(k)) is not bool for k in ('constructors_completed', 'entered_nnMain', 'initialization_executed'))
            or (report['constructors_completed'] and report['completed_stages'] < 6)
            or (report['entered_nnMain'] and (not report['constructors_completed'] or through != 'nnMain'))):
        raise ValueError('invalid native startup report')
    return report


def worker(exefs, through, executable, output, romfs=None, seconds=30):
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
    # The native child loads this plan; keeping parsed images would duplicate its memory.
    del modules, n
    environment = dict(os.environ)
    environment['TOTK_RUN_SECONDS'] = str(seconds)
    environment.update(DM_NVN_TBDR='0', DM_PSO_MANIFEST='off', DM_PRESENT_LINK='0', DM_MEM_CAP_GB='4',
                       DM_NVN_SHMAP=str(ROOT / 'work/shaders/shader_map.tsv'),
                       DM_MSL_DIR=str(ROOT / 'work/shaders'), DM_PSO_USER=str(ROOT / 'work/shaders/pso_user.tsv'))
    resource = verify_romfs(romfs) if romfs else None
    if resource: environment['ZELDA_ROMFS'] = resource['path']
    else: environment.pop('ZELDA_ROMFS', None)
    command = [str(executable), str(plan), str(STAGES.index(through) + 1), *map(hex, addresses)]
    for attempt in range(1, 9):
        if PROCESS.parent.is_symlink() or PROCESS.is_symlink(): raise ValueError('process receipt must not be a symlink')
        PROCESS.parent.mkdir(exist_ok=True)
        with subprocess.Popen(command, stdout=subprocess.PIPE, text=True, env=environment) as child:
            try:
                write_json(PROCESS, dict(pid=child.pid, executable=str(executable), identity=process_identity(child.pid)))
                stdout, _ = child.communicate(timeout=seconds + 5)
                result = subprocess.CompletedProcess(command, child.returncode, stdout)
            finally:
                if child.poll() is None: child.kill(); child.wait()
                if PROCESS.is_file() and json.loads(PROCESS.read_text()).get('pid') == child.pid: PROCESS.unlink()
        if result.returncode != ADDRESS_UNAVAILABLE:
            break
    native = native_report(result.stdout, result.returncode, through, graphics=executable == NATIVE_APP)
    status = 'startup_stopped' if result.returncode else 'window_closed' if native.get('window_closed') else 'startup_checkpoint'
    write_json(output, {'status': status,
                       'exit_code': result.returncode, 'through': through, 'native': native,
                       'address_space_attempts': attempt, 'title_verified': False,
                       'resource': resource,
                       'input_sha256': {name: info['sha256'] for name, info in infos.items()}})


def startup(exefs, through, romfs=None, graphics=False, seconds=30):
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
        graphics_result, graphics_flags = None, []
        if graphics:
            from graphics import build as build_graphics, link_flags
            for name in ('shaders', 'shaders/source', 'shaders/code', 'shaders/variants'):
                path = ROOT / 'work' / name
                if path.is_symlink(): raise ValueError('shader output must not be a symlink')
                path.mkdir(exist_ok=True)
            mapping = ROOT / 'work/shaders/shader_map.tsv'
            if mapping.is_symlink(): raise ValueError('shader map must not be a symlink')
            if not mapping.exists(): mapping.write_text('package\tindex\tstage\tcontrol\n')
            graphics_objects, graphics_result = build_graphics(exefs)
            objects += graphics_objects
            graphics_flags = ['-DTOTK_GRAPHICS', *link_flags()]
        with tempfile.TemporaryDirectory(prefix='startup-', dir=ROOT / 'work/tmp') as temp:
            temp = Path(temp)
            candidate = temp / 'probe'
            response = temp / 'link.rsp'
            command = [*FLAGS, '-DTOTK_LINK_ALL', '-DTOTK_STARTUP', '-I', str(VENDOR / 'runtime'),
                       *[str(ROOT / 'runtime' / name) for name in SOURCES if name.endswith('.c')],
                       *map(str, objects), *platform_flags(), *graphics_flags, '-lm', '-o', str(candidate)]
            response.write_text(shlex.join(command) + '\n')
            link = run(['clang', '@' + str(response)], cwd=ROOT, log=ROOT / 'work/logs/startup-link.log', seconds=120,
                       rss_limit=(2048 if graphics else 1536) << 20)
            candidate.replace(directory / 'probe')
            if graphics: bundle(directory / 'probe')
            output = temp / 'result.json'
            command = [sys.executable, str(Path(__file__).resolve()), '--exefs', str(exefs),
                       '--through', through, '--worker-result', str(output), '--seconds', str(seconds)]
            if romfs: command += ['--romfs', str(romfs.resolve())]
            if graphics: command += ['--graphics']
            execution = run(command,
                            cwd=ROOT, log=ROOT / 'work/logs/startup.log', seconds=seconds + 90,
                            rss_limit=(3072 if graphics else 1536) << 20)
            result = json.loads(output.read_text())
            result.update(link=link, execution=execution, executable_sha256=digest(directory / 'probe'),
                          runtime_sources={name: digest(ROOT / 'runtime' / name) for name in SOURCES},
                          planner_sha256=digest(Path(__file__)), graphics=graphics_result)
            write_json(directory / 'result.json', result)
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exefs', type=Path, required=True)
    parser.add_argument('--through', choices=STAGES, default='nnMain')
    parser.add_argument('--romfs', type=Path, help='unmodified merged v1.4.3 RomFS; verified read-only')
    parser.add_argument('--graphics', action='store_true', help='attach native Metal and an AppKit window')
    parser.add_argument('--seconds', type=int, choices=range(1, 601), metavar='1..600', default=30)
    parser.add_argument('--worker-result', type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    try:
        if args.worker_result:
            worker(args.exefs, args.through, NATIVE_APP if args.graphics else ROOT / 'work/startup/probe',
                   args.worker_result, args.romfs, args.seconds)
        else:
            result = startup(args.exefs.resolve(), args.through, args.romfs, args.graphics, args.seconds)
            print(json.dumps(result, indent=2))
            if result['exit_code']:
                parser.exit(2, 'startup stopped before its requested checkpoint; see work/logs/startup.log\n')
    except (ValueError, KeyError, OSError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, f'{error}\n')
