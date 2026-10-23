"""Build and run synthetic Vulkan compute or indexed-draw checks on Windows."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

from bounded import run
from build import write_json
from host import lock_file, response_args, response_command
from recompile import ROOT, digest, prepare_work
from vulkan_probe import HEADERS, header_hashes


def probe(mode):
    if os.name != 'nt' or mode not in ('compute', 'draw'):
        raise ValueError('expected Windows compute or draw probe')
    prepare_work()
    directory = ROOT / f'work/vulkan-{mode}'
    if directory.is_symlink(): raise ValueError('probe output must not be a symlink')
    directory.mkdir(exist_ok=True)
    for name in ('probe.exe', 'result.json', 'result.json.partial', 'probe.lock'):
        if (directory / name).is_symlink(): raise ValueError('probe output must not be a symlink')
    sources = ['vulkan_host.c', 'vulkan_host.h', 'vulkan_windows.c', 'vulkan_windows.h', 'vulkan_shader_checks.h']
    sources += (['vulkan_compute.c', 'vulkan_compute.h'] if mode == 'compute' else
                ['vulkan_render.c', 'vulkan_render.h', 'vulkan_color.h', 'vulkan_draw.h', 'vulkan_draw.inc',
                 'vulkan_pixels.inc', 'vulkan_image_io.inc'])
    fixture = ROOT / f'tests/native_vulkan_{mode}_probe.c'
    shader = ROOT / f'tests/vulkan_{mode}_spirv.h'
    with (directory / 'probe.lock').open('a') as lock:
        lock_file(lock)
        record = dict(status='stopped', game_executed=False, headers=header_hashes(), native=None,
                      sources={p: digest(ROOT / 'runtime' / p) for p in sources},
                      fixture_sha256=digest(fixture), shader_sha256=digest(shader),
                      compiler=subprocess.check_output(['clang', '--version'], text=True, timeout=10))
        with tempfile.TemporaryDirectory(prefix='vulkan-shader-probe-', dir=ROOT / 'work/tmp') as temp:
            temp = Path(temp)
            arguments = ['-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                         '-I', str(ROOT / 'runtime'), '-I', str(HEADERS / 'include'),
                         *[str(ROOT / 'runtime' / p) for p in sources if p.endswith('.c')],
                         str(fixture), '-o', str(temp / 'probe.exe')]
            response = temp / 'compile.rsp'
            response.write_text(response_args(arguments), encoding='utf-8')
            record['link'] = run(response_command(response), cwd=ROOT, log=ROOT / f'work/logs/vulkan-{mode}-link.log', seconds=60)
            (temp / 'probe.exe').replace(directory / 'probe.exe')
        record['executable_sha256'] = digest(directory / 'probe.exe')
        try:
            log = ROOT / f'work/logs/vulkan-{mode}-probe.log'
            record['execution'] = run([str(directory / 'probe.exe')], cwd=ROOT, log=log, seconds=30)
            native = json.loads(log.read_text(encoding='utf-8'))
            expected = ({'synthetic_compute_verified': True, 'dispatches': 3, 'verified_words': 96} if mode == 'compute' else
                        {'synthetic_draw_verified': True, 'indexed_triangles': 4, 'changed_pixels': 1152, 'verified_pixels': 16384,
                         'alpha_blend_verified': True, 'depth_sampling_verified': True, 'stencil_rejection_verified': True})
            if native.get('game_executed') is not False or any(native.get(k) != v for k, v in expected.items()):
                raise ValueError('invalid synthetic shader proof')
            record.update(status=f'vulkan_{mode}_verified', native=native)
        finally:
            write_json(directory / 'result.json', record)
    return record


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('compute', 'draw'))
    args = parser.parse_args()
    try: print(json.dumps(probe(args.mode), indent=2))
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error: parser.exit(1, str(error) + '\n')
