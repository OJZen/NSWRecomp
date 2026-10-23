"""Execute a bounded SDK startup path using the already compiled guest functions."""
import argparse
from contextlib import nullcontext
import json
import os
import plistlib
import re
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

from bounded import run
from build import BUILD, FLAGS, RUNTIME, object_list, write_json
from host import executable as native_name, lock_file, response_args, response_command
from load import ADDRESS_UNAVAILABLE, Module, symbol_scope, verify_build, write_plan
from recompile import ROOT, VENDOR, Nso, digest, prepare_work, verify
from resources import verified_romfs, resource_child_options
from npdm import read_policy

STAGES = ('_init_libc0', 'nnosInitialize', '_init_libc1', 'nninitInitializeSdkModule',
          'nninitStartup', '_init_libc2', 'nnMain')
CORE_SOURCES = (*RUNTIME, 'startup.c', 'kernel.c', 'kernel.h', 'threads.c', 'threads.h',
                'host.c', 'host.h', 'mach_memory.h')
BASIC_SOURCES = (*CORE_SOURCES, 'services.c', 'services.h', 'filesystem.h', 'audio.h',
                 'guest_callback.c', 'guest_callback.h', 'graphics_allocator.c', 'graphics_allocator.h',
                 'time_clock.c', 'time_clock.h', 'romfs.c', 'romfs.h', 'audio_out.h', 'audio_out_ipc.inc')
SOURCES = (*BASIC_SOURCES, 'filesystem.c', 'audio.c',
           'audio_mixer.c', 'audio_mixer.h', 'audio_voice.h', 'audio_output.inc', 'audio_effects.inc')
VULKAN_SOURCES = ('vulkan_host.c', 'vulkan_host.h', 'vulkan_windows.c', 'vulkan_windows.h',
                  'audio_headless.c', 'audio_voice.h', 'audio_out.c',
                  'graphics/tex/nvn_deswizzle.c', 'graphics/tex/nvn_deswizzle.h', 'vulkan_texture_io.inc',
                  'vulkan_render.c', 'vulkan_render.h', 'vulkan_color.h', 'vulkan_shader_capture.inc',
                  'vulkan_compute.c', 'vulkan_compute.h', 'vulkan_shader.inc', 'vulkan_image_io.inc', 'vulkan_pixels.inc',
                  'vulkan_draw.h', 'vulkan_draw.inc', 'vulkan_draw_commands.inc', 'vulkan_shader_checks.h',
                  'vulkan_runtime.c', 'vulkan_runtime.h', 'vulkan_bridge.c', 'nvn_bridge.h',
                  'graphics/bridge.c', 'graphics/input.c', 'graphics/input.h', 'graphics/include/host_compat.h',
                  'vulkan_display.c', 'vulkan_display.h', 'vulkan_nvn.inc', 'vulkan_nvn_entries.inc',
                  'graphics/nvn/nvn_obj.h', 'graphics/nvn/nvn_tex.h', 'graphics/nvn/nvn_tex.c', 'vulkan_texture.inc', 'vulkan_commands.inc', 'vulkan_state.inc', 'vulkan_descriptor.inc', 'vulkan_program.inc')
VULKAN_VIDEO_SOURCES = ('graphics/movie.c', 'graphics/movie.h', 'graphics/movie_video.c',
                        'graphics/movie_video.h', 'graphics/movie_audio.c', 'graphics/movie_audio.h', 'graphics/codec.c', 'graphics/guest_symbols.inc')
APP = ROOT / 'work/app/TOTK Recompiled.app'
GAME_APP = APP / 'Contents/Helpers/TOTK Runtime.app'
NATIVE_APP = GAME_APP / 'Contents/MacOS/totk-recomp'
PROCESS = ROOT / 'work/session/guest.json'
LAUNCHER_SOURCE = ROOT / 'runtime/macos_launcher.m'


def process_identity(pid):
    if os.name == 'nt':
        # The macOS launcher uses ps identity strings; Windows runs are owned by
        # the enclosing Job Object and do not support launcher attachment.
        return None
    result = subprocess.run(['ps', '-p', str(pid), '-o', 'lstart=', '-o', 'comm='], capture_output=True,
                            text=True, timeout=5, env=dict(os.environ, LC_ALL='C'))
    return result.stdout.strip() if result.returncode == 0 else None


