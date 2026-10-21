from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ControlPanelTests(unittest.TestCase):
    def compile_run(self, sources, flags=()):
        with tempfile.TemporaryDirectory(prefix='panel-test-') as temp:
            binary = Path(temp) / 'check'
            command = ['clang', '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined',
                       '-Wall', '-Wextra', '-Werror']
            for directory in ('runtime', 'runtime/graphics', 'runtime/graphics/include', 'third_party/lasrecomp/runtime'):
                command += ['-I', str(ROOT / directory)]
            build = subprocess.run([*command, *flags, *(str(ROOT / p) for p in sources), '-o', str(binary)],
                                   capture_output=True, text=True, timeout=30)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(run.returncode, 0, run.stderr)

    def test_frame_statistics_and_gpu_interval_union(self):
        self.compile_run(['runtime/graphics/performance.c', 'tests/native_performance.c'])

    def test_extended_controller_mapping_focus_and_disconnect(self):
        self.compile_run(['runtime/graphics/input.c', 'tests/native_gamepad.m'],
                         ['-fobjc-arc', '-framework', 'Foundation', '-framework', 'GameController'])

    def test_mute_drains_audio_and_unmute_resumes_samples(self):
        self.compile_run(['tests/native_audio_output.c'], ['-framework', 'AudioToolbox'])
