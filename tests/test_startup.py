from pathlib import Path
import json
import os
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
    assert(c->x[0] && c->x[1] == 0);
    c->x[1] = c->x[0]; rt_svc(c, 0x0c, 0x7200010080ull);
    assert(!c->x[0] && c->x[1] == 44);
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
                        *[str(ROOT / 'runtime' / name) for name in startup.SOURCES if name.endswith('.c')],
                        str(source), *startup.platform_flags(), '-o', str(cls.binary)], check=True, timeout=30)
        cls.kernel = cls.root / 'kernel'
        empty_guest = cls.root / 'empty_guest.c'
        empty_guest.write_text('#include "cpu.h"\n#include <stdlib.h>\n'
                               'GuestFn rt_find_function(uint64_t a) { (void)a; return NULL; }\n'
                               'void rt_dispatch(Cpu *c) { (void)c; abort(); }\n')
        subprocess.run(['clang', '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror',
                        '-I', str(ROOT / 'third_party/lasrecomp/runtime'), str(empty_guest),
                        '-I', str(ROOT / 'runtime'), str(ROOT / 'runtime/kernel.c'),
                        str(ROOT / 'runtime/threads.c'), str(ROOT / 'tests/native_kernel.c'),
                        '-o', str(cls.kernel)], check=True, timeout=30)
        cls.services = cls.root / 'services'
        subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer', '-Wall', '-Wextra', '-Werror',
                        '-I', str(ROOT / 'third_party/lasrecomp/runtime'), str(empty_guest),
                        *[str(ROOT / 'runtime' / name) for name in startup.SOURCES
                          if name.endswith('.c') and name not in ('startup.c', 'probe.c', 'load.c')],
                        str(ROOT / 'tests/native_services.c'), *startup.platform_flags(),
                        '-o', str(cls.services)], check=True, timeout=30)
        cls.threads = cls.root / 'threads'
        subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined',
                        '-DTOTK_TEST_THREADS', '-Wall', '-Wextra', '-Werror',
                        '-I', str(ROOT / 'runtime'), '-I', str(ROOT / 'third_party/lasrecomp/runtime'),
                        str(ROOT / 'runtime/kernel.c'), str(ROOT / 'runtime/threads.c'),
                        str(ROOT / 'tests/native_threads.c'), '-o', str(cls.threads)], check=True, timeout=30)

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
                self.assertEqual(json.loads(result.stdout)['completed_stages'], 1)
                self.assertFalse(json.loads(result.stdout)['entered_nnMain'])

    def test_missing_startup_export_rejected(self):
        modules = [fixture(self.root / 'main'), fixture(self.root / 'sdk', main=False)]
        with self.assertRaisesRegex(ValueError, 'missing startup export'):
            startup.entries(modules)

    def test_report_cannot_turn_an_incomplete_checkpoint_into_success(self):
        partial = {'completed_stages': 1, 'title_verified': False, 'function_calls': 4,
                   'constructors_completed': False, 'entered_nnMain': False, 'initialization_executed': True}
        self.assertEqual(startup.native_report(json.dumps(partial), 78, 'nnosInitialize'), partial)
        for value in ({}, [], partial, dict(partial, completed_stages=9), dict(partial, title_verified=True),
                      dict(partial, completed_stages=2, entered_nnMain=True)):
            with self.assertRaisesRegex(ValueError, 'invalid native startup report'):
                startup.native_report(json.dumps(value), 0, 'nnosInitialize')

    def test_window_close_is_distinct_from_a_completed_checkpoint(self):
        report = {'completed_stages': 6, 'title_verified': False, 'function_calls': 4,
                  'constructors_completed': True, 'entered_nnMain': True,
                  'initialization_executed': True, 'window_closed': True}
        self.assertEqual(startup.native_report(json.dumps(report), 0, 'nnMain', graphics=True), report)
        with self.assertRaisesRegex(ValueError, 'invalid native startup report'):
            startup.native_report(json.dumps(report), 0, 'nnMain')
        early = dict(report, completed_stages=0, constructors_completed=False, entered_nnMain=False,
                     function_calls=0, initialization_executed=False)
        self.assertEqual(startup.native_report(json.dumps(early), 0, 'nnMain', graphics=True), early)
        for value in (False, 1):
            with self.assertRaisesRegex(ValueError, 'invalid native startup report'):
                startup.native_report(json.dumps(dict(report, window_closed=value)), 0, 'nnMain', graphics=True)

    def test_native_memory_and_main_thread_contracts(self):
        for _ in range(8):
            result = subprocess.run([str(self.kernel)], capture_output=True, text=True, timeout=15)
            if result.returncode != load.ADDRESS_UNAVAILABLE:
                break
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_native_ipc_services_and_malformed_packets(self):
        (self.root / 'work/tmp').mkdir(parents=True, exist_ok=True)
        for _ in range(8):
            result = subprocess.run([str(self.services)], cwd=self.root, capture_output=True, text=True, timeout=25,
                                    env=dict(os.environ, DM_AUDIO='0', UBSAN_OPTIONS='halt_on_error=1',
                                             ASAN_OPTIONS='detect_stack_use_after_return=0'))
            if result.returncode != load.ADDRESS_UNAVAILABLE:
                break
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_native_thread_context_waits_exit_and_stop(self):
        for _ in range(8):
            result = subprocess.run([str(self.threads)], capture_output=True, text=True, timeout=25,
                                    env=dict(os.environ, UBSAN_OPTIONS='halt_on_error=1'))
            if result.returncode != load.ADDRESS_UNAVAILABLE:
                break
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
