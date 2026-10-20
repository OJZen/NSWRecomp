"""Synthetic native/Unicorn differential tests for bounded exclusive transactions and FP/SIMD."""
import ctypes
from pathlib import Path
import random
import struct
import subprocess
import tempfile
import unittest

import capstone
import unicorn as uc
from unicorn import arm64_const as A
from lifter import Lifter, exclusive_pair_transaction

ROOT = Path(__file__).resolve().parents[1]
CODE = 0x100000
REGS = [getattr(A, f'UC_ARM64_REG_X{i}') for i in range(31)]
CONFIGS = [(0, 1, 2, 3, 0, 1), (31, 1, 2, 1, 4, 5), (0, 1, 2, 3, 1, 0),
           (0, 1, 31, 3, 4, 0), (0, 1, 2, 31, 4, 5)]
FP = [(0x1e222820, 'fadd'), (0x1e620820, 'fmul'), (0x1e621820, 'fdiv'),
      (0x9e780020, 'fcvtzs'), (0x4e22cc20, 'fmla')]


def pair_words(size, acquire, release, regs):
    t, t2, n, s, u, u2 = regs
    return ((size << 30) | 0x087f0000 | acquire << 15 | t2 << 10 | n << 5 | t,
            (size << 30) | 0x08200000 | s << 16 | release << 15 | u2 << 10 | n << 5 | u)


class CpuSemanticsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='cpu-semantics-')
        cls.directory = Path(cls.temp.name)
        cls.cases = []
        code = ['#include "cpu.h"', '#include "simd.h"']
        for size in (2, 3):
            for acq in (0, 1):
                for rel in (0, 1):
                    for regs in CONFIGS:
                        load, store = pair_words(size, acq, rel, regs)
                        body = exclusive_pair_transaction(load, store, CODE)
                        assert body is not None
                        index = len(cls.cases)
                        code.append(f'void p{index}(Cpu *c) {{ {body} }}')
                        cls.cases.append((size, acq, rel, regs, load, store))
        code.append('GuestFn pair_tests[] = {' + ','.join(f'p{i}' for i in range(len(cls.cases))) + '};')
        lifter = Lifter(0, 'c')
        code.append('void test_clrex(Cpu *c) {' + lifter.lift(0xd5033f5f, CODE).code + '}')
        md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        for i, (word, mnemonic) in enumerate(FP):
            assert next(md.disasm(struct.pack('<I', word), CODE)).mnemonic == mnemonic
            code.append(f'void fp{i}(Cpu *c) {{ {lifter.lift(word, CODE).code} }}')
        source = cls.directory / 'cases.c'
        source.write_text('\n'.join(code))
        library = cls.directory / 'cases.so'
        subprocess.run(['clang', '-O1', '-fno-strict-aliasing', '-shared', '-fPIC', '-pthread',
                        '-I', str(ROOT / 'runtime'), str(source), str(ROOT / 'tools/test_cpu_native.c'),
                        '-lm', '-o', str(library)], check=True, timeout=60, capture_output=True)
        cls.lib = ctypes.CDLL(str(library))
        cls.buffer = (ctypes.c_ubyte * 8192)()
        cls.address = (ctypes.addressof(cls.buffer) + 4095) & ~4095
        cls.mu = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
        cls.mu.mem_map(CODE, 4096)
        cls.mu.mem_map(cls.address, 4096)
        cls.state = (ctypes.c_uint64 * 128)()

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def compare(self, words, function, seed, regs=None, fpcr=0):
        rng = random.Random(seed)
        state, mu = self.state, self.mu
        ctypes.memset(ctypes.addressof(state), 0, ctypes.sizeof(state))
        for i in range(31):
            state[i] = rng.getrandbits(64)
        if regs:
            n = regs[2]
            state[32 if n == 31 else n] = self.address
        state[34], state[40] = rng.randrange(16) << 28, fpcr
        for i in range(64, 128):
            state[i] = rng.choice((0, 0x8000000000000000, 0x3ff0000000000000,
                                  0x7ff0000000000000, 0x7ff8000000001234, 1,
                                  0x3f8000003f800000, 0x7fc123457f812345, rng.getrandbits(64)))
        before = rng.randbytes(64)
        ctypes.memmove(self.address, before, len(before)); mu.mem_write(self.address, before)
        # A new translation invalidates Unicorn's cached instruction bytes.
        mu.mem_write(CODE, struct.pack('<' + 'I' * len(words), *words))
        mu.ctl_remove_cache(CODE, CODE + 4096)
        for i, register in enumerate(REGS): mu.reg_write(register, state[i])
        mu.reg_write(A.UC_ARM64_REG_SP, state[32])
        mu.reg_write(A.UC_ARM64_REG_NZCV, state[34])
        mu.reg_write(A.UC_ARM64_REG_FPCR, fpcr)
        mu.reg_write(A.UC_ARM64_REG_FPSR, 0)
        for i in range(32): mu.reg_write(A.UC_ARM64_REG_Q0 + i, state[64 + 2*i] | state[65 + 2*i] << 64)
        mu.emu_start(CODE, CODE + 4 * len(words), count=len(words))
        native = getattr(self.lib, function)
        native.argtypes, native.restype = [ctypes.c_void_p], None
        native(ctypes.byref(state))
        self.assertEqual(list(state[:31]), [mu.reg_read(r) for r in REGS])
        self.assertEqual(state[32], mu.reg_read(A.UC_ARM64_REG_SP))
        self.assertEqual(state[34], mu.reg_read(A.UC_ARM64_REG_NZCV))
        self.assertEqual(ctypes.string_at(self.address, len(before)), bytes(mu.mem_read(self.address, len(before))))
        for i in range(32):
            self.assertEqual(state[64+2*i] | state[65+2*i] << 64, mu.reg_read(A.UC_ARM64_REG_Q0 + i))

    def test_adjacent_pair_results(self):
        for index, (_, _, _, regs, load, store) in enumerate(self.cases):
            for seed in range(32):
                with self.subTest(index=index, seed=seed):
                    self.compare([load, store], f'p{index}', seed, regs)

    def test_scalar_reservation_failures(self):
        self.assertEqual(self.lib.test_scalar_exclusives(), 0)

    def test_alignment_failure(self):
        self.assertEqual(self.lib.test_misaligned_pair(0), 0)
        self.assertEqual(self.lib.test_misaligned_pair(20), 0)

    def test_acquire_release_threads(self):
        self.assertEqual(self.lib.test_pair_threads(30, 26), 0)

    def test_fp_simd_results(self):
        for index, (word, _) in enumerate(FP):
            for fpcr in (0, 1 << 24, 1 << 25, 1 << 22, 2 << 22, 3 << 22):
                for seed in range(32):
                    with self.subTest(index=index, fpcr=fpcr, seed=seed):
                        self.compare([word], f'fp{index}', seed, fpcr=fpcr)

    def test_invalid_pair_forms_stay_unsupported(self):
        load, store = pair_words(3, 1, 1, CONFIGS[0])
        for a, b in ((load, store ^ (1 << 30)), (load, store ^ (1 << 5)),
                     (load & 0x3fffffff, store & 0x3fffffff), (load, 0xd503201f)):
            self.assertIsNone(exclusive_pair_transaction(a, b, CODE))


if __name__ == '__main__':
    unittest.main()