def bundle(executable):
    for directory in (ROOT / 'work/app', APP, APP / 'Contents', APP / 'Contents/MacOS', APP / 'Contents/Helpers',
                      GAME_APP, GAME_APP / 'Contents', NATIVE_APP.parent):
        if directory.is_symlink(): raise ValueError('app bundle must not be a symlink')
        directory.mkdir(exist_ok=True)
    launch = APP / 'Contents/MacOS/launch'
    for path in (NATIVE_APP, NATIVE_APP.with_suffix('.partial'), launch,
                 launch.with_suffix('.partial'), APP / 'Contents/Info.plist', GAME_APP / 'Contents/Info.plist'):
        if path.is_symlink(): raise ValueError('app bundle file must not be a symlink')
    partial = NATIVE_APP.with_suffix('.partial')
    partial.unlink(missing_ok=True)
    os.link(executable, partial); partial.replace(NATIVE_APP)
    candidate = launch.with_suffix('.partial')
    try:
        subprocess.run(['clang', '-fobjc-arc', '-O2', '-Wno-deprecated-declarations',
                        str(LAUNCHER_SOURCE), '-framework', 'Cocoa', '-o', str(candidate)],
                       check=True, capture_output=True, text=True, timeout=30,
                       env=dict(os.environ, TMPDIR=str(ROOT / 'work/tmp')))
        if launch.is_file() and digest(launch) == digest(candidate): candidate.unlink()
        else: candidate.replace(launch)
    finally:
        candidate.unlink(missing_ok=True)
    with (APP / 'Contents/Info.plist').open('wb') as stream:
        plistlib.dump(dict(CFBundleIdentifier='local.totk-recomp', CFBundleName='TOTK Recompiled',
                          CFBundleExecutable='launch', CFBundlePackageType='APPL', CFBundleVersion='0.0.1',
                          LSMinimumSystemVersion='26.0', NSHighResolutionCapable=True, NSPrincipalClass='NSApplication',
                          NSRemovableVolumesUsageDescription='读取你放在外置盘上的游戏数据、存档与本项目源码。'), stream)
    with (GAME_APP / 'Contents/Info.plist').open('wb') as stream:
        plistlib.dump(dict(CFBundleIdentifier='local.totk-recomp.runtime', CFBundleName='TOTK Recompiled',
                          CFBundleExecutable='totk-recomp', CFBundlePackageType='APPL', CFBundleVersion='0.0.1',
                          LSMinimumSystemVersion='26.0', NSHighResolutionCapable=True, NSPrincipalClass='NSApplication'), stream)


def platform_flags(kernel_only=False, basic_services=False):
    if os.name == 'nt':
        if not (kernel_only or basic_services):
            raise ValueError('Windows startup requires --kernel-only or --basic-services')
        return ['-pthread', '-lbcrypt', '-Wl,--stack,67108864']
    if kernel_only or basic_services:
        return ['-pthread']
    return shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'opus'], text=True, timeout=10)) + [
        '-framework', 'AudioToolbox', '-framework', 'CoreAudio']


def entries(modules, through='nnMain'):
    scope = symbol_scope(modules)
    addresses = []
    for name in STAGES[:STAGES.index(through) + 1]:
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
            or (returncode and report.get('stop_reason') == 'checkpoint')
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


