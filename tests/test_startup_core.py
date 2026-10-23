from pathlib import Path
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from test_load import BASE, fixture

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import load
import startup
from host import cpu_flags, executable
from test_npdm import fixture as npdm_fixture


class StartupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='startup-test-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.root = Path(cls.temp.name)
        cls.binary = cls.root / executable('startup')
        source = cls.root / 'synthetic.c'
        source.write_text(r'''
#include "cpu.h"
#include "kernel.h"
#include <assert.h>
#include <stdlib.h>
extern void hle_nvnBootstrapLoader(Cpu *c);
extern void hle__ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_(Cpu *);
extern void hle__ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_(Cpu *);
extern void hle__ZN2nv18InitializeGraphicsEPvm(Cpu *);
#ifndef _WIN32
#include <unistd.h>
#endif
static void first(Cpu *c) {
    assert(c->tpidr && c->tpidrro && !(c->sp & 15));
    assert(c->tpidr < (1ull << 39) && c->tpidrro < (1ull << 39));
    assert(kernel_access(c->tpidr, 8192, 3) && kernel_access(c->tpidrro, 0x200, 3));
    assert(LD64(c->tpidrro + 0x1f8));
    assert(!LD64(c->tpidr) && !c->fpcr && !c->fpsr);
    ST64(c->sp - 8, 123);
    ST64(c->tpidr, 456);
    FN_ENTER(0x7200000080ull);
    const char *mode = getenv("NSW_TEST_STARTUP");
    if (mode && !strcmp(mode, "host-api")) hle_nvnBootstrapLoader(c);
    if (mode && (!strcmp(mode, "graphics") || !strcmp(mode, "graphics-registration"))) {
        c->x[0] = c->x[1] = c->x[2] = 0x7200000080ull; c->x[3] = 0;
        hle__ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_(c);
        hle__ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_(c);
        if (!strcmp(mode, "graphics-registration")) return;
        c->x[0] = c->sp - 0x100; c->x[1] = 0x100;
        hle__ZN2nv18InitializeGraphicsEPvm(c);
        abort(); // No graphics device is available in the basic runtime.
    }
    if (mode && !strcmp(mode, "exit")) exit(78);
    if (mode && !strcmp(mode, "sm")) {
        uint64_t name = c->sp - 64;
        memcpy((void *)name, "sm:", 4);
        c->x[1] = name; rt_svc(c, 0x1f, 0x7200000080ull);
        assert(!c->x[0] && c->x[1]);
        uint64_t handle = c->x[1];
        memset((void *)c->tpidrro, 0, 0x100);
        ST32(c->tpidrro, 16); ST32(c->tpidrro + 4, 0x80000000); ST32(c->tpidrro + 8, 1);
        c->x[0] = handle; rt_svc(c, 0x21, 0x7200000080ull);
        assert(!c->x[0] && !LD32(c->tpidrro + 8));
        c->x[0] = handle; rt_svc(c, 0x16, 0x7200000080ull); assert(!c->x[0]);
    }
    if (mode && !strcmp(mode, "ipc")) rt_svc(c, 0x1f, 0x7200000080ull);
    if (mode && !strcmp(mode, "guard")) ST64(c->sp, 1);
    if (mode && !strcmp(mode, "hang")) {
#ifndef _WIN32
        alarm(0); // Exercise the Python supervisor independently of the native alarm.
#endif
        for (;;) { FN_ENTER(0x7200000080ull); }
    }
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
                        *cpu_flags(), *(['-municode'] if os.name == 'nt' else []),
                        '-DTOTK_LINK_ALL', '-DTOTK_STARTUP', '-DNSW_KERNEL_ONLY',
                        '-I', str(ROOT / 'runtime'), '-I', str(ROOT / 'third_party/lasrecomp/runtime'),
                        *[str(ROOT / 'runtime' / name) for name in startup.CORE_SOURCES if name.endswith('.c')],
                        str(source), *startup.platform_flags(kernel_only=True), '-o', str(cls.binary)], check=True, timeout=30)
        cls.basic_binary = cls.root / executable('startup-basic')
        subprocess.run(['clang', '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', *cpu_flags(),
                        *(['-municode'] if os.name == 'nt' else []),
                        '-DTOTK_LINK_ALL', '-DTOTK_STARTUP', '-DNSW_BASIC_SERVICES',
                        '-I', str(ROOT / 'runtime'), '-I', str(ROOT / 'third_party/lasrecomp/runtime'),
                        *[str(ROOT / 'runtime' / name) for name in startup.BASIC_SOURCES if name.endswith('.c')],
                        str(source), *startup.platform_flags(basic_services=True), '-o', str(cls.basic_binary)], check=True, timeout=30)
        cls.kernel = cls.root / executable('kernel')
        empty_guest = cls.root / 'empty_guest.c'
        empty_guest.write_text('#include "cpu.h"\n#include <stdlib.h>\n'
                               'GuestFn rt_find_function(uint64_t a) { (void)a; return NULL; }\n'
                               'void rt_dispatch(Cpu *c) { (void)c; abort(); }\n'
                               'void rt_udf(Cpu *c, uint64_t pc) { (void)c; (void)pc; abort(); }\n')
        subprocess.run(['clang', '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror',
                        '-I', str(ROOT / 'third_party/lasrecomp/runtime'), str(empty_guest),
                        *cpu_flags(), '-I', str(ROOT / 'runtime'), str(ROOT / 'runtime/host.c'), str(ROOT / 'runtime/kernel.c'),
                        str(ROOT / 'runtime/threads.c'), str(ROOT / 'tests/native_kernel.c'),
                        *startup.platform_flags(kernel_only=True), '-o', str(cls.kernel)], check=True, timeout=30)
        cls.threads = cls.root / executable('threads')
        subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined',
                        '-DTOTK_TEST_THREADS', '-Wall', '-Wextra', '-Werror',
                        '-I', str(ROOT / 'runtime'), '-I', str(ROOT / 'third_party/lasrecomp/runtime'),
                        *cpu_flags(), str(ROOT / 'runtime/host.c'), str(ROOT / 'runtime/kernel.c'), str(ROOT / 'runtime/threads.c'),
                        str(ROOT / 'tests/native_threads.c'), *startup.platform_flags(kernel_only=True), '-o', str(cls.threads)], check=True, timeout=30)

    def test_state_persists_and_unknown_svc_stops(self):
        modules = [fixture(self.root / 'main'), fixture(self.root / 'sdk', main=False)]
        plan = self.root / 'image.plan'
        load.write_plan(plan, modules)
        addresses = [BASE + 0x80, BASE + 0x10080] + [BASE + 0x80] * 5
        for through, expected in ((1, 0), (2, 78)):
            for _ in range(8):
                result = subprocess.run([str(self.binary), str(plan), str(through), *map(hex, addresses[:through])],
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
        modules = [fixture(self.root / 'main', exports=('_init_libc0',)), fixture(self.root / 'sdk', main=False)]
        self.assertEqual(startup.entries(modules, '_init_libc0'), [BASE + 0x80])
        with self.assertRaisesRegex(ValueError, 'missing startup export'):
            startup.entries(modules)

    def run_worker(self, mode, seconds=5, basic_services=False):
        exefs = self.root / 'exefs'
        exefs.mkdir(exist_ok=True)
        modules = [fixture(exefs / 'main', exports=('_init_libc0',)), fixture(exefs / 'sdk', main=False)]
        infos = {m.name: {'base': hex(m.base), 'sha256': startup.digest(exefs / m.name)} for m in modules}
        output, process = self.root / (mode + '.json'), self.root / 'process.json'
        metadata = exefs / 'main.npdm'; metadata.write_bytes(npdm_fixture())
        with patch.object(startup, 'verify', return_value=infos), patch.object(startup, 'PROCESS', process), \
                patch.dict(os.environ, NSW_TEST_STARTUP=mode):
            startup.worker(exefs, '_init_libc0', self.basic_binary if basic_services else self.binary, output,
                           seconds=seconds, kernel_only=not basic_services, basic_services=basic_services,
                           npdm_sha256=startup.digest(metadata) if basic_services else None)
        self.assertFalse(process.exists())
        return json.loads(output.read_text())

    def test_basic_service_svc_bridge_and_receipt(self):
        result = self.run_worker('sm', basic_services=True)
        self.assertEqual(result['status'], 'startup_checkpoint')
        self.assertEqual(result['native']['completed_stages'], 1)
        self.assertEqual(result['runtime_mode'], 'basic_services')
        self.assertFalse(result['kernel_only'])
        self.assertEqual(result['service_policy']['client_services'], ['lm', 'fsp-srv'])

    def test_missing_host_api_is_a_stopped_run(self):
        result = self.run_worker('host-api', basic_services=True)
        self.assertEqual(result['exit_code'], 78)
        self.assertEqual(result['status'], 'startup_stopped')
        self.assertEqual(result['native']['stop_reason'], 'unsupported_host_api')

    def test_basic_graphics_registration_stops_at_device_initialization(self):
        registered = self.run_worker('graphics-registration', basic_services=True)
        self.assertEqual(registered['status'], 'startup_checkpoint')
        self.assertEqual(registered['exit_code'], 0)
        self.assertEqual(registered['native']['completed_stages'], 1)
        result = self.run_worker('graphics', basic_services=True)
        self.assertEqual(result['status'], 'startup_stopped')
        self.assertEqual(result['exit_code'], 78)
        self.assertEqual(result['native']['stop_reason'], 'unsupported_host_api')
        self.assertEqual(result['native']['completed_stages'], 0)

    def test_unreported_exit_cannot_claim_a_checkpoint(self):
        result = self.run_worker('exit')
        self.assertEqual(result['exit_code'], 78)
        self.assertEqual(result['native']['stop_reason'], 'incomplete')

    def test_kernel_only_stops_at_ipc(self):
        result = self.run_worker('ipc')
        self.assertEqual(result['exit_code'], 78)
        self.assertEqual(result['status'], 'startup_stopped')
        self.assertEqual(result['native']['stop_reason'], 'unsupported_svc')
        self.assertEqual(result['native']['completed_stages'], 0)
        self.assertTrue(result['kernel_only'])

    def test_stack_guard_fault_is_not_a_checkpoint(self):
        result = self.run_worker('guard')
        self.assertNotEqual(result['exit_code'], 0)
        self.assertEqual(result['status'], 'startup_stopped')
        if os.name == 'nt':
            self.assertEqual(result['native']['stop_reason'], 'host_fault')
            self.assertEqual(result['native']['completed_stages'], 0)

    def test_worker_timeout_stops_native_execution_and_records_failure(self):
        result = self.run_worker('hang', seconds=1)
        self.assertEqual(result['status'], 'startup_stopped')
        if os.name == 'nt':
            self.assertEqual(result['exit_code'], 78)
            self.assertEqual(result['native']['stop_reason'], 'time_budget')
            self.assertGreater(result['native']['function_calls'], 0)
        else:
            self.assertEqual(result['exit_code'], 124)
            self.assertTrue(result['timed_out'])
            self.assertIsNone(result['native'])

    def test_report_cannot_turn_an_incomplete_checkpoint_into_success(self):
        partial = {'completed_stages': 1, 'title_verified': False, 'function_calls': 4,
                   'constructors_completed': False, 'entered_nnMain': False, 'initialization_executed': True}
        self.assertEqual(startup.native_report(json.dumps(partial), 78, 'nnosInitialize'), partial)
        with self.assertRaisesRegex(ValueError, 'invalid native startup report'):
            startup.native_report(json.dumps(dict(partial, stop_reason='checkpoint')), 78, 'nnosInitialize')
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

    def test_sync_diagnostics_are_bounded_and_address_filtered(self):
        for address_range, expected in (('0:0x8000000000', 512), ('0:1', 0), ('broken', 0)):
            with self.subTest(address_range=address_range):
                for _ in range(8):
                    result = subprocess.run([str(self.threads), 'sync-trace'], capture_output=True, text=True, timeout=25,
                        env=dict(os.environ, UBSAN_OPTIONS='halt_on_error=1', NSW_TRACE_SYNC_RANGE=address_range))
                    if result.returncode != load.ADDRESS_UNAVAILABLE: break
                self.assertEqual(result.returncode, 0, result.stderr)
                records = re.findall(r'^SYNC seq=(\d+) .*phase=([^ ]+)', result.stderr, re.M)
                self.assertEqual([int(sequence) for sequence, _ in records], list(range(1, expected + 1)))
                if records:
                    self.assertTrue({'wait-enter', 'wait-published', 'notify', 'reacquire-wait', 'handoff', 'wait-return'} <= {phase for _, phase in records})

    def test_native_memory_and_main_thread_contracts(self):
        for _ in range(8):
            result = subprocess.run([str(self.kernel)], capture_output=True, text=True, timeout=15)
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

    def test_external_stop_waits_for_main_guest_safe_point(self):
        for _ in range(8):
            result = subprocess.run([str(self.threads), 'external-stop'], capture_output=True, text=True, timeout=15,
                                    env=dict(os.environ, UBSAN_OPTIONS='halt_on_error=1'))
            if result.returncode != load.ADDRESS_UNAVAILABLE:
                break
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
