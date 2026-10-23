from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import load
import startup


class ServiceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='services-test-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.root = Path(cls.temp.name)
        empty_guest = cls.root / 'empty_guest.c'
        empty_guest.write_text('#include "cpu.h"\n#include <stdlib.h>\n'
                               'GuestFn rt_find_function(uint64_t a) { (void)a; return NULL; }\n'
                               'GuestFn rt_find_external(uint64_t a) { (void)a; return NULL; }\n'
                               'void rt_dispatch(Cpu *c) { (void)c; abort(); }\n'
                               'void rt_udf(Cpu *c, uint64_t pc) { (void)c; (void)pc; abort(); }\n')
        cls.services = cls.root / 'services'
        subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer', '-Wall', '-Wextra', '-Werror',
                        '-I', str(ROOT / 'third_party/lasrecomp/runtime'), str(empty_guest),
                        *[str(ROOT / 'runtime' / name) for name in startup.SOURCES
                          if name.endswith('.c') and name not in ('startup.c', 'probe.c', 'load.c')],
                        str(ROOT / 'tests/native_services.c'), *startup.platform_flags(),
                        '-o', str(cls.services)], check=True, timeout=30)

    def test_native_ipc_services_and_malformed_packets(self):
        (self.root / 'work/tmp').mkdir(parents=True, exist_ok=True)
        for _ in range(8):
            result = subprocess.run([str(self.services)], cwd=self.root, capture_output=True, text=True, timeout=25,
                                    env=dict(os.environ, DM_AUDIO='0', UBSAN_OPTIONS='halt_on_error=1',
                                             ASAN_OPTIONS='detect_stack_use_after_return=0'))
            if result.returncode != load.ADDRESS_UNAVAILABLE:
                break
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
