"""Prepare a bounded NSO relocation plan and verify real native mappings without executing guest code."""
import argparse
from collections import Counter
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib

from bounded import run
from host import executable, lock_file
from build import BUILD, RUNTIME, context, write_json
from recompile import ROOT, Nso, digest, generation_inputs, prepare_work, profile, verify

HEADER = struct.Struct('<15Q')
MASK64 = (1 << 64) - 1
R_NONE, R_ABS64, R_GLOB_DAT, R_JUMP_SLOT, R_RELATIVE = 0, 257, 1025, 1026, 1027
ADDRESS_UNAVAILABLE = 75  # runtime/load.h; distinct from table/CLI validation failures.


def contains(start, size, address, length):
    return length >= 0 and start <= address and address + length <= start + size


class Module:
    def __init__(self, name, base, nso):
        self.name, self.base, self.nso = name, base, nso
        n = nso
        if (base < 65536 or base % 65536 or n.size > 128 << 20 or base + n.size > MASK64
                or not sum(n.data) <= n.bss_start <= n.bss_end <= sum(n.data) + n.bss_size):
            raise ValueError(f'{name}: invalid module or BSS extent')
        # The analysis parser need not retain visibility; a linker must inspect it.
        self.symbols = []
        for i in range(len(n.syms)):
            at = n.dyn[6] + i * 24
            name_offset, info, other, section, value, size = struct.unpack_from('<IBBHQQ', n.image, at)
            if info >> 4 not in (0, 1, 2) or info & 15 not in (0, 1, 2, 3) or other > 3:
                raise ValueError(f'{name}: unsupported symbol attributes')
            strsz = n.dyn.get(10, 0)
            if not contains(0, n.size, n.strtab, strsz) or name_offset >= strsz:
                raise ValueError(f'{name}: symbol string outside string table')
            end = n.image.find(b'\0', n.strtab + name_offset, n.strtab + strsz)
            if end < 0:
                raise ValueError(f'{name}: unterminated symbol name')
            symbol_name = bytes(n.image[n.strtab + name_offset:end])
            if section and section != 0xfff1 and (section >= 0xff00 or not contains(0, n.size, value, size)):
                raise ValueError(f'{name}: symbol outside image or unsupported section')
            self.symbols.append((symbol_name, info >> 4, info & 15, other, section, value, size))
        if self.symbols and self.symbols[0] != (b'', 0, 0, 0, 0, 0, 0):
            raise ValueError(f'{name}: invalid null symbol')
        if (n.dyn.get(9, 24) != 24 or n.dyn.get(20, 7) != 7
                or any(t in n.dyn for t in (16, 17, 18, 19, 22, 32, 33, 0x6ffffff0, 0x6ffffffe))
                or n.dyn.get(30, 0) & ~8 or n.dyn.get(0x6ffffffb, 0) & ~(1 | 0x8000000)):
            raise ValueError(f'{name}: unsupported dynamic linking mode')
        self.resolved = {}
        self.weak_unresolved = set()

    def defined_value(self, symbol):
        return symbol[5] if symbol[4] == 0xfff1 else self.base + symbol[5]

    def resolve(self, index, scope):
        if index in self.resolved:
            return self.resolved[index]
        if not 0 <= index < len(self.symbols):
            raise ValueError(f'{self.name}: invalid relocation symbol index')
        s = self.symbols[index]
        if not index:
            value = 0
        elif s[4] and (s[1] == 0 or s[3] in (1, 2, 3)):
            value = self.defined_value(s)
        elif s[3] in (1, 2, 3):
            raise ValueError(f'{self.name}: undefined non-default visibility symbol')
        elif s[0] in scope:
            owner, definition = scope[s[0]]
            value = owner.defined_value(definition)
        elif s[4]:
            value = self.defined_value(s)
        elif s[1] == 2:
            value = 0
            self.weak_unresolved.add(index)
        else:
            raise ValueError(f'{self.name}: unresolved strong symbol')
        self.resolved[index] = value
        return value


def symbol_scope(modules):
    scope = {}
    previous_end = 0
    total = 0
    for m in modules:
        if m.base < previous_end:
            raise ValueError('module mappings overlap')
        previous_end = m.base + ((m.nso.size + 65535) & ~65535)
        total += m.nso.size
        for s in m.symbols:
            if s[0] and s[4] and s[1] in (1, 2) and s[3] in (0, 3):
                # Runtime lookup follows load order, including weak definitions.
                scope.setdefault(s[0], (m, s))
    if not 1 <= len(modules) <= 13 or total > 256 << 20:
        raise ValueError('load plan exceeds module or memory budget')
    for m in modules:
        for i, s in enumerate(m.symbols):
            if i and not s[4]:
                m.resolve(i, scope)
    return scope


