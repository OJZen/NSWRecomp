"""Run compiler, loader, kernel, IPC and graphics ABI checks without game data or a GPU."""
import argparse
import json
import os
from pathlib import Path
import sys
import unittest

from bounded import run
# Synthetic checks always select their own fixtures, independently of the shell's
# last real-input experiment. Worker processes inherit this cleared environment.
os.environ.pop('NSWRECOMP_PROFILE', None)
os.environ.pop('NSWRECOMP_OPT_LEVEL', None)
from recompile import ROOT, prepare_work

PROJECT_TESTS = ('test_host.py', 'test_host_pipeline.py', 'test_startup_core.py', 'test_npdm.py', 'test_ipc.py',
                 'test_graphics_allocator.py', 'test_nvn_bridge.py', 'test_vulkan_host.py', 'test_vulkan_nvn.py', 'test_vulkan_shaders.py', 'test_time_clock.py', 'test_build.py', 'test_pipeline.py',
                 'test_load.py', 'test_resources.py', 'test_romfs.py', 'test_filesystem_windows.py', 'test_input.py', 'test_audio_headless.py', 'test_window.py', 'test_import.py', 'test_repository.py')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--worker', choices=('compiler', 'project'), help=argparse.SUPPRESS)
    args = parser.parse_args()
    prepare_work()
    if args.worker:
        loader = unittest.TestLoader()
        if args.worker == 'compiler':
            suite = loader.discover(str(ROOT / 'third_party/lasrecomp/tools'), pattern='test_*.py')
        else:
            suite = unittest.TestSuite(loader.discover(str(ROOT / 'tests'), pattern=name) for name in PROJECT_TESTS)
        return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1
    for suite in ('compiler', 'project'):
        log = ROOT / 'work/logs' / f'tests-core-{suite}.log'
        try:
            metrics = run([sys.executable, str(Path(__file__).resolve()), '--worker', suite],
                          cwd=ROOT, log=log, rss_limit=4 << 30)
            print(json.dumps({'suite': suite, **metrics}), flush=True)
        finally:
            if log.exists():
                print(log.read_text(encoding='utf-8', errors='replace'), end='', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
