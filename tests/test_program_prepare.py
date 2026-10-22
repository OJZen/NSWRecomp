from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ProgramPreparationTests(unittest.TestCase):
    def test_command_prefix_batches_only_needed_shaders_without_mutating_cursor(self):
        with tempfile.TemporaryDirectory(prefix='program-prepare-') as temp:
            binary = Path(temp) / 'check'
            compiled = subprocess.run(['clang', '-std=gnu11', '-O2', '-fobjc-arc', '-Wno-deprecated-declarations',
                            '-I', str(ROOT / 'runtime/graphics/include'), '-framework', 'Foundation',
                            str(ROOT / 'tests/native_program_prepare.m'), '-o', str(binary)],
                           capture_output=True, text=True, timeout=30)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            for name, expected in (('normal', 0), ('invalid', 78)):
                case = Path(temp) / name; case.mkdir()
                command = [str(binary), str(case)] + (['invalid'] if expected else [])
                result = subprocess.run(command, capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, expected, result.stderr)
