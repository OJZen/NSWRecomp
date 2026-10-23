"""Compiled callback and allocator ABI checks without a graphics device or game data."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from host import cpu_flags, executable


class GraphicsAllocatorTests(unittest.TestCase):
    def test_guest_allocator_reentry_state_and_invalid_targets(self):
        with tempfile.TemporaryDirectory(prefix='graphics allocator ') as td:
            binary = Path(td) / executable('check')
            subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined',
                            '-Wall', '-Wextra', '-Werror', *cpu_flags(), '-pthread',
                            '-I', str(ROOT / 'runtime'), '-I', str(ROOT / 'third_party/lasrecomp/runtime'),
                            str(ROOT / 'runtime/guest_callback.c'), str(ROOT / 'runtime/graphics_allocator.c'),
                            str(ROOT / 'tests/native_graphics_allocator.c'), '-o', str(binary)],
                           check=True, capture_output=True, timeout=30)
            for arguments, expected in (([], 0), (['invalid-registration'], 78)):
                result = subprocess.run([str(binary), *arguments], capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
                self.assertNotIn('runtime error:', result.stderr)
                self.assertNotIn('ERROR: AddressSanitizer', result.stderr)


if __name__ == '__main__':
    unittest.main()
