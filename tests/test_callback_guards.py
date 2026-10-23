"""Explicit adapter bindings and forced completion-before-wait ordering."""
from pathlib import Path
import copy
import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import callback_guards
from host import cpu_flags, executable
from load import Module
from recompile import Nso, digest
from test_load import fixture


class CallbackGuardTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='callback-guard-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        main = fixture(self.root / 'main')
        sdk = self.root / 'sdk'
        fixture(sdk, main=False, exports=('synthetic_lock', 'synthetic_unlock'))
        raw = bytearray(sdk.read_bytes())
        for i, value in ((2, 0x90), (3, 0xa0)):
            struct.pack_into('<Q', raw, 0x1100 + 0x200 + i * 24 + 8, value)
        sdk.write_bytes(raw)
        self.modules = [main, Module('sdk', 0x7200010000, Nso(sdk))]
        self.fingerprints = {m.name: digest(self.root / m.name) for m in self.modules}
        self.config = dict(schema=1, guards=[dict(module='main', module_sha256=self.fingerprints['main'],
            function_offset='0x80', function_size=4, function_sha256=hashlib.sha256(main.nso.image[0x80:0x84]).hexdigest(),
            context_pointer_offset=8, mutex_offset=16, mutex_size=64,
            lock_symbol='synthetic_lock', unlock_symbol='synthetic_unlock')])
        self.path = self.root / 'guards.json'

    def generate(self, config):
        self.path.write_text(json.dumps(config))
        return callback_guards.generate(self.path, self.modules, self.fingerprints)

    def test_mismatched_code_helpers_and_layout_are_rejected(self):
        for field, value in (('module_sha256', '0'*64), ('function_sha256', '0'*64), ('function_offset', '0x4000'),
                             ('function_size', 3), ('lock_symbol', 'unknown'), ('unlock_symbol', 'synthetic_lock'),
                             ('context_pointer_offset', 3), ('mutex_offset', -4), ('mutex_size', 0), ('mutex_size', True)):
            with self.subTest(field=field, value=value):
                config = copy.deepcopy(self.config); config['guards'][0][field] = value
                with self.assertRaises(ValueError): self.generate(config)
        config = copy.deepcopy(self.config); config['guards'] *= 2
        with self.assertRaisesRegex(ValueError, 'duplicate'): self.generate(config)
        for config in ({}, dict(schema=2, guards=[]), dict(schema=1, guards=[])):
            with self.assertRaises(ValueError): self.generate(config)

    @unittest.skipUnless(os.name == 'nt', 'LLVM MinGW link wrapping')
    def test_linked_mutex_guard_orders_the_delayed_waiter(self):
        source, flags, receipt = self.generate(self.config)
        self.assertEqual(receipt['config_sha256'], digest(self.path))
        self.assertEqual(receipt['source_sha256'], hashlib.sha256(source.encode()).hexdigest())
        wrapper = self.root / 'binding.c'; wrapper.write_text(source)
        original = self.root / 'original.o'; binary = self.root / executable('guard')
        common = ['clang', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                  '-fsanitize=address,undefined', '-pthread', *cpu_flags(),
                  '-I', str(ROOT / 'runtime'), '-I', str(ROOT / 'third_party/lasrecomp/runtime')]
        fixture_source = ROOT / 'tests/native_callback_guard.c'
        subprocess.run([*common, '-DGUARD_ORIGINAL', '-c', str(fixture_source), '-o', str(original)],
                       check=True, capture_output=True, timeout=30)
        result = subprocess.run([*common, str(fixture_source), str(original), str(wrapper),
            str(ROOT / 'runtime/callback_guard.c'), str(ROOT / 'runtime/guest_callback.c'), *flags, '-o', str(binary)],
            capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        for mode in ('direct', 'table', 'bad-context', 'bad-mutex', 'missing-helper'):
            with self.subTest(mode=mode):
                result = subprocess.run([str(binary), mode], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0 if mode in ('direct', 'table') else 78, result.stderr)
                self.assertNotIn('runtime error:', result.stderr)
                self.assertNotIn('ERROR: AddressSanitizer', result.stderr)
                if result.returncode: self.assertIn('adapter_guard_error:', result.stderr)
        # Negative control: the same callback without the link binding reaches
        # completion before the test permits its waiter to publish the wait.
        identity = self.root / 'identity.c'
        identity.write_text(f'const char callback_guards_sha256[] = "{receipt["config_sha256"]}";\n')
        unguarded = self.root / executable('unguarded')
        subprocess.run([*common, str(fixture_source), str(original), str(identity),
                        str(ROOT / 'runtime/callback_guard.c'), str(ROOT / 'runtime/guest_callback.c'), '-o', str(unguarded)],
                       check=True, capture_output=True, timeout=30)
        result = subprocess.run([str(unguarded), 'table'], capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('waiting', result.stderr)
