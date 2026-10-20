import contextlib
import io
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'third_party/lasrecomp/tools'))
import gen
import recompile
from test_nso import fixture


class HleTests(unittest.TestCase):
    def test_compiled_calls_and_dispatch_require_an_implemented_backend(self):
        with tempfile.TemporaryDirectory(prefix='hle-test-') as temp:
            directory = Path(temp)
            raw = fixture()
            struct.pack_into('<ii', raw, 0x150, -0x40, -0x40)
            name = b'\0nvnBootstrapLoader\0'
            struct.pack_into('<Q', raw, 0x238, len(name))
            raw[0x330:0x330 + len(name)] = name
            struct.pack_into('<4I', raw, 0x180, 0xd2800000, 0xd65f03c0, 0x97fffffe, 0xd65f03c0)
            nso = directory / 'input.nso'; nso.write_bytes(raw)
            bindings = directory / 'hle.txt'; bindings.write_text('\n'.join(recompile.hle_symbols()))
            generated = directory / 'generated'
            argv = ['gen', 'rtld', str(nso), '0x100000', str(generated), '--hle', str(bindings),
                    '--no-game-patches', '--simd', 'c']
            with patch.object(sys, 'argv', argv), contextlib.redirect_stderr(io.StringIO()):
                gen.main()
            main = directory / 'main.c'
            main.write_text('''#include "cpu.h"
#include <assert.h>
void f_100088(Cpu *);
int main(void) {
    Cpu c = {0}; c.x[0] = 11; f_100088(&c); assert(c.x[0] == 42);
    c.x[0] = 11; c.pc = 0x100080; rt_dispatch(&c); assert(c.x[0] == 42);
    return 0;
}
''')
            native = directory / 'native.c'
            native.write_text('#include "cpu.h"\nvoid hle_nvnBootstrapLoader(Cpu *c) { c->x[0] += 31; }\n')
            command = ['clang', '-std=gnu11', '-O1', '-I', str(ROOT / 'third_party/lasrecomp/runtime'),
                       *map(str, generated.glob('*.c')), str(ROOT / 'runtime/probe.c'),
                       str(ROOT / 'runtime/hle.c'), str(main)]
            binary = directory / 'check'
            subprocess.run([*command, '-o', str(binary)], check=True, capture_output=True, timeout=30)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 78, result.stderr)
            self.assertIn('missing host API: nvnBootstrapLoader', result.stderr)
            subprocess.run([*command, str(native), '-o', str(binary)], check=True, capture_output=True, timeout=30)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_binding_manifest_rejects_duplicate_or_invalid_symbols(self):
        with tempfile.TemporaryDirectory() as temp:
            manifest = Path(temp) / 'hle.inc'
            with patch.object(recompile, 'HLE_SYMBOLS', manifest):
                for text in ('HLE(example)\nHLE(example)\n', 'HLE(bad-name)\n', 'unrecognized\n'):
                    manifest.write_text(text)
                    with self.assertRaisesRegex(ValueError, 'invalid or duplicate'):
                        recompile.hle_symbols()


if __name__ == '__main__':
    unittest.main()
