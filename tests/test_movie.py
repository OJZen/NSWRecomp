from pathlib import Path
import os
import shlex
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import graphics
from host import cpu_flags, executable
from test_load import BASE, fixture


class MovieTests(unittest.TestCase):
    def test_callbacks_use_game_override_and_symbol_visibility(self):
        with tempfile.TemporaryDirectory(prefix='movie-symbol-test-') as temp:
            directory = Path(temp)
            (directory / 'guest_symbols.inc').write_text('GUEST(external)\n')
            infos = {}
            for index, name in enumerate(('main', 'subsdk0', 'sdk')):
                path = directory / name
                module = fixture(path, main=name == 'main')
                raw = bytearray(path.read_bytes())
                ro_file = struct.unpack_from('<I', raw, 0x20)[0]
                symbol_at = ro_file + module.nso.dyn[6] - module.nso.rodata[0] + 24
                imported = name == 'subsdk0'
                struct.pack_into('<BBHQQ', raw, symbol_at + 4, 0x22 if name == 'sdk' else 0x12,
                                 0, 0 if imported else 1, 0 if imported else 0x80, 0 if imported else 4)
                path.write_bytes(raw)
                infos[name] = {'base': hex(BASE + index * 0x10000)}
                if name == 'main': main_visibility = symbol_at + 5
            for hidden in (False, True):
                with self.subTest(hidden=hidden):
                    raw = bytearray((directory / 'main').read_bytes())
                    raw[main_visibility] = 2 if hidden else 0
                    (directory / 'main').write_bytes(raw)
                    with patch.object(graphics, 'OUTPUT', directory / 'output'), \
                            patch.object(graphics, 'GFX', directory), \
                            patch.object(graphics, 'source_hashes', return_value={}), \
                            patch.object(graphics, 'verify', return_value=infos), \
                            patch.object(graphics, 'signature', side_effect=RuntimeError('symbols generated')):
                        with self.assertRaisesRegex(RuntimeError, 'symbols generated'):
                            graphics.build(directory)
                    expected = BASE + (0x20000 if hidden else 0) + 0x80
                    source = (directory / 'output/guest_symbols.c').read_text()
                    self.assertIn(f'return {expected:#x}ull;', source)

    def test_decoding_and_compiled_cpu_protocol(self):
        if os.name == 'nt':
            from prepare_vulkan_video import verified_dependency
            _, flags = verified_dependency()
        else:
            flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'vpx', 'vorbisenc'], text=True, timeout=10))
        with tempfile.TemporaryDirectory(prefix='movie-test-') as temp:
            for name in ('native_movie_video', 'native_movie'):
                with self.subTest(name=name):
                    binary = Path(temp) / executable(name)
                    compile_result = subprocess.run(['clang', '-std=gnu11', '-O2', '-g', '-fsanitize=address,undefined',
                                    '-Wall', '-Wextra', '-Werror', '-pthread', *cpu_flags(),
                                    *(['-DNSW_VULKAN_VIDEO'] if name == 'native_movie' else []),
                                    '-I', str(ROOT / 'runtime'), '-I', str(ROOT / 'runtime/graphics/include'),
                                    '-I', str(ROOT / 'third_party/lasrecomp/runtime'),
                                    str(ROOT / 'tests' / (name + '.c')),
                                    str(ROOT / 'runtime/graphics/movie_video.c'),
                                    str(ROOT / 'runtime/graphics/movie_audio.c'), *flags,
                                    '-o', str(binary)], capture_output=True, text=True, timeout=30)
                    self.assertEqual(compile_result.returncode, 0, compile_result.stderr)
                    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    if name == 'native_movie':
                        for mode in ('texture', 'audio'):
                            result = subprocess.run([str(binary), mode], capture_output=True, text=True, timeout=20)
                            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
