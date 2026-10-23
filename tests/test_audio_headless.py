"""Silent renderer timing and voice ownership; no speaker or game input."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from host import executable


class HeadlessAudioTests(unittest.TestCase):
    def test_revision_voice_consumption_events_and_pause(self):
        with tempfile.TemporaryDirectory(prefix='headless-audio-') as td:
            binary = Path(td) / executable('check')
            result = subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined',
                                     '-Wall', '-Wextra', '-Werror', '-pthread', '-I', str(ROOT / 'runtime'),
                                     '-I', str(ROOT / 'third_party/lasrecomp/runtime'),
                                     str(ROOT / 'runtime/audio_headless.c'), str(ROOT / 'tests/native_audio_headless.c'),
                                     '-lm', '-o', str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            for revision in (5, 9, 15):
                with self.subTest(revision=revision):
                    result = subprocess.run([str(binary), str(revision)], capture_output=True, text=True, timeout=5)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertNotIn('runtime error:', result.stderr)
                    self.assertNotIn('ERROR: AddressSanitizer', result.stderr)


if __name__ == '__main__':
    unittest.main()
