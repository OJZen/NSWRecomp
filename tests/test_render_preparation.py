from pathlib import Path
import copy
import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import prepare_render_pipelines as render


class RenderPreparationTests(unittest.TestCase):
    def test_exported_libraries_bind_only_by_verified_content_hash(self):
        with tempfile.TemporaryDirectory(prefix='render-script-') as temp:
            directory = Path(temp); ir = directory / 'ir-v1'; ir.mkdir()
            path = ir / ('1' * 64 + '.metallib'); path.write_bytes(b'synthetic IR')
            hashed = hashlib.sha256(b'synthetic IR').hexdigest()
            path.with_suffix('.json').write_text(json.dumps(dict(format='totk-metal-ir-v1', library_sha256=hashed, os='27.0.1')))
            script = dict(libraries=dict(paths=[dict(label=hashed.upper(), path='/must/not/be/read')]),
                          pipelines=dict(render_pipelines=[dict(vertex_function='alias:'+hashed.upper()+'#v')]), functions={})
            bound = copy.deepcopy(script); inputs, version = render.bind_libraries(bound, directory, 1)
            self.assertEqual(bound['libraries']['paths'][0]['path'], str(path))
            self.assertEqual(inputs, {str(path):hashed}); self.assertEqual(version, '27.0.1')
            path.write_bytes(b'changed')
            with self.assertRaisesRegex(ValueError, 'hash mismatch'): render.bind_libraries(copy.deepcopy(script), directory, 1)
            with self.assertRaises(ValueError): render.bind_libraries(copy.deepcopy(script), directory, 0)
            script['pipelines']['compute_pipelines'] = [{}]
            with self.assertRaises(ValueError): render.bind_libraries(script, directory, 1)

    def test_invalid_capture_cannot_invoke_compiler_or_escape_work(self):
        with tempfile.TemporaryDirectory(prefix='render-request-') as temp:
            root = Path(temp); capture = root / 'work/capture'; capture.mkdir(parents=True)
            with patch.object(render, 'ROOT', root), patch.object(render, 'metal_tools') as compiler:
                with self.assertRaises(ValueError): render.prepare(capture, root, root / 'work/output')
                (capture / 'capture.json').write_text('{"format":"totk-render-capture-v1","keys":[]}')
                with self.assertRaises(ValueError): render.prepare(capture, root / 'work/b-test', root / 'work/output')
                compiler.assert_not_called()

    def test_strict_cache_identity_integrity_misses_and_descriptor_preservation(self):
        with tempfile.TemporaryDirectory(prefix='render-cache-') as temp:
            base = Path(temp); binary = base / 'check'
            result = subprocess.run(['clang', '-std=gnu11', '-O2', '-fobjc-arc', '-Wno-deprecated-declarations',
                '-I', str(ROOT / 'runtime/graphics/include'), '-framework', 'Foundation', '-framework', 'Metal',
                str(ROOT / 'tests/native_render_cache.m'), str(ROOT / 'runtime/graphics/shader_prep.m'),
                str(ROOT / 'runtime/graphics/render_prep.m'), '-o', str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            for mode in ('valid', 'stale', 'corrupt', 'duplicate', 'symlink', 'driver-miss'):
                directory = base / mode; directory.mkdir()
                result = subprocess.run([str(binary), str(directory), mode], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('[render-binary]', result.stderr)
