"""Owned Windows preview integration with synthetic RGBA pixels."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from host import executable


@unittest.skipUnless(os.name == 'nt', 'Windows desktop integration')
class WindowTests(unittest.TestCase):
    def test_pixels_resize_copy_and_close(self):
        with tempfile.TemporaryDirectory(prefix='window-test-') as directory:
            binary = Path(directory) / executable('window-test')
            result = subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                                     '-fsanitize=address,undefined', '-I', str(ROOT / 'runtime'),
                                     str(ROOT / 'runtime/window_windows.c'), str(ROOT / 'tests/native_window.c'),
                                     '-luser32', '-lgdi32', '-o', str(binary)],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertNotIn('runtime error:', result.stderr)
            self.assertNotIn('ERROR: AddressSanitizer', result.stderr)


if __name__ == '__main__':
    unittest.main()
