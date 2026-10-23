"""Compare configured hash-locked memory/string routines with Unicorn and independent results."""
import argparse
from collections import Counter
import ctypes
import json
from pathlib import Path
import random

import unicorn as uc
from unicorn import arm64_const as A
from recompile import Nso, profile, verify


def probe(exefs, library):
    info = verify(exefs)['rtld']
    n, base = Nso(exefs / 'rtld'), int(info['base'], 0)
    functions = profile()['rtld_probe_functions']
    if not functions or not functions.keys() <= {'memset', 'strlen', 'memmove', 'strcmp'}:
        raise ValueError('configure one or more supported rtld probe functions')
    if any(int(offset, 0) % 4 or not n.text[0] <= int(offset, 0) < sum(n.text)
           for offset in functions.values()):
        raise ValueError('rtld probe function must be an aligned text address')
    lib = ctypes.CDLL(str(library))
    lib.probe_cpu_size.restype = ctypes.c_size_t
    if lib.probe_cpu_size() != 1024:
        raise ValueError('unexpected Cpu layout')
    storage = (ctypes.c_ubyte * (16384 + 4095))()
    address = (ctypes.addressof(storage) + 4095) & ~4095
    buffer = (ctypes.c_ubyte * 16384).from_address(address)
    state = (ctypes.c_uint64 * 128)()
    oracle = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
    oracle.mem_map(base, n.size)
    oracle.mem_write(base, bytes(n.image))
    oracle.mem_map(address, len(buffer))
    sentinel = base + n.size + 4096
    oracle.mem_map(sentinel, 4096)
    regs = [getattr(A, f'UC_ARM64_REG_X{i}') for i in range(31)]
    rng = random.Random(0x143)
    initial = bytes((i * 37 + 19) & 255 for i in range(len(buffer)))
    counts = Counter()

    def check(kind, args, before, after, answer):
        if kind not in functions:
            return
        offset = int(functions[kind], 0)
        function = getattr(lib, f'f_{base + offset:x}')
        function.argtypes, function.restype = [ctypes.c_void_p], None
        ctypes.memset(ctypes.addressof(state), 0, ctypes.sizeof(state))
        for i in range(31):
            state[i] = rng.getrandbits(64)
        for i, value in enumerate(args):
            state[i] = value
        state[30], state[32], state[34] = sentinel, address + 16000, rng.randrange(16) << 28
        for i, register in enumerate(regs):
            oracle.reg_write(register, state[i])
        oracle.reg_write(A.UC_ARM64_REG_SP, state[32])
        oracle.reg_write(A.UC_ARM64_REG_NZCV, state[34])
        oracle.mem_write(address, bytes(before))
        oracle.emu_start(base + offset, sentinel, timeout=1000000, count=100000)
        if oracle.reg_read(A.UC_ARM64_REG_PC) != sentinel:
            raise AssertionError(f'{kind}: reference execution did not return within budget')
        ctypes.memmove(address, bytes(before), len(buffer))
        function(ctypes.byref(state))
        expected_registers = [oracle.reg_read(register) for register in regs]
        if list(state[:31]) != expected_registers or state[32] != oracle.reg_read(A.UC_ARM64_REG_SP):
            raise AssertionError(f'{kind}: register mismatch at case {counts[kind]}')
        if state[34] != oracle.reg_read(A.UC_ARM64_REG_NZCV):
            raise AssertionError(f'{kind}: NZCV mismatch at case {counts[kind]}')
        if state[0] != answer:
            raise AssertionError(f'{kind}: unexpected result at case {counts[kind]}')
        if bytes(buffer) != bytes(after) or bytes(buffer) != bytes(oracle.mem_read(address, len(buffer))):
            raise AssertionError(f'{kind}: memory or guard-byte mismatch at case {counts[kind]}')
        counts[kind] += 1

    lengths = (0, 1, 2, 3, 4, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65,
               95, 96, 97, 127, 128, 129, 255, 256, 257, 511, 1024, 2048)
    for length in lengths:
        for alignment in range(8):
            start = 4096 + alignment
            for value in (0, 0x80, 0xff, 0x1234):
                after = bytearray(initial)
                after[start:start + length] = bytes([value & 255]) * length
                check('memset', (address + start, value, length), initial, after, address + start)
            text = bytes(1 + (i * 71 % 255) for i in range(length))
            before = bytearray(initial)
            before[start:start + length + 1] = text + b'\0'
            check('strlen', (address + start,), before, before, length)
            for shift in (-64, -1, 0, 1, 64, 4096):
                target = start + shift
                after = bytearray(initial)
                after[target:target + length] = initial[start:start + length]
                check('memmove', (address + target, address + start, length), initial, after, address + target)
            for change in (None, 0, length // 2):
                other = 10240 + ((alignment * 3) % 8)
                before = bytearray(initial)
                before[start:start + length + 1] = text + b'\0'
                before[other:other + length + 1] = text + b'\0'
                answer = 0
                if change is not None and length:
                    before[other + change] ^= 0x80
                    answer = (before[start + change] - before[other + change]) & 0xffffffff
                check('strcmp', (address + start, address + other), before, before, answer)
    return {'status': 'passed', 'cases': dict(counts), 'total': sum(counts.values()),
            'functions': functions,
            'scope': 'configured rtld routines; registers, NZCV, return values and all buffer guard bytes',
            'game_boot_tested': False}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exefs', type=Path, required=True)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--result', type=Path, required=True)
    args = parser.parse_args()
    result = probe(args.exefs, args.library)
    args.result.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))
