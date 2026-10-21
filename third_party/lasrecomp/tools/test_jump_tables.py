"""Synthetic jump tables whose case labels cross discovered function boundaries."""
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


class JumpTableTests(unittest.TestCase):
    def check_table(self, taken, split_guard=False, prior_load=False, conditional=False, unsafe=False, dual_guard=False, restored=False, clobbered=False, index_register=0, normalized=False):
        with tempfile.TemporaryDirectory(prefix='jump-table-') as temp:
            directory = Path(temp)
            if restored:
                instructions = ['mov w6, w0', 'cmp w0, #2', 'b.hi .+64', 'ldr x4, [x5]', 'ldr x4, [x4]',
                                'add w6, w6, #7' if clobbered else 'nop', 'mov w0, w6', 'adr x1, .+612',
                                'adr x2, .+16', 'ldrh w3, [x1, x0, lsl #1]', 'add x2, x2, x3, lsl #2', 'br x2',
                                'mov w0, #11', 'ret', 'mov w0, #22', 'ret', 'mov w0, #33', 'ret', 'mov w0, #99', 'ret']
                branch, cases = 0xac, [0xb0, 0xb8, 0xc0]
            elif taken:
                instructions = ['cmp w0, #2', 'b.ls .+12', 'mov w0, #99', 'ret',
                                'adr x1, .+624', 'adr x2, .+16', 'ldrh w3, [x1, x0, lsl #1]',
                                'add x2, x2, x3, lsl #2', 'br x2',
                                'mov w0, #11', 'ret', 'mov w0, #22', 'ret', 'mov w0, #33', 'ret']
                branch, cases = 0xa0, [0xa4, 0xac, 0xb4]
            else:
                instructions = ['cmp w0, #2', 'b.hi .+48', 'adr x1, .+632', 'adr x2, .+16',
                                'ldrh w3, [x1, x0, lsl #1]', 'add x2, x2, x3, lsl #2', 'br x2',
                                'mov w0, #11', 'ret', 'mov w0, #22', 'ret', 'mov w0, #33', 'ret',
                                'mov w0, #99', 'ret']
                branch, cases = 0x98, [0x9c, 0xa4, 0xac]
            if normalized:
                instructions[6] = 'nop'
                instructions[:0] = ['and w0, w0, #7', 'sub w0, w0, #1']
                instructions[9] = 'adr x1, .+604'
                branch += 8; cases = [value + 8 for value in cases]
            if index_register:
                instructions[0] = f'cmp w{index_register}, #2'
                instructions[4] = f'ldrh w3, [x1, x{index_register}, lsl #1]'
                if index_register == 30: instructions = ['ret x7' if i == 'ret' else i for i in instructions]
            if dual_guard:
                instructions.insert(1, 'b.gt .+52')
                instructions[3] = 'adr x1, .+628'
                branch += 4; cases = [value + 4 for value in cases]
            if conditional: instructions[0] = f'ccmp w0, #2, #{0 if unsafe else 2}, ne'
            if split_guard:
                instructions[3] = 'adr x2, .'
                instructions[4] = 'ldrsw x3, [x1, x0, lsl #2]'
                instructions[5] = 'add x2, x2, x3'
            words = assemble(directory, (['ldrb w8, [x20, x21]'] if prior_load else []) + instructions)
            raw = fixture()
            struct.pack_into('<ii', raw, 0x150, -0x40, -0x40)
            if conditional or restored: struct.pack_into('<Q', raw, 0x328, len(words) * 4)
            if prior_load: struct.pack_into('<I', raw, 0x17c, words.pop(0))
            struct.pack_into('<' + 'I' * len(words), raw, 0x180, *words)
            for offset in (0x500, 0x508, 0x6f8, 0x700):
                struct.pack_into('<Q', raw, offset + 0x100, cases[0] if split_guard and offset == 0x500 else 0x80)
            if split_guard: struct.pack_into('<3i', raw, 0x400, 16, 24, 32)
            else: struct.pack_into('<3H', raw, 0x400, 0, 2, 4)
            path = directory / 'synthetic.nso'
            path.write_bytes(raw)
            buffer = (ctypes.c_ubyte * 8192)()
            base = (ctypes.addressof(buffer) + 4095) & ~4095
            module = gen.Module('synthetic', path, base, 'c', game_patches=False)
            targets = [base + offset for offset in cases]
            if unsafe or clobbered:
                self.assertIsNone(module.jump_table(base + branch, base + 0x80, targets[0]))
                return
            labels, external, tables = module.scan(base + (0x8c if split_guard else 0x80), targets[0])
            self.assertEqual(tables[base + branch], targets)
            self.assertTrue(set(targets) <= external)
            self.assertFalse(set(targets) & labels)
            output = directory / 'generated'
            args = ['gen.py', 'synthetic', str(path), hex(base), str(output), '--simd', 'c', '--no-game-patches']
            with patch.object(sys, 'argv', args), contextlib.redirect_stderr(io.StringIO()):
                gen.main()
            table = (output / 'synthetic_table.c').read_text()
            for target in targets:
                self.assertIn(f'void f_{target:x}(Cpu *c);', table)
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
                + f'uint64_t run(uint64_t value, uint64_t flags) {{ Cpu cpu = {{0}}; uint64_t cell = 0, pointer = (uintptr_t)&cell; cpu.x[5] = (uintptr_t)&pointer; cpu.x[0] = value; cpu.x[{index_register}] = value; cpu.nzcv = flags; f_{base + 0x80:x}(&cpu); return cpu.x[0]; }}\n')
            library = directory / 'synthetic.so'
            subprocess.run(['clang', '-shared', '-fPIC', '-O1', '-I', str(ROOT / 'runtime'),
                            *map(str, output.glob('*.c')), str(wrapper), '-o', str(library)],
                           check=True, timeout=30, capture_output=True)
            ctypes.memmove(base, bytes(module.nso.image), len(module.nso.image))
            native = ctypes.CDLL(str(library))
            native.run.argtypes = [ctypes.c_uint64, ctypes.c_uint64]
            native.run.restype = ctypes.c_uint64
            values = ((0, 11), (1, 22), (2, 33), (3, 99), (0xffffffff, 99))
            if normalized: values = ((0, 99), (1, 11), (2, 22), (3, 33), (4, 99), (9, 11), (0xffffffff, 99))
            for value, expected in values: self.assertEqual(native.run(value, 0), expected)
            if conditional:
                for value in (0, 3, 0xffffffff): self.assertEqual(native.run(value, 4 << 28), 99)
            # Targets outside executable text are not accepted merely because they are aligned.
            broken = bytearray(raw)
            if split_guard: struct.pack_into('<i', broken, 0x408, 0x1000)
            else: struct.pack_into('<H', broken, 0x404, 0xffff)
            path.write_bytes(broken)
            invalid = gen.Module('synthetic', path, base, 'c', game_patches=False)
            self.assertIsNone(invalid.jump_table(base + branch, base + 0x80, targets[0]))

    def test_new_bound_after_mask_normalization(self):
        self.check_table(False, restored=True, normalized=True)

    def test_frame_and_link_register_indices(self):
        for register in (29, 30): self.check_table(False, index_register=register)

    def test_index_restored_from_a_saved_alias_after_comparison(self):
        self.check_table(False, restored=True)
        self.check_table(False, restored=True, clobbered=True)

    def test_unsigned_guard_after_signed_rejection(self):
        self.check_table(False, dual_guard=True)

    def test_conditional_compare_must_skip_the_table_on_fallback(self):
        for taken in (False, True):
            self.check_table(taken, conditional=True)
            self.check_table(taken, conditional=True, unsafe=True)

    def test_existing_guard_ignores_unavailable_previous_partition_memory(self):
        self.check_table(False, prior_load=True)

    def test_signed_table_with_guard_in_previous_partition(self):
        self.check_table(False, split_guard=True)

    def test_cross_partition_fallthrough_table(self):
        self.check_table(False)

    def test_cross_partition_taken_table(self):
        self.check_table(True)


if __name__ == '__main__':
    unittest.main()
