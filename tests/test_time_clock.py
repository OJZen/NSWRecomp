"""Deterministic clock wire layout, epoch arithmetic and SDK shared-memory reads."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from host import executable


class TimeClockTests(unittest.TestCase):
    def test_shared_clock_identity_progression_and_overflow(self):
        with tempfile.TemporaryDirectory(prefix='clock-test-') as directory:
            binary = Path(directory) / executable('check')
            subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined',
                            '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'runtime'),
                            str(ROOT / 'runtime/time_clock.c'), str(ROOT / 'tests/native_time_clock.c'),
                            '-o', str(binary)], check=True, capture_output=True, timeout=30)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertNotIn('runtime error:', result.stderr)
            self.assertNotIn('ERROR: AddressSanitizer', result.stderr)


if __name__ == '__main__':
    unittest.main()
