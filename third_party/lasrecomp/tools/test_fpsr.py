"""Synthetic FPSR tests: lifted C versus Unicorn and, on AArch64, native instructions."""
import ctypes
from native_test import native_library
import json
from pathlib import Path
import platform
import random
import struct
import subprocess
import tempfile
import unittest

import unicorn as uc
from unicorn import arm64_const as A
from lifter import Lifter

ROOT = Path(__file__).resolve().parents[1]
CODE = 0x100000
QC, IOC, IXC, IDC = 1 << 27, 1, 16, 128
MASK128 = (1 << 128) - 1


def cases():
    result = []
    def add(group, asm, bits=32):
        result.append((group, asm.split(';'), bits))
    for bits, letter, lo, hi in ((8, 'b', '8b', '16b'), (16, 'h', '4h', '8h'),
                                (32, 's', '2s', '4s'), (64, 'd', None, '2d')):
        forms = [(f'{letter}0', f'{letter}1', f'{letter}2')]
        forms += [(f'v0.{v}', f'v1.{v}', f'v2.{v}') for v in (lo, hi) if v]
        for d, n, m in forms:
            for op in ('sqadd', 'uqadd', 'sqsub', 'uqsub', 'sqshl', 'uqshl', 'sqrshl', 'uqrshl'):
                add('qc', f'{op} {d}, {n}, {m}')
            for op in ('suqadd', 'usqadd', 'sqabs', 'sqneg'):
                add('qc', f'{op} {d}, {n}')
            for op in ('sqshl', 'uqshl', 'sqshlu'):
                for shift in (0, bits - 1):
                    add('qc', f'{op} {d}, {n}, #{shift}')
    for bits, d, n, lo, hi, src in ((8, 'b0', 'h1', '8b', '16b', '8h'),
                                   (16, 'h0', 's1', '4h', '8h', '4s'),
                                   (32, 's0', 'd1', '2s', '4s', '2d')):
        for suffix, dest, source in (('', d, n), ('', f'v0.{lo}', f'v1.{src}'),
                                     ('2', f'v0.{hi}', f'v1.{src}')):
            for op in ('sqxtn', 'uqxtn', 'sqxtun'):
                add('qc', f'{op}{suffix} {dest}, {source}')
            for op in ('sqshrn', 'uqshrn', 'sqrshrn', 'uqrshrn', 'sqshrun', 'sqrshrun'):
                for shift in (1, bits):
                    add('qc', f'{op}{suffix} {dest}, {source}, #{shift}')
    for bits, src, dst, lo, hi, wide, index in ((16, 'h', 's', '4h', '8h', '4s', 3),
                                               (32, 's', 'd', '2s', '4s', '2d', 1)):
        for op in ('sqdmull', 'sqdmlal', 'sqdmlsl'):
            for suffix, d, n, m in (('', f'{dst}0', f'{src}1', f'{src}2'),
                                     ('', f'v0.{wide}', f'v1.{lo}', f'v2.{lo}'),
                                     ('2', f'v0.{wide}', f'v1.{hi}', f'v2.{hi}'),
                                     ('', f'{dst}0', f'{src}1', f'v2.{src}[{index}]'),
                                     ('', f'v0.{wide}', f'v1.{lo}', f'v2.{src}[{index}]'),
                                     ('2', f'v0.{wide}', f'v1.{hi}', f'v2.{src}[{index}]')):
                add('qc', f'{op}{suffix} {d}, {n}, {m}')
        for op in ('sqdmulh', 'sqrdmulh'):
            for form in (src, lo, hi):
                regs = [f'{form}{i}' if form == src else f'v{i}.{form}' for i in range(3)]
                add('qc', f'{op} ' + ', '.join(regs))
                add('qc', f'{op} {regs[0]}, {regs[1]}, v2.{src}[{index}]')
    for asm in ('sqadd v1.16b, v1.16b, v2.16b', 'uqsub v2.2d, v1.2d, v2.2d',
                'sqdmlal v1.4s, v1.4h, v2.4h', 'sqdmulh v2.4s, v1.4s, v2.s[1]',
                'sqxtn2 v1.16b, v1.8h', 'sqrshrn2 v1.8h, v1.4s, #16'):
        add('qc', asm)
    for bits, scalar, vectors in ((32, 's', ('2s', '4s')), (64, 'd', ('2d',))):
        for op in ('fcmp', 'fcmpe'):
            for source in (f'{scalar}2', '#0.0'):
                add('fp', f'{op} {scalar}1, {source}', bits)
        for op in ('fccmp', 'fccmpe'):
            add('fp', f'{op} {scalar}1, {scalar}2, #5, eq', bits)
        forms = [(f'{scalar}0', f'{scalar}1', f'{scalar}2')]
        forms += [(f'v0.{v}', f'v1.{v}', f'v2.{v}') for v in vectors]
        for d, n, m in forms:
            for op in ('fcmeq', 'fcmge', 'fcmgt', 'facge', 'facgt'):
                add('fp', f'{op} {d}, {n}, {m}', bits)
            for op in ('fcmeq', 'fcmge', 'fcmgt', 'fcmle', 'fcmlt'):
                add('fp', f'{op} {d}, {n}, #0.0', bits)
            for op in ('frintn', 'frintp', 'frintm', 'frintz', 'frinta', 'frintx', 'frinti'):
                add('fp', f'{op} {d}, {n}', bits)
            for rounding in 'npmza':
                for signed in 'su':
                    add('fp', f'fcvt{rounding}{signed} {d}, {n}', bits)
            for op in ('fcvtzs', 'fcvtzu'):
                for scale in (1, bits):
                    add('fp', f'{op} {d}, {n}, #{scale}', bits)
            for op in ('fmax', 'fmin', 'fmaxnm', 'fminnm'):
                add('fp', f'{op} {d}, {n}, {m}', bits)
        for integer, ib in (('w0', 32), ('x0', 64)):
            for rounding in 'npmza':
                for signed in 'su':
                    add('fp', f'fcvt{rounding}{signed} {integer}, {scalar}1', bits)
            for op in ('fcvtzs', 'fcvtzu'):
                for scale in (1, ib):
                    add('fp', f'{op} {integer}, {scalar}1, #{scale}', bits)
    add('state', 'msr fpsr, x1;mrs x3, fpsr;msr fpsr, xzr;mrs x2, fpsr')
    add('state', 'sqadd v0.16b, v1.16b, v2.16b;mrs x3, fpsr;'
                 'msr fpsr, xzr;uqsub v0.16b, v1.16b, v2.16b;mrs x2, fpsr')
    return result


