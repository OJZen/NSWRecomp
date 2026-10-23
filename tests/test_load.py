from pathlib import Path
import mmap
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import load
from recompile import Nso

BASE = 0x7200000000


def fixture(path, *, main=True, shared=False, exports=()):
    text, ro, data = bytearray(0x1000), bytearray(0x1000), bytearray(0x1000)
    ro_at, data_at = 0x4000, 0x5000 if shared else 0x8000
    struct.pack_into('<I', text, 4, 0x40)
    struct.pack_into('<4s6i', text, 0x40, b'MOD0', ro_at - 0x40,
                     data_at + 0x1000 - 0x40, data_at + 0x2000 - 0x40, 0, 0, 0)
    symbols = [(b'', 0, 0, 0, 0, 0)]
    if main:
        symbols += [(b'external', 0x12, 0, 0, 0, 0), (b'missing', 0x20, 0, 0, 0, 0),
                    (b'absolute', 0x11, 0, 0xfff1, 0x1234, 8), (b'nnMain', 0x12, 0, 1, 0x80, 4)]
    else:
        symbols += [(b'external', 0x12, 0, 1, 0x80, 4)]
    symbols += [(name.encode(), 0x12, 0, 1, 0x80, 4) for name in exports]
    strings = bytearray()
    for i, (name, info, other, section, value, size) in enumerate(symbols):
        struct.pack_into('<IBBHQQ', ro, 0x200 + i * 24, len(strings), info, other, section, value, size)
        strings.extend(name + b'\0')
    ro[0x800:0x800 + len(strings)] = strings
    tags = [(6, ro_at + 0x200), (11, 24), (5, ro_at + 0x800), (10, len(strings)), (12, 0x80)]
    if main:
        relas = [(data_at, 1025, 1, 0), (data_at + 8, 257, 2, 32),
                 (data_at + 16, 257, 3, -4), (data_at + 64, 1027, 0, 0x80)]
        for i, (off, kind, sym, add) in enumerate(relas):
            struct.pack_into('<QQq', ro, 0xa00 + i * 24, off, (sym << 32) | kind, add)
        tags += [(7, ro_at + 0xa00), (8, len(relas) * 24), (9, 24),
                 (36, ro_at + 0xd00), (35, 16), (37, 8), (25, data_at + 64), (27, 8)]
        struct.pack_into('<QQ', ro, 0xd00, data_at + 32, 3)
        struct.pack_into('<QQ', data, 32, 0x90, 0x94)
    for i, pair in enumerate(tags):
        struct.pack_into('<QQ', ro, i * 16, *pair)
    header, payload = bytearray(0x100), bytearray()
    header[:4] = b'NSO0'
    struct.pack_into('<I', header, 0x3c, 0x1000)
    struct.pack_into('<II', header, 0x98, 0x200, len(symbols) * 24)
    for i, (offset, segment) in enumerate(((0, text), (ro_at, ro), (data_at, data))):
        struct.pack_into('<III', header, 0x10 + 16 * i, 0x100 + len(payload), offset, len(segment))
        payload.extend(segment)
    path.write_bytes(header + payload)
    return load.Module(path.name, BASE if main else BASE + 0x10000, Nso(path))


class LoadTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='native-load-test-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.native = Path(cls.temp.name) / 'native-load'
        subprocess.run(['clang', '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror',
                        *(['-municode'] if os.name == 'nt' else []),
                        '-I', str(ROOT / 'runtime'), str(ROOT / 'runtime/load.c'),
                        str(ROOT / 'tests/native_load.c'), '-o', str(cls.native)], check=True, timeout=30)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='load-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.modules = [fixture(self.root / 'main'), fixture(self.root / 'sdk', main=False)]
        self.plan = self.root / 'image.plan'

    def native_run(self, mode=0):
        for _ in range(8):
            result = subprocess.run([str(self.native), str(self.plan), str(mode)], capture_output=True, text=True, timeout=10)
            if result.returncode != load.ADDRESS_UNAVAILABLE:
                return result
        self.fail('synthetic fixed address unavailable after 8 process attempts')

    def test_native_retry_is_bounded_and_only_for_address_unavailability(self):
        unavailable = subprocess.CompletedProcess([], load.ADDRESS_UNAVAILABLE, '', 'address reservation')
        success = subprocess.CompletedProcess([], 0, '{"game_executed":false}', '')
        with patch.object(load.subprocess, 'run', side_effect=[unavailable, unavailable, success]) as run:
            self.assertEqual(load.native_load(self.plan), ({'game_executed': False}, 3))
            self.assertEqual(run.call_count, 3)
        for code in (1, 2, -9):
            invalid = subprocess.CompletedProcess([], code, '', 'invalid data or execution')
            with patch.object(load.subprocess, 'run', return_value=invalid) as run:
                with self.assertRaisesRegex(ValueError, 'invalid data or execution'):
                    load.native_load(self.plan)
                self.assertEqual(run.call_count, 1)
        with patch.object(load.subprocess, 'run', return_value=unavailable) as run:
            with self.assertRaisesRegex(ValueError, 'after 8'):
                load.native_load(self.plan)
            self.assertEqual(run.call_count, 8)

    def test_native_mapping_relocations_permissions_cleanup_and_collision(self):
        before = bytes(self.modules[0].nso.image)
        report = load.write_plan(self.plan, self.modules)
        self.assertEqual(bytes(self.modules[0].nso.image), before)
        self.assertEqual(report['main']['relr_relocations'], 2)
        self.assertEqual(report['main']['unresolved_weak_symbols'], 1)
        for mode in (0, 2, 3) * 4:
            r = self.native_run(mode)
            self.assertEqual(r.returncode, 0, f'mode={mode}: {r.stderr}')
        self.assertNotEqual(self.native_run(1).returncode, 0)
        if os.name == 'nt':
            self.assertEqual(self.native_run(4).returncode, 73, 'read-only guest text must fault on write')

    def test_shared_host_page_is_reported(self):
        self.modules[0] = fixture(self.root / 'main', shared=True)
        load.write_plan(self.plan, self.modules)
        r = self.native_run()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(int(r.stdout), int(mmap.PAGESIZE > 4096))

    def test_unicode_plan_path(self):
        self.plan = self.root / 'plan-\u6d4b\u8bd5.bin'
        load.write_plan(self.plan, self.modules)
        result = self.native_run()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_corrupt_truncated_duplicate_and_out_of_bounds_plans(self):
        load.write_plan(self.plan, self.modules)
        raw = self.plan.read_bytes()
        first_reloc = 16 + load.HEADER.size + sum(self.modules[0].nso.data)
        mutations = [(0, '<8s', b'BADPLAN!'), (8, '<Q', 5), (16 + 8, '<Q', 1 << 40),
                     (16 + 14 * 8, '<Q', 0), (first_reloc, '<Q', 0),
                     (first_reloc, '<Q', self.modules[0].nso.size),
                     (first_reloc + 16, '<Q', self.modules[0].nso.data[0])]
        for offset, fmt, value in mutations:
            with self.subTest(offset=offset, value=value):
                changed = bytearray(raw)
                struct.pack_into(fmt, changed, offset, value)
                self.plan.write_bytes(changed)
                self.assertNotEqual(self.native_run().returncode, 0)
        for changed in (raw[:10], raw[:-1], raw + b'extra'):
            self.plan.write_bytes(changed)
            self.assertNotEqual(self.native_run().returncode, 0)

    def test_missing_strong_and_unsupported_relocations_fail(self):
        with self.assertRaisesRegex(ValueError, 'unresolved strong'):
            load.write_plan(self.plan, self.modules[:1])
        original = list(self.modules[0].nso.relocs)
        for relocation in [(0x8000, 999, 0, 0), (0x8000, 257, 99, 0), (0, 1027, 0, 0),
                           (0x8001, 1027, 0, 0), (0x8000, 1027, 1, 0)]:
            with self.subTest(relocation=relocation):
                self.modules[0].nso.relocs = [relocation]
                with self.assertRaises(ValueError):
                    load.write_plan(self.plan, self.modules)
        self.modules[0].nso.relocs = original + [original[0]]
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            load.write_plan(self.plan, self.modules)

    def test_binding_visibility_absolute_and_load_order(self):
        first, second = self.modules
        # Same-name default definitions are preemptable; protected/local definitions are not.
        first.symbols = [(b'', 0, 0, 0, 0, 0, 0), (b'x', 2, 2, 0, 1, 0x80, 4)]
        second.symbols = [(b'', 0, 0, 0, 0, 0, 0), (b'x', 1, 2, 0, 1, 0x90, 4),
                          (b'x', 1, 2, 3, 1, 0x94, 4), (b'x', 0, 2, 0, 1, 0x98, 4),
                          (b'abs', 1, 1, 0, 0xfff1, 0x1234, 8)]
        scope = load.symbol_scope(self.modules)
        self.assertEqual(second.resolve(1, scope), BASE + 0x80)
        self.assertEqual(second.resolve(2, scope), second.base + 0x94)
        self.assertEqual(second.resolve(3, scope), second.base + 0x98)
        self.assertEqual(second.resolve(4, scope), 0x1234)
        first.symbols[1] = (b'x', 1, 2, 2, 1, 0x80, 4)
        second.resolved.clear()
        self.assertEqual(second.resolve(1, load.symbol_scope(self.modules)), second.base + 0x90)

    def test_invalid_bss_symbol_metadata_and_initialization_fail(self):
        for mutate in (lambda n: setattr(n, 'bss_end', n.size + 1),
                       lambda n: n.image.__setitem__(n.dyn[6] + 24 + 5, 0x80),
                       lambda n: n.dyn.__setitem__(10, 1), lambda n: n.dyn.__setitem__(22, 0)):
            n = Nso(self.root / 'main')
            mutate(n)
            with self.assertRaises(ValueError):
                load.Module('main', BASE, n)
        self.modules[0].nso.dyn[27] = 9
        with self.assertRaisesRegex(ValueError, 'initialization'):
            load.write_plan(self.plan, self.modules)

    def test_failed_worker_preserves_receipt_and_cleans_parent_owned_images(self):
        temporary = self.root / 'work/tmp'
        temporary.mkdir(parents=True)
        output = self.root / 'work/load'
        output.mkdir()
        receipt = output / 'result.json'
        receipt.write_text('{"previous": true}\n')

        def fail(command, **kwargs):
            result = Path(command[command.index('--worker-result') + 1])
            abandoned = result.parent / 'load-abandoned'
            abandoned.mkdir()
            (abandoned / 'image.plan').write_bytes(b'task-owned partial output')
            raise RuntimeError('worker interrupted')

        with patch.object(load, 'ROOT', self.root), patch.object(load, 'prepare_work'), \
                patch.object(load, 'run', side_effect=fail), \
                patch.object(sys, 'argv', ['load.py', '--exefs', str(self.root)]):
            with self.assertRaises(SystemExit):
                load.main()
        self.assertEqual(receipt.read_text(), '{"previous": true}\n')
        self.assertFalse(list(temporary.iterdir()))


if __name__ == '__main__':
    unittest.main()
