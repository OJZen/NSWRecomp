"""Host process, lock and compiler contracts; no game inputs."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from bounded import run
from host import executable, lock_file, response_args, response_command
import recompile


class HostTests(unittest.TestCase):
    def test_lock_is_nonblocking_and_released_on_close(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / 'build.lock'
            command = [sys.executable, '-c',
                       'import sys; from host import lock_file; '
                       'f=open(sys.argv[1], "a"); lock_file(f)', str(path)]
            env = dict(os.environ, PYTHONPATH=str(ROOT / 'tools'))
            with path.open('a') as stream:
                lock_file(stream)
                result = subprocess.run(command, env=env, capture_output=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
            subprocess.run(command, env=env, check=True, capture_output=True, timeout=10)

    def test_response_file_with_spaces_links_and_runs(self):
        with tempfile.TemporaryDirectory(prefix='host paths with spaces ') as td:
            root = Path(td)
            source = root / 'input file.c'
            source.write_text('int main(void) { return 23; }\n')
            binary = root / executable('output file')
            response = root / 'compile args.rsp'
            response.write_text(response_args([str(source), '-o', str(binary)]), encoding='utf-8')
            subprocess.run(response_command(response), check=True, capture_output=True, timeout=30)
            self.assertEqual(subprocess.run([str(binary)], timeout=10).returncode, 23)

    def test_local_profile_does_not_inherit_default_hooks(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / 'profile.json'
            path.write_text(json.dumps({'version': 'synthetic', 'modules': {'main': {}}}))
            with patch.object(recompile, 'PROFILE', path):
                self.assertIsNone(recompile.hooks())
                path.write_text(json.dumps({'version': 'synthetic', 'modules': {'../unsafe': {}}}))
                with self.assertRaisesRegex(ValueError, 'module names'):
                    recompile.profile()

    def test_child_exit_code_and_temporary_volume(self):
        with tempfile.TemporaryDirectory(prefix='bounded paths ') as td:
            root = Path(td)
            log = root / 'child.log'
            run([sys.executable, '-c', 'import os,json; print(json.dumps([os.environ[k] for k in ("TMP", "TEMP", "TMPDIR")]))'],
                cwd=root, log=log, seconds=10, reserve=0)
            self.assertEqual(json.loads(log.read_text()), [str(root / 'work/tmp')] * 3)
            with self.assertRaisesRegex(RuntimeError, 'exit code 23'):
                run([sys.executable, '-c', 'raise SystemExit(23)'], cwd=root, log=log, reserve=0)

    @unittest.skipUnless(os.name == 'nt', 'Windows Job Object integration')
    def test_job_reaps_descendant_after_root_exit_and_timeout(self):
        from windows_job import open_process, exit_code, close, W, C
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            for timeout in (False, True):
                pidfile = root / 'pid.json'
                script = ('import subprocess,sys,pathlib,time; '
                          'p=subprocess.Popen([sys.executable,"-c","import time; time.sleep(60)"]); '
                          'pathlib.Path(sys.argv[1]).write_text(str(p.pid)); '
                          + ('time.sleep(60)' if timeout else 'time.sleep(.1)'))
                args = dict(cwd=root, log=root / 'tree.log', seconds=1 if timeout else 10, reserve=0)
                if timeout:
                    with self.assertRaisesRegex(RuntimeError, 'time budget'):
                        run([sys.executable, '-c', script, str(pidfile)], **args)
                else:
                    run([sys.executable, '-c', script, str(pidfile)], **args)
                handle = open_process(0x1000 | 0x100000, False, int(pidfile.read_text()))
                if handle:
                    try:
                        from windows_job import api
                        wait = api('WaitForSingleObject', W.DWORD, W.HANDLE, W.DWORD)
                        self.assertEqual(wait(handle, 5000), 0)
                        code = W.DWORD()
                        self.assertTrue(exit_code(handle, C.byref(code)))
                        self.assertNotEqual(code.value, 259)
                    finally:
                        close(handle)

    @unittest.skipUnless(os.name == 'nt', 'Windows aggregate working-set accounting')
    def test_job_counts_descendant_memory(self):
        child = 'import time; a=bytearray(96<<20); time.sleep(20)'
        parent = 'import subprocess,sys,time; subprocess.Popen([sys.executable,"-c",sys.argv[1]]); time.sleep(20)'
        with tempfile.TemporaryDirectory() as td:
            with self.assertRaisesRegex(RuntimeError, 'RSS'):
                run([sys.executable, '-c', parent, child], cwd=Path(td), log=Path(td) / 'memory.log',
                    seconds=10, rss_limit=80 << 20, reserve=0)
