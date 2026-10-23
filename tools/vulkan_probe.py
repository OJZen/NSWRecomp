"""Build and run a bounded Windows Vulkan transfer check; no game inputs or window."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from bounded import run
from build import write_json
from host import executable, lock_file, response_args, response_command
from recompile import ROOT, digest, prepare_work

HEADERS = ROOT / 'third_party/vulkan-headers'
SOURCES = ('vulkan_host.c', 'vulkan_host.h', 'vulkan_windows.c', 'vulkan_windows.h')


def header_hashes():
    manifest = json.loads((HEADERS / 'SOURCE.json').read_text(encoding='utf-8'))
    hashes = {}
    for name, expected in manifest['files'].items():
        path = HEADERS / name
        if path.is_symlink() or digest(path) != expected:
            raise ValueError('Vulkan header provenance mismatch: ' + name)
        hashes[name] = expected
    return {'manifest_sha256': digest(HEADERS / 'SOURCE.json'), 'files': hashes}


def probe(adapter=-1, render_clear=False):
    if os.name != 'nt': raise ValueError('this hardware probe requires Windows')
    prepare_work()
    directory = ROOT / ('work/vulkan-render' if render_clear else 'work/vulkan')
    log_prefix = 'vulkan-render' if render_clear else 'vulkan'
    selected_sources = SOURCES + (('vulkan_render.c', 'vulkan_render.h', 'vulkan_color.h', 'vulkan_image_io.inc', 'vulkan_pixels.inc', 'vulkan_draw.h', 'vulkan_draw.inc', 'vulkan_shader_checks.h') if render_clear else ())
    if directory.is_symlink(): raise ValueError('Vulkan output must not be a symlink')
    directory.mkdir(exist_ok=True)
    for name in ('probe.exe', 'result.json', 'result.json.partial', 'probe.lock'):
        if (directory / name).is_symlink(): raise ValueError('Vulkan output file must not be a symlink')
    headers = header_hashes()
    sources = {name: digest(ROOT / 'runtime' / name) for name in selected_sources}
    test_source = ROOT / 'tests/native_vulkan_probe.c'
    with (directory / 'probe.lock').open('a') as lock:
        lock_file(lock)
        with tempfile.TemporaryDirectory(prefix='vulkan-probe-', dir=ROOT / 'work/tmp') as temp:
            temp = Path(temp)
            candidate = temp / executable('probe')
            flags = ['-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                     '-I', str(ROOT / 'runtime'), '-I', str(HEADERS / 'include')]
            if render_clear: flags += ['-DNSW_RENDER_PROBE']
            response = temp / 'compile.rsp'
            response.write_text(response_args([*flags, *[str(ROOT / 'runtime' / name) for name in selected_sources if name.endswith('.c')],
                                               str(test_source), '-o', str(candidate)]), encoding='utf-8')
            link = run(response_command(response), cwd=ROOT, log=ROOT / 'work/logs' / f'{log_prefix}-link.log', seconds=60)
            candidate.replace(directory / executable('probe'))
        record = {'status': 'vulkan_probe_stopped', 'adapter_requested': adapter, 'native': None,
                  'game_executed': False, 'rendering_verified': False, 'headers': headers,
                  'runtime_sources': sources, 'probe_source_sha256': digest(test_source), 'flags': flags,
                  'executable_sha256': digest(directory / executable('probe')), 'link': link,
                  'compiler': subprocess.check_output(['clang', '--version'], text=True, timeout=10)}
        try:
            record['execution'] = run([str(directory / executable('probe')), str(adapter)], cwd=ROOT,
                                      log=ROOT / 'work/logs' / f'{log_prefix}-probe.log', seconds=30)
            native = json.loads((ROOT / 'work/logs' / f'{log_prefix}-probe.log').read_text(encoding='utf-8'))
            if (native.get('status') != 'vulkan_transfer_verified' or native.get('game_executed') is not False
                    or native.get('rendering_verified') is not False
                    or [p['bytes'] for p in native.get('transfers', [])] != [4, 4096, 65536, 1048576]):
                raise ValueError('invalid Vulkan probe report')
            if render_clear and (native.get('color_attachment_clear_verified') is not True or native.get('srgb_clear_verified') is not True
                                 or native.get('rgb11f_clear_verified') is not True or native.get('r8_clear_verified') is not True or native.get('clear_passes') != 12
                                 or native.get('verified_pixels') != 7866876 or native.get('depth_stencil_verified') is not True
                                 or native.get('depth_clear_passes') != 3 or native.get('depth_verified_pixels') != 2764800
                                 or native.get('image_copy_verified') is not True or native.get('image_copies') != 10
                                 or native.get('rgb11_to_rgba16f_verified') is not True or native.get('hdr_to_unorm_srgb_verified') is not True
                                 or native.get('nonuniform_depth_resize_verified') is not True or native.get('nonuniform_linear_resize_verified') is not True):
                raise ValueError('invalid Vulkan color attachment proof')
            record.update(status='vulkan_color_attachment_verified' if render_clear else 'vulkan_transfer_verified', native=native)
        finally:
            write_json(directory / 'result.json', record)
    return record


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--adapter', type=int, choices=range(-1, 32), default=-1, metavar='-1..31')
    parser.add_argument('--render-clear', action='store_true', help='also execute color-attachment passes with full image readback')
    args = parser.parse_args()
    try:
        print(json.dumps(probe(args.adapter, args.render_clear), indent=2))
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, str(error) + '\n')