def write_plan(path, modules):
    scope = symbol_scope(modules)
    if b'nnMain' not in scope:
        raise ValueError('missing nnMain export')
    report = {}
    with path.open('w+b') as output:
        output.write(b'TKLOAD1\0' + struct.pack('<Q', len(modules)))
        for m in modules:
            n = m.nso
            init = n.dyn.get(12, 0)
            array, array_size = n.dyn.get(25, 0), n.dyn.get(27, 0)
            if (bool(25 in n.dyn) != bool(27 in n.dyn) or array_size % 8 or
                    (array_size and (array % 8 or not contains(n.data[0], n.data[1] + n.bss_size, array, array_size)))
                    or (init and not contains(*n.text, init, 4))):
                raise ValueError(f'{m.name}: invalid initialization metadata')
            entry = 0
            if b'nnMain' in scope and scope[b'nnMain'][0] is m:
                entry = m.defined_value(scope[b'nnMain'][1])
            records = sorted(r for r in n.relocs if r[1] != R_NONE)
            header = [m.base, n.size, *n.text, *n.rodata, *n.data, n.bss_size,
                      len(records), init, array, array_size, entry, 0]
            position = output.tell()
            output.write(HEADER.pack(*header))
            output.write(memoryview(n.image)[:sum(n.data)])
            expected = bytearray(n.image)
            previous = -1
            kinds = Counter()
            for offset, kind, symbol, addend in records:
                if (offset <= previous or offset % 8 or
                        not contains(n.data[0], n.data[1] + n.bss_size, offset, 8)):
                    raise ValueError(f'{m.name}: duplicate, unaligned or non-data relocation')
                if kind == R_RELATIVE:
                    if symbol:
                        raise ValueError(f'{m.name}: relative relocation has a symbol')
                    value = (m.base + addend) & MASK64
                elif kind in (R_ABS64, R_GLOB_DAT, R_JUMP_SLOT):
                    value = (m.resolve(symbol, scope) + addend) & MASK64
                else:
                    raise ValueError(f'{m.name}: unsupported relocation type')
                output.write(struct.pack('<QQ', offset, value))
                struct.pack_into('<Q', expected, offset, value)
                previous = offset
                kinds[kind] += 1
            header[-1] = zlib.crc32(expected)
            end = output.tell()
            if end > 256 << 20:
                raise ValueError('load plan exceeds file budget')
            output.seek(position)
            output.write(HEADER.pack(*header))
            output.seek(end)
            init_count = bool(init) + sum(struct.unpack_from('<Q', expected, at)[0] not in (0, MASK64)
                                         for at in range(array, array + array_size, 8))
            report[m.name] = {'base': hex(m.base), 'image_bytes': n.size, 'bss_bytes': n.bss_size,
                              'relocations': len(records), 'relr_relocations': n.relr_count,
                              'relocation_types': dict(kinds), 'unresolved_weak_symbols': len(m.weak_unresolved),
                              'init_functions': init_count, 'entry_functions': int(bool(entry)),
                              'relocated_crc32': f'{header[-1]:08x}'}
    return report


def runtime_sources():
    return {name: digest(ROOT / 'runtime' / name) for name in RUNTIME}


def verify_build(infos):
    record = json.loads((BUILD / 'result.json').read_text())
    if (record.get('runtime_sources') != runtime_sources() or record['context'] != context()
            or record['executable_sha256'] != digest(BUILD / executable('link-probe'))
            or any(record['generation'][name]['inputs'] != generation_inputs(info) for name, info in infos.items())):
        raise ValueError('linked probe is stale; rerun make build before load-only')
    return record['executable_sha256']


def native_load(plan):
    for attempt in range(1, 9):
        result = subprocess.run([str(BUILD / executable('link-probe')), '--load-only', str(plan)],
                                capture_output=True, text=True, timeout=60)
        if result.returncode == ADDRESS_UNAVAILABLE:  # A fresh process may have a different ASLR reservation.
            continue
        if result.returncode:
            raise ValueError('native load-only failed: ' + result.stderr.strip())
        return json.loads(result.stdout), attempt
    raise ValueError('fixed guest address unavailable after 8 process attempts')


def worker(exefs, output):
    infos = verify(exefs)
    executable_sha = verify_build(infos)
    modules = []
    for name, info in infos.items():
        n = Nso(exefs / name)
        if digest(exefs / name) != info['sha256']:
            raise ValueError('input changed during loading')
        modules.append(Module(name, int(info['base'], 0), n))
    # The supervisor owns this parent, so a killed worker cannot strand its images.
    with tempfile.TemporaryDirectory(prefix='load-', dir=output.parent) as temp:
        plan = Path(temp) / 'image.plan'
        planned = write_plan(plan, modules)
        native, attempts = native_load(plan)
        if (native['modules'] != len(modules) or native['game_executed'] is not False
                or any(native[key] != sum(m[key] for m in planned.values())
                       for key in ('relocations', 'init_functions', 'entry_functions'))):
            raise ValueError('native loading differs from the prepared plan')
        write_json(output, {'status': 'native_load_only_verified', 'modules': planned,
                           'native': native, 'address_space_attempts': attempts, 'plan_bytes': plan.stat().st_size,
                           'executable_sha256': executable_sha, 'runtime_sources': runtime_sources(),
                           'planner_sha256': digest(Path(__file__)), 'input_sha256': {n: i['sha256'] for n, i in infos.items()},
                           'game_executed': False, 'initialization_executed': False})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exefs', type=Path, required=True)
    parser.add_argument('--worker-result', type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    try:
        prepare_work()
        if args.worker_result:
            with (BUILD / 'build.lock').open('r') as lock:
                lock_file(lock, shared=True)
                worker(args.exefs, args.worker_result)
            return
        directory = ROOT / 'work/load'
        if directory.is_symlink():
            raise ValueError('load output must not be a symlink')
        directory.mkdir(exist_ok=True)
        if any(p.is_symlink() or p.name not in ('result.json', 'result.json.partial') for p in directory.iterdir()):
            raise ValueError('unrecognized load output')
        with tempfile.TemporaryDirectory(prefix='load-result-', dir=ROOT / 'work/tmp') as temp:
            candidate = Path(temp) / 'result.json'
            metrics = run([sys.executable, str(Path(__file__).resolve()), '--exefs', str(args.exefs.resolve()),
                           '--worker-result', str(candidate)], cwd=ROOT, log=ROOT / 'work/logs/load.log', seconds=120)
            result = json.loads(candidate.read_text())
            result['execution'] = metrics
            write_json(directory / 'result.json', result)
        print(json.dumps(result, indent=2))
    except (ValueError, KeyError, OSError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
