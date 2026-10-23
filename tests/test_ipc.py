from pathlib import Path
import os
import subprocess
import sys
import struct
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from host import cpu_flags, executable
from load import ADDRESS_UNAVAILABLE
from startup import platform_flags
from test_romfs import fixture


class IpcTests(unittest.TestCase):
    def test_headless_audio_ipc(self):
        self._check_services(headless_audio=True)

    def test_basic_services_protocol_policy_lifetimes_and_malformed_packets(self):
        self._check_services(headless_audio=False)

    def _check_services(self, headless_audio):
        with tempfile.TemporaryDirectory(prefix='ipc-test-') as directory:
            root = Path(directory)
            stub = root / 'guest.c'
            stub.write_text('#include "cpu.h"\n#include <stdlib.h>\n'
                            'GuestFn rt_find_function(uint64_t a) { (void)a; return NULL; }\n'
                            'void rt_dispatch(Cpu *c) { (void)c; abort(); }\n'
                            'void rt_udf(Cpu *c, uint64_t pc) { (void)c; (void)pc; abort(); }\n')
            binary = root / executable('ipc')
            romfs = root / 'romfs.bin'; fixture(romfs)
            font = root / 'synthetic.ttf'
            font.write_bytes(struct.pack('>IHHHH', 0x10000, 2, 0, 0, 0)
                             + struct.pack('>4sIII', b'glyf', 0, 44, 8)
                             + struct.pack('>4sIII', b'loca', 0, 52, 4) + bytes(12))
            environment = dict(os.environ, UBSAN_OPTIONS='halt_on_error=1', ZELDA_IPC_LOG='1', NSW_TEST_ROMFS=str(romfs), ZELDA_SHARED_FONT=str(font))
            environment.pop('ZELDA_ROMFS', None); environment.pop('NSW_ROMFS_HANDLE', None)
            subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined',
                            '-fno-omit-frame-pointer', '-Wall', '-Wextra', '-Werror', *cpu_flags(),
                            '-DNSW_BASIC_SERVICES', '-I', str(ROOT / 'runtime'),
                            *(['-DNSW_HEADLESS_AUDIO', str(ROOT / 'runtime/audio_headless.c'), str(ROOT / 'runtime/audio_out.c')] if headless_audio else []),
                            '-I', str(ROOT / 'third_party/lasrecomp/runtime'), str(stub),
                            *[str(ROOT / 'runtime' / name) for name in ('host.c', 'kernel.c', 'threads.c', 'services.c', 'time_clock.c', 'romfs.c')],
                            str(ROOT / 'tests/native_ipc.c'), *platform_flags(basic_services=True),
                            '-o', str(binary)], check=True, timeout=30)
            for _ in range(8):
                result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20,
                                        env=environment)
                if result.returncode != ADDRESS_UNAVAILABLE:
                    break
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('Basic IPC checks passed', result.stdout)
