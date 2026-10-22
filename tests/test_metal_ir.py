from pathlib import Path
import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import prepare_metal_ir
import prepare_compute_pipelines


class MetalIRTests(unittest.TestCase):
    def test_compiler_identity_survives_remount_but_rejects_tool_changes(self):
        a = dict(version='Apple metal version example\nInstalledDir: /mount/one', metal_sha256='a', metallib_sha256='b')
        b = dict(a, version='Apple metal version example\nInstalledDir: /mount/two')
        self.assertTrue(prepare_metal_ir.same_compiler(a, b))
        self.assertFalse(prepare_metal_ir.same_compiler(a, dict(b, metal_sha256='changed')))
        self.assertFalse(prepare_metal_ir.same_compiler(a, dict(b, version='Apple metal version newer')))
        self.assertFalse(prepare_metal_ir.same_compiler(None, b))

    def test_cache_identity_integrity_and_source_fallback_without_gpu(self):
        with tempfile.TemporaryDirectory(prefix='metal-ir-test-') as temp:
            binary = Path(temp) / 'check'
            subprocess.run(['clang', '-std=gnu11', '-O2', '-fobjc-arc', '-Wno-deprecated-declarations',
                            '-I', str(ROOT / 'runtime/graphics/include'), '-framework', 'Foundation', '-framework', 'Metal',
                            str(ROOT / 'tests/native_shader_ir.m'), str(ROOT / 'runtime/graphics/shader_prep.m'),
                            '-o', str(binary)], check=True, capture_output=True, timeout=30)
            result = subprocess.run([str(binary), temp], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('hits=1', result.stderr)
            self.assertIn('rejected=3', result.stderr)
            self.assertIn('[compute-binary] synthetic-cache hits=1 misses=2 rejected=3', result.stderr)

    def test_invalid_compute_requests_cannot_launch_offline_compiler(self):
        with tempfile.TemporaryDirectory(prefix='compute-request-') as temp:
            root = Path(temp); directory = root / 'work/shaders'; directory.mkdir(parents=True)
            with patch.object(prepare_compute_pipelines, 'ROOT', root), patch.object(prepare_compute_pipelines, 'metal_tools') as compiler:
                for keys in ([], ['../escape'], ['x'] * 2, [None], [str(i) for i in range(33)]):
                    with self.assertRaises(ValueError): prepare_compute_pipelines.prepare(directory, keys, root / 'work/result.json')
                with self.assertRaises(ValueError): prepare_compute_pipelines.prepare(root, ['x'], root / 'work/result.json')
                (directory / 'x.metallib').write_text('synthetic')
                (directory / 'x.json').write_text('{"stage":"vs", "entry":"main0"}')
                with self.assertRaisesRegex(ValueError, 'compute reflection'):
                    prepare_compute_pipelines.prepare(directory, ['x'], root / 'work/result.json')
                compiler.assert_not_called()

    def test_compute_batch_deduplicates_binaries_and_reuses_verified_cache(self):
        with tempfile.TemporaryDirectory(prefix='compute-batch-') as temp:
            root = Path(temp); directory = root / 'work/shaders'
            (directory / 'ir-v1').mkdir(parents=True); (root / 'work/tmp').mkdir()
            translator = root / 'metal-tt'; translator.write_text('synthetic compiler')
            names = ['one', 'alias', 'two']; identities = []
            for name, number in zip(names, [1, 1, 2]):
                (directory / (name + '.metallib')).write_text('synthetic MSL')
                (directory / (name + '.json')).write_text('{"stage":"cs", "entry":"main0"}')
                key = str(number) * 64; ir = directory / 'ir-v1' / (key + '.metallib'); ir.write_bytes(b'IR')
                ir.with_suffix('.json').write_text(json.dumps(dict(library_sha256=hashlib.sha256(b'IR').hexdigest(), os='27.0')))
                identities.append(dict(key=key, ir_key=key, function='main0', architecture='synthetic'))
            invocations = []
            def execute(command, **kwargs):
                if command[0] == str(translator):
                    plan = json.loads(Path(command[1]).read_text()); invocations.append(plan)
                    self.assertEqual(len(plan['pipelines']['compute_pipelines']), 2)
                    Path(command[-1]).write_bytes(b'synthetic binary')
                    return subprocess.CompletedProcess(command, 0, '', '')
                self.assertEqual(command[1], 'compute-verify')
                return subprocess.CompletedProcess(command, 0, json.dumps([dict(strict_archive_hit=True)] * 3), '')
            with patch.object(prepare_compute_pipelines, 'ROOT', root), \
                    patch.object(prepare_compute_pipelines, 'metal_tools', return_value=(root / 'metal', root / 'metallib', dict(version='synthetic'))), \
                    patch.object(prepare_compute_pipelines, 'build_helper', return_value=root / 'helper'), \
                    patch.object(prepare_compute_pipelines.subprocess, 'check_output', return_value=json.dumps(identities)), \
                    patch.object(prepare_compute_pipelines.subprocess, 'run', side_effect=execute):
                first = prepare_compute_pipelines.prepare(directory, names, root / 'work/first.json')
                second = prepare_compute_pipelines.prepare(directory, names, root / 'work/second.json')
            self.assertEqual(len(invocations), 1)
            self.assertEqual(first['strict_binary_hits'], 3)
            self.assertEqual(second['compiler_invocations'], 0)
            self.assertTrue(all(row['cached'] for row in second['results']))
            binaries = list((directory / 'compute-v1').glob('*.metallib'))
            self.assertEqual(len(binaries), 2)
            self.assertEqual(binaries[0].stat().st_ino, binaries[1].stat().st_ino)

    def test_invalid_requests_cannot_invoke_compiler_or_escape_work(self):
        with tempfile.TemporaryDirectory(prefix='metal-ir-request-') as temp:
            root = Path(temp); directory = root / 'work/shaders'; directory.mkdir(parents=True)
            with patch.object(prepare_metal_ir, 'ROOT', root), patch.object(prepare_metal_ir, 'metal_tools') as compiler:
                for keys in ([], ['../escape'], ['x'] * 2, [None], [str(i) for i in range(257)]):
                    with self.assertRaises(ValueError): prepare_metal_ir.prepare(directory, keys, 4, root / 'work/result.json')
                with self.assertRaises(ValueError): prepare_metal_ir.prepare(root, ['x'], 4, root / 'work/result.json')
                with self.assertRaises(ValueError): prepare_metal_ir.prepare(directory, ['x'], 9, root / 'work/result.json')
                compiler.assert_not_called()