def worker(exefs, through, executable, output, romfs=None, seconds=30, kernel_only=False,
           basic_services=False, npdm_sha256=None, vulkan=False, adapter=-1, romfs_profile=None, scratch_filesystem=False):
    if vulkan and (os.name != 'nt' or not basic_services or kernel_only):
        raise ValueError('headless Vulkan requires Windows basic-services mode without Metal')
    policy = read_policy(exefs / 'main.npdm', npdm_sha256) if basic_services else None
    infos = verify(exefs)
    modules = []
    for name, info in infos.items():
        n = Nso(exefs / name)
        if digest(exefs / name) != info['sha256']:
            raise ValueError('startup input changed while reading')
        modules.append(Module(name, int(info['base'], 0), n))
    addresses = entries(modules, through)
    helper_bindings = {}
    if vulkan:
        helper_name = b'_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE'
        binding = symbol_scope(modules).get(helper_name)
        if binding:
            owner, symbol = binding
            if symbol[2] != 2 or not owner.nso.text[0] <= symbol[5] < sum(owner.nso.text):
                raise ValueError('SystemEvent helper is not executable code')
            helper_bindings['NSW_VSYNC_ATTACH'] = hex(owner.defined_value(symbol))
    plan = output.parent / 'image.plan'
    write_plan(plan, modules)
    # The native child loads this plan; keeping parsed images would duplicate its memory.
    del modules, n
    environment = dict(os.environ)
    environment.pop('NSW_SERVICE_ACCESS', None)
    environment.pop('NSW_VSYNC_ATTACH', None)
    environment.update(helper_bindings)
    environment.pop('NSW_VULKAN_ADAPTER', None)
    if vulkan: environment['NSW_VULKAN_ADAPTER'] = str(adapter)
    environment.pop('NSW_PROGRAM_ID', None)
    if policy is not None:
        environment['NSW_PROGRAM_ID'] = policy['program_id']
        environment['NSW_SERVICE_ACCESS'] = ','.join(policy['client_services'])
    environment['TOTK_RUN_SECONDS'] = str(seconds)
    environment.setdefault('DM_NVN_TBDR', '0')
    environment.setdefault('ZELDA_FS_COMPUTE', '1')
    environment.setdefault('ZELDA_SUBMIT_FIRST', '6')
    environment.setdefault('ZELDA_SUBMIT_SECOND', '16')
    environment.update(DM_PSO_MANIFEST='off', DM_PRESENT_LINK='0', DM_MEM_CAP_GB='4',
                       DM_NVN_SHMAP=str(ROOT / 'work/shaders/shader_map.tsv'),
                       DM_MSL_DIR=str(ROOT / 'work/shaders'), DM_PSO_USER=str(ROOT / 'work/shaders/pso_user.tsv'))
    guard = verified_romfs(romfs, romfs_profile) if romfs else nullcontext((None, None))
    scratch = tempfile.TemporaryDirectory(prefix='scratch-fs-', dir=ROOT / 'work/tmp') if scratch_filesystem else nullcontext(None)
    with guard as (resource, stream), scratch as scratch_root:
        environment.pop('NSW_SCRATCH_FS', None)
        if scratch_root: environment['NSW_SCRATCH_FS'] = scratch_root
        if resource and romfs_profile and resource.get('exefs_sha256') != {name: info['sha256'] for name, info in infos.items()}:
            raise ValueError('RomFS profile does not match the selected ExeFS modules')
        if resource: environment['ZELDA_ROMFS'] = resource['path']
        else: environment.pop('ZELDA_ROMFS', None)
        command = [str(executable), str(plan), str(STAGES.index(through) + 1), *map(hex, addresses)]
        timed_out = False
        for attempt in range(1, 9):
            if PROCESS.parent.is_symlink() or PROCESS.is_symlink(): raise ValueError('process receipt must not be a symlink')
            PROCESS.parent.mkdir(exist_ok=True)
            with resource_child_options(stream, environment) as options, subprocess.Popen(
                    command, stdout=subprocess.PIPE, text=True, encoding='utf-8', env=environment, **options) as child:
                try:
                    write_json(PROCESS, dict(pid=child.pid, executable=str(executable), identity=process_identity(child.pid)))
                    try:
                        stdout, _ = child.communicate(timeout=seconds)
                    except subprocess.TimeoutExpired:
                        timed_out = True
                        child.kill()
                        stdout, _ = child.communicate(timeout=5)
                        print('STARTUP native execution time budget exceeded', file=sys.stderr, flush=True)
                    result = subprocess.CompletedProcess(command, 124 if timed_out else child.returncode, stdout)
                finally:
                    if child.poll() is None: child.kill(); child.wait()
                    if PROCESS.is_file() and json.loads(PROCESS.read_text(encoding='utf-8')).get('pid') == child.pid: PROCESS.unlink()
            if result.returncode != ADDRESS_UNAVAILABLE:
                break
    native = None if timed_out else native_report(result.stdout, result.returncode, through, graphics=executable == NATIVE_APP)
    if native is not None and native.get('kernel_only') is not kernel_only:
        raise ValueError('native startup mode differs from the requested runtime')
    mode = 'vulkan_headless' if vulkan else 'kernel_only' if kernel_only else 'basic_services' if basic_services else 'full'
    if native is not None and native.get('runtime_mode') != mode:
        raise ValueError('native service backend differs from the requested runtime')
    status = 'startup_stopped' if result.returncode else 'window_closed' if native.get('window_closed') else 'startup_checkpoint'
    write_json(output, {'status': status,
                       'exit_code': result.returncode, 'through': through, 'native': native,
                       'address_space_attempts': attempt, 'title_verified': False,
                       'resource': resource, 'kernel_only': kernel_only, 'timed_out': timed_out,
                       'runtime_mode': mode, 'service_policy': policy, 'scratch_filesystem': scratch_filesystem, 'helper_bindings': helper_bindings,
                       'vulkan_adapter_requested': adapter if vulkan else None,
                       'input_sha256': {name: info['sha256'] for name, info in infos.items()}})


