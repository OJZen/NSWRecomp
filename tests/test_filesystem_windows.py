import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(os.name == 'nt', 'Windows filesystem backend')
class WindowsFilesystemTests(unittest.TestCase):
    def test_scratch_file_operations_and_path_confinement(self):
        with tempfile.TemporaryDirectory(prefix='scratch-test-') as directory:
            root = Path(directory); scratch = root / 'isolated'; scratch.mkdir()
            executable = root / 'filesystem.exe'
            subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined',
                            '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'runtime'),
                            str(ROOT / 'runtime/filesystem_windows.c'), str(ROOT / 'tests/native_filesystem_windows.c'),
                            '-o', str(executable)], check=True, timeout=30)
            result = subprocess.run([str(executable)], env=dict(os.environ, NSW_SCRATCH_FS=str(scratch), UBSAN_OPTIONS='halt_on_error=1'),
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('Windows scratch filesystem checks passed', result.stdout)
            self.assertEqual(sorted(p.name for p in scratch.iterdir()), ['volume-0', 'volume-1'])
