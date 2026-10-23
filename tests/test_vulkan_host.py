"""Vulkan contracts with a synthetic driver; no installed SDK, GPU or game input."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from host import cpu_flags, executable
from vulkan_probe import HEADERS, header_hashes


class VulkanHostTests(unittest.TestCase):
    def test_compute_readback_failure_atomicity_and_ownership(self):
        with tempfile.TemporaryDirectory(prefix='vulkan-compute-') as td:
            binary = Path(td) / executable('check')
            result = subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined',
                                     '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                                     '-I', str(ROOT / 'runtime'), '-I', str(HEADERS / 'include'),
                                     str(ROOT / 'runtime/vulkan_compute.c'), str(ROOT / 'runtime/vulkan_host.c'),
                                     str(ROOT / 'tests/native_vulkan_compute.c'), '-o', str(binary)],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            for case in ('coherent', 'noncoherent', 'missing-compute-entry', 'pipeline-failure', 'allocation-failure',
                         'flush-failure', 'map-failure', 'invalidate-failure', 'second-readback-failure', 'timeout', 'submit-failure'):
                with self.subTest(case=case):
                    result = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=5)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertNotIn('runtime error:', result.stderr)
                    self.assertNotIn('ERROR: AddressSanitizer', result.stderr)

    @unittest.skipUnless(os.name == 'nt', 'Windows offscreen VI clock')
    def test_display_lifecycle_and_vsync_callback(self):
        with tempfile.TemporaryDirectory(prefix='vulkan-display-') as td:
            binary = Path(td) / executable('check')
            result = subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined',
                                     '-Wall', '-Wextra', '-Werror', '-pthread', *cpu_flags(),
                                     '-I', str(ROOT / 'runtime'), '-I', str(ROOT / 'runtime/graphics/include'),
                                     '-I', str(ROOT / 'third_party/lasrecomp/runtime'), '-I', str(HEADERS / 'include'),
                                     str(ROOT / 'tests/native_vulkan_display.c'), '-o', str(binary)],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_render_pass_lifetimes_and_failures(self):
        with tempfile.TemporaryDirectory(prefix='vulkan-render-') as td:
            binary = Path(td) / executable('check')
            result = subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined',
                                     '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                                     '-I', str(ROOT / 'runtime'), '-I', str(HEADERS / 'include'),
                                     str(ROOT / 'runtime/vulkan_render.c'), str(ROOT / 'runtime/vulkan_host.c'),
                                     str(ROOT / 'tests/native_vulkan_render.c'), '-o', str(binary)],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            for case in ('coherent', 'noncoherent', 'depth', 'missing-render-entry', 'pass-failure', 'view-failure',
                         'framebuffer-failure', 'allocation-failure', 'timeout', 'submit-failure', 'map-failure', 'invalidate-failure', 'render-mismatch'):
                with self.subTest(case=case):
                    result = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=5)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertNotIn('runtime error:', result.stderr)
                    self.assertNotIn('ERROR: AddressSanitizer', result.stderr)

    def test_runtime_initialization_cleanup_and_failure(self):
        with tempfile.TemporaryDirectory(prefix='vulkan runtime ') as td:
            binary = Path(td) / executable('check')
            subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined',
                            '-Wall', '-Wextra', '-Werror', '-pthread', *cpu_flags(),
                            '-I', str(ROOT / 'runtime'), '-I', str(ROOT / 'third_party/lasrecomp/runtime'),
                            '-I', str(HEADERS / 'include'), str(ROOT / 'runtime/vulkan_runtime.c'),
                            str(ROOT / 'tests/native_vulkan_runtime.c'), '-o', str(binary)],
                           check=True, capture_output=True, timeout=30)
            env = dict(os.environ)
            env.pop('NSW_VULKAN_ADAPTER', None)
            for case in ('initialize', 'finalize-reinitialize', 'render-lifetime', 'invalid-memory', 'loader-failure', 'device-failure', 'double-init'):
                with self.subTest(case=case):
                    result = subprocess.run([str(binary), case], env=env, capture_output=True, text=True, timeout=5)
                    self.assertEqual(result.returncode, 0 if case in ('initialize', 'finalize-reinitialize', 'render-lifetime') else 78,
                                     result.stdout + result.stderr)
                    self.assertNotIn('runtime error:', result.stderr)
                    self.assertNotIn('ERROR: AddressSanitizer', result.stderr)

    def test_pinned_header_provenance(self):
        hashes = header_hashes()
        self.assertEqual(len(hashes['files']), 17)
        self.assertEqual(json.loads((HEADERS / 'SOURCE.json').read_text())['revision'],
                         '6802bb4733b63ed5efd3adb308a6c885ef180ea1')

    def test_device_transfer_failure_and_pending_ownership(self):
        with tempfile.TemporaryDirectory(prefix='vulkan synthetic ') as td:
            binary = Path(td) / executable('check')
            subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined',
                            '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                            '-I', str(ROOT / 'runtime'), '-I', str(HEADERS / 'include'),
                            str(ROOT / 'runtime/vulkan_host.c'), str(ROOT / 'tests/native_vulkan_host.c'),
                            '-o', str(binary)], check=True, capture_output=True, timeout=30)
            for case in ('coherent', 'noncoherent', 'ineligible-adapter', 'old-loader', 'device-failure',
                         'missing-entry', 'no-memory', 'allocation-failure', 'timeout', 'submit-failure',
                         'map-failure', 'invalidate-failure', 'mismatch'):
                with self.subTest(case=case):
                    result = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=5)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertNotIn('runtime error:', result.stderr)
                    self.assertNotIn('ERROR: AddressSanitizer', result.stderr)


if __name__ == '__main__':
    unittest.main()