def assemble(directory, instructions):
    source, output = directory / 'cases.s', directory / 'cases.o'
    source.write_text('.text\n' + '\n'.join(instructions) + '\n')
    subprocess.run(['clang', '-target', 'aarch64-none-elf', '-c', str(source), '-o', str(output)],
                   check=True, timeout=30)
    raw = output.read_bytes()
    assert raw[:6] == b'\x7fELF\x02\x01'
    shoff = struct.unpack_from('<Q', raw, 40)[0]
    stride, count, strings_index = struct.unpack_from('<HHH', raw, 58)
    headers = [struct.unpack_from('<IIQQQQIIQQ', raw, shoff + i * stride) for i in range(count)]
    strings = headers[strings_index]
    names = raw[strings[4]:strings[4] + strings[5]]
    section = next(h for h in headers if names[h[0]:].split(b'\0', 1)[0] == b'.text')
    code = raw[section[4]:section[4] + section[5]]
    assert len(code) == 4 * len(instructions)
    return list(struct.unpack('<' + 'I' * len(instructions), code))


def hardware_function(index, words):
    asm = ('msr fpcr, %[cr]\nmsr fpsr, %[sr]\nmsr nzcv, %[nz]\n'
           'ldp q0, q1, [%[v]]\nldp q2, q3, [%[v], #32]\n'
           'ldp x0, x1, [%[x]]\nldp x2, x3, [%[x], #16]\n' +
           '\n'.join(f'.inst 0x{w:08x}' for w in words) +
           '\nstp x0, x1, [%[x]]\nstp x2, x3, [%[x], #16]\n'
           'stp q0, q1, [%[v]]\nstp q2, q3, [%[v], #32]\n'
           'mrs %[out_sr], fpsr\nmrs %[out_nz], nzcv\n')
    return f'''#if defined(__aarch64__)
void hw{index}(Cpu *c) {{
    uint64_t cr, sr, nz;
    __asm__ volatile("mrs %0, fpcr; mrs %1, fpsr; mrs %2, nzcv" : "=r"(cr), "=r"(sr), "=r"(nz));
    __asm__ volatile({json.dumps(asm)}
        : [out_sr] "=r"(c->fpsr), [out_nz] "=r"(c->nzcv)
        : [cr] "r"(c->fpcr), [sr] "r"(c->fpsr), [nz] "r"(c->nzcv), [v] "r"(c->v), [x] "r"(c->x)
        : "x0", "x1", "x2", "x3", "v0", "v1", "v2", "v3", "memory", "cc");
    __asm__ volatile("msr fpcr, %0; msr fpsr, %1; msr nzcv, %2" :: "r"(cr), "r"(sr), "r"(nz) : "memory", "cc");
}}
#endif'''


