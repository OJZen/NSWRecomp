from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from host import cpu_flags, executable


class InputTests(unittest.TestCase):
    def test_keyboard_latching_release_styles_and_memory_bounds(self):
        with tempfile.TemporaryDirectory(prefix='input-test-') as temp:
            binary = Path(temp) / executable('check')
            subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined',
                            '-Wall', '-Wextra', '-Werror', *cpu_flags(), '-pthread', '-I', str(ROOT / 'runtime'),
                            '-I', str(ROOT / 'third_party/lasrecomp/runtime'),
                            '-I', str(ROOT / 'runtime/graphics'), '-I', str(ROOT / 'runtime/graphics/include'),
                            str(ROOT / 'runtime/graphics/input.c'), str(ROOT / 'tests/native_input.c'),
                            '-o', str(binary)], check=True, timeout=30, capture_output=True)
            for args, code in (([], 0), (['bad-pointer'], 78), (['bad-single-id'], 78), (['bad-single-output'], 78)):
                result = subprocess.run([str(binary), *args], capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, code, result.stderr)
