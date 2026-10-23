"""Portable NVN resource, command snapshot and Vulkan handoff contracts."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from host import cpu_flags, executable


class VulkanNvnTests(unittest.TestCase):
    def test_recording_submission_bounds_and_callbacks(self):
        with tempfile.TemporaryDirectory(prefix='vulkan-nvn-') as td:
            binary = Path(td) / executable('check')
            includes = ('runtime', 'runtime/graphics/include', 'runtime/graphics/nvn', 'third_party/vulkan-headers/include',
                        'third_party/lasrecomp/runtime', 'third_party/zelda-mobile/core/gfx/nvn',
                        'third_party/zelda-mobile/core/gfx/nvn/include')
            result = subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined',
                                    '-Wall', '-Wextra', '-Werror', '-pthread', *cpu_flags(),
                                    *[arg for p in includes for arg in ('-I', str(ROOT / p))],
                                    str(ROOT / 'tests/native_vulkan_nvn.c'), str(ROOT / 'runtime/graphics/nvn/nvn_tex.c'),
                                    str(ROOT / 'runtime/graphics/tex/nvn_deswizzle.c'),
                                    '-lm', '-o', str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for mode in ('linear', 'srgb', 'rgb11f', 'texture-clear', 'depth', 'copy', 'callback', 'draw-boundary', 'pool-overflow', 'partial', 'truncated', 'readback-mismatch', 'shader-extension', 'texture-bounds'):
                with self.subTest(mode=mode):
                    result = subprocess.run([str(binary), mode], capture_output=True, text=True, timeout=5)
                    self.assertEqual(result.returncode, 0 if mode in ('linear', 'srgb', 'rgb11f', 'texture-clear', 'depth', 'copy', 'callback') else 78, result.stderr)
                    self.assertNotIn('runtime error:', result.stderr)
                    self.assertNotIn('ERROR: AddressSanitizer', result.stderr)

    def test_typed_registration_list(self):
        names = []
        for name in ('vulkan_nvn.inc', 'vulkan_texture.inc', 'vulkan_commands.inc', 'vulkan_state.inc', 'vulkan_descriptor.inc', 'vulkan_program.inc', 'vulkan_texture_io.inc'):
            names.extend(re.findall(r'^NVN\((\w+),', (ROOT / 'runtime' / name).read_text(), re.M))
        entries = re.findall(r'^NVN_ENTRY\((\w+)\)', (ROOT / 'runtime/vulkan_nvn_entries.inc').read_text(), re.M)
        self.assertEqual(entries, names)
        self.assertEqual(len(entries), len(set(entries)))


if __name__ == '__main__':
    unittest.main()