def repeat(value, bits):
    return sum(value << shift for shift in range(0, 128, bits))


class FpsrTests(unittest.TestCase):
    optimization = '-O1'

    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='fpsr-')
        cls.addClassCleanup(cls.temp.cleanup)
        directory = Path(cls.temp.name)
        cls.cases = cases()
        encoded = assemble(directory, [line for _, lines, _ in cls.cases for line in lines])
        lifter, pos, code = Lifter(0, 'c'), 0, ['#include "cpu.h"', '#include "simd.h"']
        cls.words = []
        for i, (_, lines, _) in enumerate(cls.cases):
            words = encoded[pos:pos + len(lines)]
            pos += len(lines)
            cls.words.append(words)
            body = ' '.join(lifter.lift(word, CODE + j * 4).code for j, word in enumerate(words))
            code += [f'void c{i}(Cpu *c) {{ {body} }}', hardware_function(i, words)]
        source, library = directory / 'lifted.c', directory / 'lifted.so'
        source.write_text('\n'.join(code))
        subprocess.run(['clang', cls.optimization, '-fno-strict-aliasing', '-shared', '-fPIC', '-I', str(ROOT / 'runtime'),
                        str(source), '-lm', '-o', str(library)], check=True, timeout=60)
        cls.lib = cls.enterClassContext(native_library(library))
        cls.hw = platform.machine().lower() in ('arm64', 'aarch64')
        cls.mu = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
        cls.mu.mem_map(CODE, 4096)
        cls.state_type = ctypes.c_uint64 * 128
        cls.checked = cls.hardware_checked = 0
        print(f'FPSR: {len(cls.cases)} instruction sequences; native AArch64 oracle={cls.hw}', flush=True)

    @classmethod
    def tearDownClass(cls):
        print(f'FPSR cases passed: C/Unicorn={cls.checked}, native AArch64={cls.hardware_checked}', flush=True)

    def compare(self, index, v0, v1, v2, seed, fpcr=0, fpsr=None, x1=None, hardware=True):
        r, mu = random.Random(seed), self.mu
        state = self.state_type()
        for i in range(31): state[i] = r.getrandbits(64)
        state[34], state[40] = (seed % 16) << 28, fpcr
        state[41] = (0, QC, 0x9f)[seed % 3] if fpsr is None else fpsr
        state[1] = r.getrandbits(32) & (QC | 0x9f) if x1 is None else x1
        for i, value in enumerate((v0, v1, v2, r.getrandbits(128))):
            state[64 + 2*i], state[65 + 2*i] = value & ((1 << 64) - 1), value >> 64
        expected = self.state_type.from_buffer_copy(state)
        native = self.state_type.from_buffer_copy(state)
        words = self.words[index]
        mu.mem_write(CODE, struct.pack('<' + 'I' * len(words), *words))
        mu.ctl_remove_cache(CODE, CODE + 4096)
        for i in range(4): mu.reg_write(getattr(A, f'UC_ARM64_REG_X{i}'), state[i])
        for i in range(4): mu.reg_write(A.UC_ARM64_REG_Q0 + i, state[64+2*i] | state[65+2*i] << 64)
        for register, field in ((A.UC_ARM64_REG_NZCV, 34), (A.UC_ARM64_REG_FPCR, 40), (A.UC_ARM64_REG_FPSR, 41)):
            mu.reg_write(register, state[field])
        mu.emu_start(CODE, CODE + 4 * len(words), count=len(words))
        for i in range(4): expected[i] = mu.reg_read(getattr(A, f'UC_ARM64_REG_X{i}'))
        for i in range(4):
            value = mu.reg_read(A.UC_ARM64_REG_Q0 + i)
            expected[64+2*i], expected[65+2*i] = value & ((1 << 64) - 1), value >> 64
        expected[34], expected[41] = mu.reg_read(A.UC_ARM64_REG_NZCV), mu.reg_read(A.UC_ARM64_REG_FPSR)
        for prefix, target in [('c', state)] + ([('hw', native)] if self.hw and hardware else []):
            fn = getattr(self.lib, f'{prefix}{index}')
            fn.argtypes, fn.restype = [ctypes.c_void_p], None
            fn(ctypes.byref(target))
            for at, (actual, wanted) in enumerate(zip(target, expected)):
                if actual != wanted:
                    self.fail(f'{prefix}: {self.cases[index][1]} seed={seed} fpcr={fpcr:x} word={at}: {actual:x} != {wanted:x}')
        type(self).checked += 1
        type(self).hardware_checked += bool(self.hw and hardware)
        return state

    def test_saturation_state(self):
        patterns = [(0, 0, 0), (MASK128, MASK128, MASK128)]
        for bits in (8, 16, 32, 64):
            top, maximum = 1 << (bits - 1), (1 << bits) - 1
            patterns += [(0, repeat(top - 1, bits), repeat(1, bits)),
                         (MASK128, repeat(top, bits), repeat(top, bits)),
                         (0, 0, repeat(maximum, bits)),
                         (0, repeat(top - 1, bits) & (MASK128 << 64), repeat(1, bits) & (MASK128 << 64))]
        rng = random.Random(2801)
        patterns += [tuple(rng.getrandbits(128) for _ in range(3)) for _ in range(16)]
        for i, (group, _, _) in enumerate(self.cases):
            if group == 'qc':
                for seed, values in enumerate(patterns):
                    self.compare(i, *values, seed)

    def test_float_state(self):
        values = {
            32: [0, 0x80000000, 0x3f800000, 0x3fc00000, 0xbfc00000, 0x7f800000, 0xff800000,
                 0x7fc12345, 0x7f812345, 1, 0x80000001, 0x007fffff, 0x7f7fffff, 0x4f000000, 0xcf000000, 0x5f800000],
            64: [0, 1 << 63, 0x3ff0000000000000, 0x3ff8000000000000, 0xbff8000000000000,
                 0x7ff0000000000000, 0xfff0000000000000, 0x7ff8000000012345, 0x7ff0000000012345,
                 1, 0x8000000000000001, 0xfffffffffffff, 0x7fefffffffffffff,
                 0x41e0000000000000, 0xc1e0000000000000, 0x43f0000000000000]}
        for i, (group, _, bits) in enumerate(self.cases):
            if group != 'fp': continue
            data = values[bits]
            for fpcr in (0, 1 << 24, 1 << 25, 1 << 22, 2 << 22, 3 << 22):
                for seed, value in enumerate(data):
                    self.compare(i, MASK128, repeat(value, bits), repeat(data[(seed * 7 + 3) % len(data)], bits), seed, fpcr, fpsr=0)

    def test_float_flags_accumulate(self):
        i = next(i for i, (_, lines, _) in enumerate(self.cases) if lines == ['fcmpe s1, s2'])
        state = self.compare(i, 0, repeat(0x7fc12345, 32), 0, 0, fpsr=QC | IXC | 6)
        self.assertEqual(state[41], QC | IXC | 6 | IOC)

    def test_qc_intermediate_saturation_inactive_lanes_and_sticky_state(self):
        def run(asm, v0, v1, v2, fpsr=0):
            i = next(i for i, (_, lines, _) in enumerate(self.cases) if lines == [asm])
            return self.compare(i, v0, v1, v2, 0, fpsr=fpsr)
        self.assertEqual(run('sqadd v0.8b, v1.8b, v2.8b', 0, 0, 0, QC | IXC)[41], QC | IXC)
        self.assertEqual(run('sqadd v0.8b, v1.8b, v2.8b', 0,
                             repeat(127, 8) & (MASK128 << 64), repeat(1, 8) & (MASK128 << 64))[41], 0)
        self.assertEqual(run('uqadd v0.16b, v1.16b, v2.16b', 0, MASK128, 0)[41], 0)
        state = run('sqdmlal s0, h1, h2', 0xffffffff, 0x8000, 0x8000)
        self.assertEqual((state[64], state[41]), (0x7ffffffe, QC))
        state = run('sqdmlsl s0, h1, h2', 0x7fffffff, 0x8000, 0x8000)
        self.assertEqual((state[64], state[41]), (0, QC))

    def test_status_register_roundtrip_and_sticky_clear(self):
        for i, (group, _, _) in enumerate(self.cases):
            if group != 'state': continue
            for seed in range(12):
                self.compare(i, 0, repeat(127, 8), repeat(1, 8), seed)
        i = next(i for i, (group, _, _) in enumerate(self.cases) if group == 'state')
        state = self.compare(i, 0, 0, 0, 0, x1=(1 << 64) - 1, hardware=False)
        self.assertEqual(state[3], 0xf800009f)  # Cortex-A57 retains AArch32 condition fields.
        self.assertEqual(state[2], 0)


class FpsrO2Tests(FpsrTests):
    optimization = '-O2'


if __name__ == '__main__':
    unittest.main()