def startup(exefs, through, romfs=None, graphics=False, seconds=30, kernel_only=False,
            basic_services=False, npdm_sha256=None, vulkan=False, adapter=-1, romfs_profile=None, scratch_filesystem=False, vulkan_video=False):
    if kernel_only and basic_services:
        raise ValueError('choose one startup runtime mode')
    if (kernel_only and (graphics or romfs)) or (basic_services and graphics):
        raise ValueError('the selected startup mode does not include the requested backend')
    if vulkan and (os.name != 'nt' or not basic_services or kernel_only or graphics):
        raise ValueError('headless Vulkan requires Windows basic-services mode without Metal')
    if vulkan_video and not vulkan:
        raise ValueError('Vulkan video requires the Windows Vulkan backend')
    if romfs_profile and not romfs:
        raise ValueError('--romfs-profile requires --romfs')
    if basic_services and romfs and not romfs_profile:
        raise ValueError('basic services require an explicit --romfs-profile')
    if scratch_filesystem and (os.name != 'nt' or not basic_services or kernel_only):
        raise ValueError('scratch filesystem requires Windows basic services')
    if basic_services:
        read_policy(exefs / 'main.npdm', npdm_sha256)  # Fail before linking or executing.
    flags = platform_flags(kernel_only, basic_services)
    sources = CORE_SOURCES if kernel_only else BASIC_SOURCES if basic_services else SOURCES
    if scratch_filesystem:
        sources += ('filesystem_windows.c',)
        flags += ['-DNSW_SCRATCH_FS']
    vulkan_headers = None
    video_dependency = None
    if vulkan:
        from vulkan_probe import HEADERS, header_hashes
        vulkan_headers = header_hashes()
        sources += VULKAN_SOURCES
        flags += ['-DNSW_VULKAN', '-DNSW_HEADLESS_AUDIO', '-I', str(HEADERS / 'include'), '-I', str(ROOT / 'runtime/graphics/include')]
    if vulkan_video:
        from prepare_vulkan_video import verified_dependency
        video_dependency, video_flags = verified_dependency()
        sources += VULKAN_VIDEO_SOURCES
        flags += ['-DNSW_VULKAN_VIDEO', *video_flags]
    prepare_work()
    directory = ROOT / 'work/startup'
    if directory.is_symlink():
        raise ValueError('startup output must not be a symlink')
    directory.mkdir(exist_ok=True)
    if any(p.is_symlink() or p.name not in (native_name('probe'), 'result.json', 'result.json.partial') for p in directory.iterdir()):
        raise ValueError('unrecognized startup output')
    with (BUILD / 'build.lock').open('a') as lock:
        lock_file(lock)
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
            if graphics:
                graphics_flags += ['-Xlinker', '-object_path_lto', '-Xlinker', str(temp / 'lto')]
            candidate = temp / native_name('probe')
            (temp / 'module_tables.inc').write_text(''.join(f'RT_MODULE({name})\n' for name in build['generation']), encoding='utf-8')
            response = temp / 'link.rsp'
            nvn_sources, nvn_flags, nvn_receipt = [], [], None
            if vulkan:
                from nvn_bridge import HEADERS as NVN_HEADERS, generate
                nvn_inputs = ROOT / 'runtime/graphics/nvn'
                extra = (nvn_inputs / 'nvn_extra_protos.h').read_text(encoding='utf-8')
                names = re.findall(r'"(nvn\w+)"', (nvn_inputs / 'nvn_names.c').read_text(encoding='utf-8'))
                nvn_source, unsupported = generate((NVN_HEADERS / 'nvn_protos.h').read_text(encoding='utf-8') + extra,
                                                    (NVN_HEADERS / 'include/nvn.h').read_text(encoding='utf-8') + extra,
                                                    names, 'nvn_extra_protos.h')
                nvn_file = temp / 'nvn_bridge.c'; nvn_file.write_text(nvn_source, encoding='utf-8')
                nvn_sources = [str(nvn_file)]
                if vulkan_video:
                    modules = [Module(name, int(info['base'], 0), Nso(exefs / name)) for name, info in verify(exefs).items()]
                    exports = {name.decode(): owner.defined_value(symbol) for name, (owner, symbol) in symbol_scope(modules).items()
                               if symbol[2] == 2 and owner.nso.text[0] <= symbol[5] < sum(owner.nso.text)}
                    symbols = re.findall(r'^GUEST\((\w+)\)$', (ROOT / 'runtime/graphics/guest_symbols.inc').read_text(), re.M)
                    if not symbols or len(set(symbols)) != len(symbols) or any(name not in exports for name in symbols):
                        raise ValueError('missing or duplicate compiled video/codec helper')
                    helper = temp / 'guest_symbols.c'
                    helper.write_text('#include <stdint.h>\n#include <string.h>\nuint64_t graphics_guest_export(const char *name) {\n' +
                        ''.join(f'if (!strcmp(name,"{name}")) return {exports[name]:#x}ull;\n' for name in symbols) + 'return 0;\n}\n', encoding='utf-8')
                    nvn_sources.append(str(helper)); video_dependency = dict(video_dependency, helper_sha256=digest(helper))
                    del modules
                nvn_flags = ['-I', str(ROOT / 'runtime'), '-I', str(NVN_HEADERS), '-I', str(NVN_HEADERS / 'include'), '-I', str(nvn_inputs)]
                nvn_receipt = {'source_sha256': digest(nvn_file), 'unsupported_abi': unsupported,
                               'generator_sha256': digest(ROOT / 'tools/nvn_bridge.py'),
                               'supplemental': {name: digest(nvn_inputs / name) for name in ('nvn_extra_protos.h', 'nvn_names.c')},
                               'headers': {name: digest(NVN_HEADERS / name) for name in ('nvn_protos.h', 'include/nvn.h')}}
            command = [*FLAGS, *(['-municode'] if os.name == 'nt' else []),
                       '-DTOTK_LINK_ALL', '-DTOTK_STARTUP', '-DNSW_MODULE_TABLES', '-I', str(temp),
                       *(['-DNSW_KERNEL_ONLY'] if kernel_only else []), '-I', str(VENDOR / 'runtime'),
                       *(['-DNSW_BASIC_SERVICES'] if basic_services else []),
                       *[str(ROOT / 'runtime' / name) for name in sources if name.endswith('.c')],
                       *nvn_sources, *map(str, objects), *flags, *nvn_flags, *graphics_flags, '-lm', '-o', str(candidate)]
            response.write_text(response_args(command) + '\n', encoding='utf-8')
            link = run(response_command(response), cwd=ROOT, log=ROOT / 'work/logs/startup-link.log', seconds=120,
                       rss_limit=(2048 if graphics else 1536) << 20)
            candidate.replace(directory / native_name('probe'))
            if graphics: bundle(directory / native_name('probe'))
            output = temp / 'result.json'
            command = [sys.executable, str(Path(__file__).resolve()), '--exefs', str(exefs),
                       '--through', through, '--worker-result', str(output), '--seconds', str(seconds)]
            if romfs: command += ['--romfs', str(romfs.resolve())]
            if romfs_profile: command += ['--romfs-profile', str(romfs_profile.resolve())]
            if scratch_filesystem: command += ['--scratch-filesystem']
            if graphics: command += ['--graphics']
            if kernel_only: command += ['--kernel-only']
            if basic_services: command += ['--basic-services', '--npdm-sha256', npdm_sha256]
            if vulkan: command += ['--vulkan-headless', '--vulkan-adapter', str(adapter)]
            execution = run(command,
                            cwd=ROOT, log=ROOT / 'work/logs/startup.log', seconds=seconds + 90,
                            rss_limit=(3072 if graphics else 1536) << 20)
            result = json.loads(output.read_text())
            result.update(link=link, execution=execution, executable_sha256=digest(directory / native_name('probe')),
                          runtime_sources={name: digest(ROOT / 'runtime' / name) for name in sources},
                          build_context=build['context'],
                          planner_sha256=digest(Path(__file__)), graphics=graphics_result,
                          vulkan_headers=vulkan_headers, nvn_bridge=nvn_receipt, vulkan_video=video_dependency)
            if graphics:
                result['launcher'] = dict(source_sha256=digest(LAUNCHER_SOURCE),
                                          executable_sha256=digest(APP / 'Contents/MacOS/launch'))
            write_json(directory / 'result.json', result)
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exefs', type=Path, required=True)
    parser.add_argument('--through', choices=STAGES, default='nnMain')
    parser.add_argument('--romfs', type=Path, help='raw merged RomFS; verified and opened read-only')
    parser.add_argument('--scratch-filesystem', action='store_true', help='fresh Windows scratch filesystem; discarded after each run')
    parser.add_argument('--romfs-profile', type=Path, help='resource size, SHA-256 and ExeFS hashes; required for basic services')
    parser.add_argument('--graphics', action='store_true', help='attach native Metal and an AppKit window')
    parser.add_argument('--kernel-only', action='store_true', help='run kernel/threads only; stop at IPC, with no filesystem or audio services')
    parser.add_argument('--basic-services', action='store_true', help='enable basic IPC/application and optional read-only RomFS; no save or audio output')
    parser.add_argument('--vulkan-headless', action='store_true', help='opt into bounded offscreen Windows Vulkan execution with --basic-services')
    parser.add_argument('--vulkan-video', action='store_true', help='enable the movie bridge with prepared Windows libvpx and Vorbis dependencies')
    parser.add_argument('--vulkan-adapter', type=int, choices=range(-1, 32), default=-1, metavar='-1..31')
    parser.add_argument('--npdm-sha256', help='expected SHA-256 of ExeFS/main.npdm; required by --basic-services')
    parser.add_argument('--seconds', type=int, choices=range(1, 601), metavar='1..600', default=30)
    parser.add_argument('--worker-result', type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    try:
        if args.worker_result:
            worker(args.exefs, args.through, NATIVE_APP if args.graphics else ROOT / 'work/startup' / native_name('probe'),
                   args.worker_result, args.romfs, args.seconds, args.kernel_only, args.basic_services, args.npdm_sha256,
                   args.vulkan_headless, args.vulkan_adapter, args.romfs_profile, args.scratch_filesystem)
        else:
            result = startup(args.exefs.resolve(), args.through, args.romfs, args.graphics, args.seconds,
                             args.kernel_only, args.basic_services, args.npdm_sha256, args.vulkan_headless, args.vulkan_adapter, args.romfs_profile, args.scratch_filesystem, args.vulkan_video)
            print(json.dumps(result, indent=2))
            if result['exit_code']:
                parser.exit(2, 'startup stopped before its requested checkpoint; see work/logs/startup.log\n')
    except (ValueError, KeyError, OSError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, f'{error}\n')
