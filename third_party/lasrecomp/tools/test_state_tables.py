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
    def check_table(self, width, metadata=True, writable=False, guarded=False, clobber=False,
                    scheduled=12, alias=False, distant=False, unrelated=False, masked=False,
                    alternate=False, far_mask=False, unmapped=False, compare_mask=False,
                    independent_guard=False, previous_guard=False, long_guard=False, unguarded=False,
                    subtract_mask=False, dead_base=False, joined_base=False, indirect_entry=False,
                    vector_write=False, branch_clobber=False, selected_base=False, call_base=None):
        with tempfile.TemporaryDirectory(prefix='state-table-') as temp:
            directory = Path(temp)
            table = 0x500 if writable else 0x300
            function_entry = 0x20 if long_guard else 0x60 if distant else 0x80
            if long_guard:
                instructions = ['cmp w0, #2', *(['nop'] * 23), 'b.hi .+88',
                                'ldr x4, [x5]', 'ldr x4, [x4]', f'adr x1, .+{table - 0x8c}', 'adr x2, .+48',
                                'ldrb w3, [x1, x0]', 'add x2, x2, x3, lsl #2', *(['nop'] * 8), 'br x2',
                                'mov w0, #11', 'ret', 'mov w0, #22', 'ret', 'mov w0, #33', 'ret', 'mov w0, #99', 'ret']
                branch, entry, cases, fmt, values = 0xbc, 0x20, [0xc0, 0xc8, 0xd0], '<4B', (0, 2, 4, 0xff)
            elif distant:
                code = {0x60: f'adr x1, .+{table - 0x60}', 0x64: 'b .+112',
                        0x80: 'mov w0, #11', 0x84: 'ret', 0x88: 'mov w0, #22', 0x8c: 'ret',
                        0x90: 'mov w0, #33', 0x94: 'ret', 0xd4: 'cmp w0, #2', 0xd8: 'b.hi .+24',
                        0xdc: 'adr x2, .-92', 0xe0: 'ldrb w3, [x1, x0]', 0xe4: 'add x2, x2, x3, lsl #2',
                        0xe8: 'ldr x7, [x5]' if unrelated else 'nop',
                        0xec: 'br x2', 0xf0: 'mov w0, #99', 0xf4: 'ret'}
                branch, entry, cases, fmt, values = 0xec, 0xd4, [0x80, 0x88, 0x90], '<4B', (0, 2, 4, 0xff)
                if vector_write: code[0xe8] = 'movi v2.2d, #0'
                if branch_clobber: code[0xe8] = 'mov x2, x0'
                if call_base is not None:
                    code.update({0x60: 'mov x21, x30', 0x64: f'adr x{call_base}, .+{table - 0x64}',
                                 0x68: 'bl .+80', 0x6c: 'mov x30, x21', 0x70: 'b .+100',
                                 0xb8: 'mov x1, xzr', 0xbc: 'ret', 0xe0: f'ldrb w3, [x{call_base}, x0]'})
                if dead_base: code.update({0x68: f'adr x1, .+{table - 0x68}', 0x6c: 'add x1, x1, #2'})
                if unguarded: code.update({0xd4: 'nop', 0xd8: 'nop'})
                if unmapped:
                    code.update({0x64: 'b .+104', 0xcc: 'cmp w0, #2', 0xd0: 'b.hi .+32',
                                 0xd4: 'ldr x7, [x5]', 0xd8: 'ldr x7, [x7]'})
                    entry = 0xcc
                if masked:
                    code.update({0x98: 'mov w0, #44', 0x9c: 'ret', 0xd0: 'and w6, w0, #3',
                                 0xd4: 'cbz x5, .+28', 0xd8: 'add w6, w6, #5' if clobber else 'nop',
                                 0xe0: 'ldrb w3, [x1, x6]'})
                    if compare_mask: code.update({0xd4: 'cmp w6, #0', 0xd8: 'b.eq .-88'})
                    if alternate: code.update({0xd4: 'cbnz x5, .+8', 0xd8: 'br x7'})
                    code[0x64] = 'b .+108'
                    if indirect_entry: code.update({0x64: 'adr x7, .+108', 0x68: 'br x7'})
                    if far_mask: code.update({0x64: 'and w6, w0, #3', 0x68: 'b .+108', 0xd0: 'nop'})
                    entry, cases, fmt, values = 0xd0, [0x80, 0x88, 0x90, 0x98], '<5B', (0, 2, 4, 6, 0xff)
                    if subtract_mask: code[0xd8] = 'sub w6, w6, #1'; cases = cases[:3]
                if joined_base:
                    code.update({0x60: 'cbnz x4, .+16', 0x64: f'adr x1, .+{table - 0x64}', 0x68: 'b .+108',
                                 0x70: f'adr x1, .+{table + 16 - 0x70}', 0x74: 'b .+96',
                                 0xa0: 'mov w0, #55', 0xa4: 'ret', 0xa8: 'mov w0, #66', 0xac: 'ret',
                                 0xb0: 'mov w0, #77', 0xb4: 'ret'})
                    cases += [0xa0, 0xa8, 0xb0]
                    if selected_base:
                        code.update({0x60: f'adr x1, .+{table - 0x60}', 0x64: f'adr x6, .+{table + 16 - 0x64}',
                                     0x68: 'cmp x4, #0', 0x6c: 'csel x1, x1, x6, eq',
                                     0x70: 'mov x1, x7' if clobber else 'nop'})
                instructions = [code.get(at, 'nop') for at in range(0x60, 0xf8, 4)]
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
            elif previous_guard:
                instructions = ['b .+16', 'cmp w0, #2', 'b.hi .+52', 'ret',
                                f'adr x1, .+{table - 0x90}', 'adr x2, .+16',
                                'ldrb w3, [x1, x0]', 'add x2, x2, x3, lsl #2', 'br x2',
                                'mov w0, #11', 'ret', 'mov w0, #22', 'ret', 'mov w0, #33', 'ret',
                                'mov w0, #99', 'ret']
                branch, entry, cases, fmt, values = 0xa0, 0x90, [0xa4, 0xac, 0xb4], '<4B', (0, 2, 4, 0xff)
            elif independent_guard:
                instructions = ['cmp w4, #2', 'b.hi .+48', f'adr x1, .+{table - 0x88}', 'adr x2, .+16',
                                'ldrb w3, [x1, x0]', 'add x2, x2, x3, lsl #2', 'br x2',
                                'mov w0, #11', 'ret', 'mov w0, #22', 'ret', 'mov w0, #33', 'ret',
                                'mov w0, #99', 'ret']
                branch, entry, cases, fmt, values = 0x98, 0x80, [0x9c, 0xa4, 0xac], '<4B', (0, 2, 4, 0xff)
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
            if long_guard:
                # Move MOD0 into unused readonly space so the long straight-line fixture fits in text.
                struct.pack_into('<I', raw, 0x104, 0x470)
                struct.pack_into('<4s6i', raw, 0x570, b'MOD0', -0x370, 0x490, 0x490, -0x470, -0x470, 0)
            struct.pack_into('<Q', raw, 0x320, function_entry)
            struct.pack_into('<Q', raw, 0x328, len(words) * 4 if metadata else 0)
            struct.pack_into('<' + 'I' * len(words), raw, 0x100 + function_entry, *words)
            for offset in (0x500, 0x508, 0x6f8, 0x700): struct.pack_into('<Q', raw, offset + 0x100, function_entry)
            struct.pack_into(fmt, raw, table + 0x100, *values)
            if joined_base: struct.pack_into('<4B', raw, table + 0x110, 8, 10, 12, 0xff)
            path = directory / 'synthetic.nso'; path.write_bytes(raw)
            buffer = (ctypes.c_ubyte * 8192)(); base = (ctypes.addressof(buffer) + 4095) & ~4095
            module = gen.Module('synthetic', path, base, 'c', game_patches=False)
            targets = module.jump_table(base + branch, base + entry, base + (0xf0 if distant else cases[-1]))
            if not metadata or writable or clobber or branch_clobber or (call_base is not None and call_base < 19):
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
extern const uint64_t synthetic_fn_addrs[], synthetic_fn_count;
extern const GuestFn synthetic_fn_ptrs[];
void rt_dispatch(Cpu *c) {
    for (uint64_t i = 0; i < synthetic_fn_count; i++)
        if (synthetic_fn_addrs[i] == c->pc) { synthetic_fn_ptrs[i](c); return; }
    abort();
}
void rt_svc(Cpu *c, uint32_t i, uint64_t p) { (void)c; (void)i; (void)p; abort(); }
void rt_brk(Cpu *c, uint32_t i, uint64_t p) { (void)c; (void)i; (void)p; abort(); }
void rt_udf(Cpu *c, uint64_t p) { (void)c; (void)p; abort(); }
uint64_t rt_ticks(void) { abort(); }
''' + f'void f_{base + function_entry:x}(Cpu *c);\n'
                + f'uint64_t run(uint64_t value) {{ Cpu cpu = {{0}}; uint64_t cell = 0, pointer = (uintptr_t)&cell; cpu.x[5] = (uintptr_t)&pointer; cpu.x[4] = value >> 32; cpu.x[0] = {'(uint32_t)value' if joined_base else 'value'}; f_{base + function_entry:x}(&cpu); return cpu.x[0]; }}\n')
            library = directory / 'synthetic.so'
            subprocess.run(['clang', '-shared', '-fPIC', '-O1', '-I', str(ROOT / 'runtime'),
                            *map(str, output.glob('*.c')), str(wrapper), '-o', str(library)],
                           check=True, timeout=30, capture_output=True)
            ctypes.memmove(base, bytes(module.nso.image), len(module.nso.image))
            native = ctypes.CDLL(str(library)); native.run.argtypes = [ctypes.c_uint64]; native.run.restype = ctypes.c_uint64
            for value, expected in enumerate((11, 22, 33), int(subtract_mask)): self.assertEqual(native.run(value), expected)
            if joined_base:
                for value, expected in enumerate((55, 66, 77)): self.assertEqual(native.run((1 << 32) | value), expected)
                if not unguarded: self.assertEqual(native.run((1 << 32) | 3), 99)
            if subtract_mask:
                self.assertEqual(native.run(5), 11)
                self.assertEqual(native.run(7), 33)
            elif masked:
                self.assertEqual(native.run(3), 44)
                self.assertEqual(native.run(4), 11)
                self.assertEqual(native.run(7), 44)
            elif (guarded or distant or long_guard) and not unguarded: self.assertEqual(native.run(3), 99)

    def test_table_base_before_partition_is_not_uninitialized_scratch(self):
        self.check_table(1, distant=True)

    def test_vector_write_does_not_clobber_general_branch_register(self):
        self.check_table(1, distant=True, unguarded=True, vector_write=True)
        self.check_table(1, distant=True, unguarded=True, branch_clobber=True)

    def test_local_mask_after_an_indirect_predecessor(self):
        self.check_table(1, distant=True, masked=True, indirect_entry=True)

    def test_table_base_join_preserves_both_predecessors(self):
        self.check_table(1, distant=True, joined_base=True)
        self.check_table(1, distant=True, unguarded=True, joined_base=True)

    def test_conditional_select_preserves_both_table_bases(self):
        self.check_table(1, distant=True, joined_base=True, selected_base=True)
        self.check_table(1, distant=True, joined_base=True, selected_base=True, unguarded=True)

    def test_calls_preserve_callee_saved_but_not_caller_saved_table_bases(self):
        self.check_table(1, distant=True, call_base=24)
        self.check_table(1, distant=True, call_base=1)

    def test_unknown_write_invalidates_a_previous_table_base(self):
        self.check_table(1, distant=True, joined_base=True, selected_base=True, clobber=True)
        self.check_table(1, distant=True, joined_base=True, selected_base=True, clobber=True, unguarded=True)

    def test_table_base_limit_does_not_accept_a_partial_set(self):
        with tempfile.TemporaryDirectory(prefix='table-base-limit-') as temp:
            directory = Path(temp)
            instructions = ['adr x1, .+640']
            for bit in range(7):
                instructions += [f'tst w4, #{1 << bit}', f'add x6, x1, #{1 << bit}', 'csel x1, x1, x6, eq']
            instructions += ['and w0, w0, #3', 'adr x2, .+16', 'ldrb w3, [x1, x0]',
                             'add x2, x2, x3, lsl #2', 'br x2', 'mov w0, #11', 'ret', 'mov w0, #22', 'ret']
            words = assemble(directory, instructions)
            raw = fixture()
            struct.pack_into('<ii', raw, 0x150, -0x40, -0x40)
            struct.pack_into('<QQ', raw, 0x320, 0x80, len(words) * 4)
            struct.pack_into('<' + 'I' * len(words), raw, 0x180, *words)
            struct.pack_into('<3B', raw, 0x400, 0, 2, 0xff)
            path = directory / 'synthetic.nso'; path.write_bytes(raw)
            module = gen.Module('synthetic', path, 0x100000, 'c', game_patches=False)
            branch = 0x100080 + (len(words) - 5) * 4
            self.assertIsNone(module.jump_table(branch, 0x100080, branch + 20))

    def test_unreachable_address_writes_do_not_change_table_base(self):
        self.check_table(1, distant=True, dead_base=True)
        self.check_table(1, distant=True, masked=True, dead_base=True)
        self.check_table(1, distant=True, unguarded=True, dead_base=True)

    def test_masked_subtraction_limits_state_candidates(self):
        self.check_table(1, distant=True, masked=True, subtract_mask=True)

    def test_unguarded_table_base_before_control_flow(self):
        self.check_table(1, distant=True, unguarded=True)
        self.check_table(1, distant=True, unguarded=True, writable=True)

    def test_scheduled_guard_preserves_distant_comparison_flags(self):
        self.check_table(1, long_guard=True)

    def test_previous_block_guard_does_not_hide_a_state_table(self):
        self.check_table(1, previous_guard=True)

    def test_state_index_is_not_bounded_by_an_unrelated_guard(self):
        self.check_table(1, independent_guard=True)

    def test_comparison_alias_does_not_clobber_a_masked_index(self):
        self.check_table(1, distant=True, masked=True, compare_mask=True)

    def test_guarded_distant_base_ignores_unmapped_object_reads(self):
        self.check_table(1, distant=True, unmapped=True)

    def test_masked_index_with_loop_invariant_table_base(self):
        self.check_table(1, distant=True, masked=True)
        self.check_table(1, distant=True, masked=True, alternate=True)
        self.check_table(1, distant=True, masked=True, far_mask=True)
        self.check_table(1, distant=True, masked=True, clobber=True)
        self.check_table(1, distant=True, masked=True, writable=True)
        self.check_table(1, distant=True, masked=True, metadata=False)

    def test_unrelated_scratch_read_does_not_hide_the_table(self):
        self.check_table(1, distant=True, unrelated=True)

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
