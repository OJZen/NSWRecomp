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
    def check_table(self, width, metadata=True, writable=False, guarded=False, clobber=False, scheduled=12, alias=False, distant=False):
        with tempfile.TemporaryDirectory(prefix='state-table-') as temp:
            directory = Path(temp)
            table = 0x500 if writable else 0x300
            function_entry = 0x60 if distant else 0x80
            if distant:
                code = {0x60: f'adr x1, .+{table - 0x60}', 0x64: 'b .+112',
                        0x80: 'mov w0, #11', 0x84: 'ret', 0x88: 'mov w0, #22', 0x8c: 'ret',
                        0x90: 'mov w0, #33', 0x94: 'ret', 0xd4: 'cmp w0, #2', 0xd8: 'b.hi .+24',
                        0xdc: 'adr x2, .-92', 0xe0: 'ldrb w3, [x1, x0]', 0xe4: 'add x2, x2, x3, lsl #2',
                        0xec: 'br x2', 0xf0: 'mov w0, #99', 0xf4: 'ret'}
                instructions = [code.get(at, 'nop') for at in range(0x60, 0xf8, 4)]
                branch, entry, cases, fmt, values = 0xec, 0xd4, [0x80, 0x88, 0x90], '<4B', (0, 2, 4, 0xff)
            elif guarded:
                extra = (['add w0, w0, #6'] if clobber else []) + (['and x6, x0, #0xffffffff'] if alias else [])
                branch = 0xa0 + scheduled * 4
                cases = [branch + n for n in (4, 12, 20)]
                address = 0x90 + len(extra) * 4
                instructions = ['ldr x4, [x5]', 'ldr x4, [x4]', 'cmp w0, #2', f'b.hi .+{branch + 28 - 0x8c}', *extra,
                                f'adr x1, .+{table - address}', f'adr x2, .+{cases[0] - address - 4}',
                                f'ldrh w3, [x1, x{6 if alias else 0}, lsl #1]', 'add x2, x2, x3, lsl #2',
                                *(['nop'] * (scheduled - len(extra))), 'br x2',
                                'mov w0, #11', 'ret', 'mov w0, #22', 'ret', 'mov w0, #33', 'ret', 'mov w0, #99', 'ret']
                entry, fmt, values = 0x80, '<4H', (0, 2, 4, 0xffff)
            elif width == 4:
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
            struct.pack_into('<Q', raw, 0x320, function_entry)
            struct.pack_into('<Q', raw, 0x328, len(words) * 4 if metadata else 0)
            struct.pack_into('<' + 'I' * len(words), raw, 0x100 + function_entry, *words)
            for offset in (0x500, 0x508, 0x6f8, 0x700): struct.pack_into('<Q', raw, offset + 0x100, function_entry)
            struct.pack_into(fmt, raw, table + 0x100, *values)
            path = directory / 'synthetic.nso'; path.write_bytes(raw)
            buffer = (ctypes.c_ubyte * 8192)(); base = (ctypes.addressof(buffer) + 4095) & ~4095
            module = gen.Module('synthetic', path, base, 'c', game_patches=False)
            targets = module.jump_table(base + branch, base + entry, base + (0xf0 if distant else cases[-1]))
            if not metadata or writable or clobber:
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
''' + f'void f_{base + function_entry:x}(Cpu *c);\n'
                + f'uint64_t run(uint64_t value) {{ Cpu cpu = {{0}}; uint64_t cell = 0, pointer = (uintptr_t)&cell; cpu.x[5] = (uintptr_t)&pointer; cpu.x[0] = value; f_{base + function_entry:x}(&cpu); return cpu.x[0]; }}\n')
            library = directory / 'synthetic.so'
            subprocess.run(['clang', '-shared', '-fPIC', '-O1', '-I', str(ROOT / 'runtime'),
                            *map(str, output.glob('*.c')), str(wrapper), '-o', str(library)],
                           check=True, timeout=30, capture_output=True)
            ctypes.memmove(base, bytes(module.nso.image), len(module.nso.image))
            native = ctypes.CDLL(str(library)); native.run.argtypes = [ctypes.c_uint64]; native.run.restype = ctypes.c_uint64
            for value, expected in enumerate((11, 22, 33)): self.assertEqual(native.run(value), expected)
            if guarded or distant: self.assertEqual(native.run(3), 99)

    def test_table_base_before_partition_is_not_uninitialized_scratch(self):
        self.check_table(1, distant=True)

    def test_guarded_table_with_scheduled_addresses_and_unmapped_prelude(self):
        self.check_table(2, guarded=True, alias=True)
        self.check_table(2, guarded=True, scheduled=0)
        self.check_table(2, guarded=True, clobber=True, alias=True)

    def test_byte_halfword_and_signed_word_state_tables(self):
        for width in (1, 2, 4):
            with self.subTest(width=width): self.check_table(width)

    def test_requires_function_metadata_and_readonly_table(self):
        self.check_table(2, metadata=False)
        self.check_table(2, writable=True)


if __name__ == '__main__':
    unittest.main()
