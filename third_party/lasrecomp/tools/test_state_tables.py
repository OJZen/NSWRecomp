"""Unguarded enum-state tables, built entirely from synthetic instructions and metadata."""
import contextlib
import ctypes
import io
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import gen
import nso
from test_fpsr import assemble
from test_nso import fixture

ROOT = Path(nso.__file__).resolve().parents[1]


class StateTableTests(unittest.TestCase):
    def check_table(self, width, metadata=True, writable=False):
        with tempfile.TemporaryDirectory(prefix='state-table-') as temp:
            directory = Path(temp)
            table = 0x500 if writable else 0x300
            if width == 4:
                instructions = ['b .+20', 'mov w0, #11', 'ret', 'mov w0, #22', 'ret',
                                f'adr x1, .+{table - 0x94}', 'adr x2, .', 'ldrsw x3, [x1, x0, lsl #2]',
                                'add x2, x2, x3', 'br x2', 'mov w0, #33', 'ret']
                branch, entry, cases, fmt, values = 0xa4, 0x98, [0x84, 0x8c, 0xa8], '<4i', (-20, -12, 16, 0x1000)
            else:
                load = 'ldrb w3, [x1, x0]' if width == 1 else 'ldrh w3, [x1, x0, lsl #1]'
                instructions = [f'adr x1, .+{table - 0x80}', 'adr x2, .+16', load,
                                'add x2, x2, x3, lsl #2', 'br x2', 'mov w0, #11', 'ret',
                                'mov w0, #22', 'ret', 'mov w0, #33', 'ret']
                branch, entry, cases = 0x90, 0x80, [0x94, 0x9c, 0xa4]
                fmt, values = ('<4B', (0, 2, 4, 0xff)) if width == 1 else ('<4H', (0, 2, 4, 0xffff))
            words = assemble(directory, instructions)
            raw = fixture()
            struct.pack_into('<ii', raw, 0x150, -0x40, -0x40)
            struct.pack_into('<Q', raw, 0x328, len(words) * 4 if metadata else 0)
            struct.pack_into('<' + 'I' * len(words), raw, 0x180, *words)
            for offset in (0x500, 0x508, 0x6f8, 0x700): struct.pack_into('<Q', raw, offset + 0x100, 0x80)
            struct.pack_into(fmt, raw, table + 0x100, *values)
            path = directory / 'synthetic.nso'; path.write_bytes(raw)
            buffer = (ctypes.c_ubyte * 8192)(); base = (ctypes.addressof(buffer) + 4095) & ~4095
            module = gen.Module('synthetic', path, base, 'c', game_patches=False)
            targets = module.jump_table(base + branch, base + entry, base + cases[-1])
            if not metadata or writable:
                self.assertIsNone(targets); return
            self.assertEqual(targets, [base + offset for offset in cases])
            output = directory / 'generated'
            args = ['gen', 'synthetic', str(path), hex(base), str(output), '--simd', 'c', '--no-game-patches']
            with patch.object(sys, 'argv', args), contextlib.redirect_stderr(io.StringIO()): gen.main()
            wrapper = directory / 'wrapper.c'
            wrapper.write_text('''#include "cpu.h"
#include <stdlib.h>
volatile int g_fnprof;
void fnprof_hit(uint64_t a, uint64_t b, uint64_t d) { (void)a; (void)b; (void)d; }
void rt_dispatch(Cpu *c) { (void)c; abort(); }
void rt_svc(Cpu *c, uint32_t i, uint64_t p) { (void)c; (void)i; (void)p; abort(); }
void rt_brk(Cpu *c, uint32_t i, uint64_t p) { (void)c; (void)i; (void)p; abort(); }
void rt_udf(Cpu *c, uint64_t p) { (void)c; (void)p; abort(); }
uint64_t rt_ticks(void) { abort(); }
''' + f'void f_{base + 0x80:x}(Cpu *c);\n'
                + f'uint64_t run(uint64_t value) {{ Cpu cpu = {{0}}; cpu.x[0] = value; f_{base + 0x80:x}(&cpu); return cpu.x[0]; }}\n')
            library = directory / 'synthetic.so'
            subprocess.run(['clang', '-shared', '-fPIC', '-O1', '-I', str(ROOT / 'runtime'),
                            *map(str, output.glob('*.c')), str(wrapper), '-o', str(library)],
                           check=True, timeout=30, capture_output=True)
            ctypes.memmove(base, bytes(module.nso.image), len(module.nso.image))
            native = ctypes.CDLL(str(library)); native.run.argtypes = [ctypes.c_uint64]; native.run.restype = ctypes.c_uint64
            for value, expected in enumerate((11, 22, 33)): self.assertEqual(native.run(value), expected)

    def test_byte_halfword_and_signed_word_state_tables(self):
        for width in (1, 2, 4):
            with self.subTest(width=width): self.check_table(width)

    def test_requires_function_metadata_and_readonly_table(self):
        self.check_table(2, metadata=False)
        self.check_table(2, writable=True)


if __name__ == '__main__':
    unittest.main()
