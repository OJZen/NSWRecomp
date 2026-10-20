from pathlib import Path
import json
import subprocess
import sys
import tempfile
import unittest

from test_load import BASE, fixture

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import load
import startup


class StartupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='startup-test-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.root = Path(cls.temp.name)
        cls.binary = cls.root / 'startup'
        source = cls.root / 'synthetic.c'
        source.write_text(r'''
#include "cpu.h"
#include <assert.h>
static void first(Cpu *c) {
    assert(c->tpidr && c->tpidrro && !(c->sp & 15));
    assert(LD64(c->tpidrro + 0x1f8));
    assert(!LD64(c->tpidr) && !c->fpcr && !c->fpsr);
    ST64(c->sp - 8, 123);
    ST64(c->tpidr, 456);
    FN_ENTER(0x7200000080ull);
}
static void second(Cpu *c) {
    assert(LD64(c->sp - 8) == 123 && LD64(c->tpidr) == 456);
    assert(c->x[0] == 0x8001 && c->x[1] == 0);
    rt_svc(c, 0x7f, 0x7200010080ull);
}
const uint64_t rtld_fn_addrs[] = {0x7200000080ull, 0x7200000090ull, 0x7200000094ull};
const GuestFn rtld_fn_ptrs[] = {first, first, first};
const uint64_t rtld_fn_count = 3;
const uint64_t main_fn_addrs[] = {0x7200010080ull};
const GuestFn main_fn_ptrs[] = {second};
const uint64_t main_fn_count = 1;
const uint64_t subsdk0_fn_addrs[] = {0x7200020080ull};
const GuestFn subsdk0_fn_ptrs[] = {first};
const uint64_t subsdk0_fn_count = 1;
const uint64_t sdk_fn_addrs[] = {0x7200030080ull};
const GuestFn sdk_fn_ptrs[] = {first};
const uint64_t sdk_fn_count = 1;
''')
        subprocess.run(['clang', '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror',
                        '-DTOTK_LINK_ALL', '-DTOTK_STARTUP', '-I', str(ROOT / 'third_party/lasrecomp/runtime'),
                        str(ROOT / 'runtime/probe.c'), str(ROOT / 'runtime/load.c'),
                        str(ROOT / 'runtime/startup.c'), str(ROOT / 'runtime/kernel.c'),
                        str(ROOT / 'runtime/threads.c'), str(source), '-o', str(cls.binary)], check=True, timeout=30)
        cls.kernel = cls.root / 'kernel'
        subprocess.run(['clang', '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror',
                        '-I', str(ROOT / 'runtime'), str(ROOT / 'runtime/kernel.c'),
                        str(ROOT / 'runtime/threads.c'), str(ROOT / 'tests/native_kernel.c'),
                        '-o', str(cls.kernel)], check=True, timeout=30)

    def test_state_persists_and_unknown_svc_stops(self):
        modules = [fixture(self.root / 'main'), fixture(self.root / 'sdk', main=False)]
        plan = self.root / 'image.plan'
        load.write_plan(plan, modules)
        addresses = [BASE + 0x80, BASE + 0x10080] + [BASE + 0x80] * 5
        for through, expected in ((1, 0), (2, 78)):
            for _ in range(8):
                result = subprocess.run([str(self.binary), str(plan), str(through), *map(hex, addresses)],
                                        capture_output=True, text=True, timeout=10)
                if result.returncode != load.ADDRESS_UNAVAILABLE:
                    break
            self.assertEqual(result.returncode, expected, result.stderr)
            if not expected:
                self.assertEqual(json.loads(result.stdout)['completed_stages'], 1)
                self.assertFalse(json.loads(result.stdout)['title_verified'])
            else:
                self.assertIn('unsupported svc=7f stage=nnosInitialize', result.stderr)

    def test_missing_startup_export_rejected(self):
        modules = [fixture(self.root / 'main'), fixture(self.root / 'sdk', main=False)]
        with self.assertRaisesRegex(ValueError, 'missing startup export'):
            startup.entries(modules)

    def test_native_memory_and_main_thread_contracts(self):
        for _ in range(8):
            result = subprocess.run([str(self.kernel)], capture_output=True, text=True, timeout=15)
            if result.returncode != load.ADDRESS_UNAVAILABLE:
                break
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
