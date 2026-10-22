import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import prepare_shader_batch as batch


class ShaderBatchTests(unittest.TestCase):
    def test_linked_inputs_are_copied_and_single_batch_restores_compiler_globals(self):
        with tempfile.TemporaryDirectory(prefix='shader-batch-') as temp:
            root = Path(temp); (root / 'work/tmp').mkdir(parents=True)
            catalog = root / 'catalog'; (catalog / 'code').mkdir(parents=True); (catalog / 'variants').mkdir()
            vertex, fragment = 'a' * 16, 'b' * 16
            linked = hashlib.sha256(f'{vertex}-00000000-{fragment}'.encode()).hexdigest()[:16]
            records = []
            for key, stage in ((vertex, 'vs'), (fragment, 'ps')):
                code = key.encode(); (catalog / 'code' / (key + '.bin')).write_bytes(code)
                records.append(dict(key=key, stage=stage, source='synthetic.nvn', code_sha256=hashlib.sha256(code).hexdigest()))
            (catalog / 'index.json').write_text(json.dumps(records))
            (catalog / 'variants' / (linked + '.json')).write_text(json.dumps(dict(base=vertex, mask=0, next=fragment)))
            previous = (batch.build.ROOT, batch.translate.ROOT, batch.runtime.SHADERS)
            def compile_selected(keys):
                self.assertEqual(keys, [linked])
                directory = batch.runtime.SHADERS
                self.assertEqual({p.stem for p in (directory / 'code').iterdir()}, {vertex, fragment})
                self.assertEqual(batch.build.shader_entries(directory, keys)[0]['next']['key'], fragment)
                target = directory / 'b-synthetic'; target.mkdir()
                (target / (linked + '_b.metallib')).write_text('synthetic source')
                (target / (linked + '_b.json')).write_text('{}')
                return False
            with patch.object(batch, 'ROOT', root), patch.object(batch.build, 'chain', return_value='synthetic'), \
                    patch.object(batch.runtime, 'compile_pending', side_effect=compile_selected) as compiler:
                report = batch.prepare(catalog, root / 'work/prepared', [linked])
                compiler.assert_called_once()
                self.assertTrue(report['source_translation_completed'])
                self.assertFalse(report['metal_ir_compiled'])
                self.assertEqual(report['base_programs'], 2)
                with self.assertRaisesRegex(ValueError, 'exists'): batch.prepare(catalog, root / 'work/prepared', [linked])
                with patch.object(batch.runtime, 'compile_pending', return_value=True):
                    blocked = batch.prepare(catalog, root / 'work/blocked', [linked])
                    self.assertFalse(blocked['source_translation_completed'])
                    self.assertEqual(blocked['artifacts'], {})
                with patch.object(batch.runtime, 'compile_pending', side_effect=RuntimeError('compiler stopped')):
                    with self.assertRaisesRegex(RuntimeError, 'compiler stopped'):
                        batch.prepare(catalog, root / 'work/exception', [linked])
                    self.assertFalse((root / 'work/exception').exists())
                (catalog / 'code' / (vertex + '.bin')).write_bytes(b'corrupted')
                with self.assertRaisesRegex(ValueError, 'fingerprint'): batch.prepare(catalog, root / 'work/invalid', [linked])
            self.assertEqual((batch.build.ROOT, batch.translate.ROOT, batch.runtime.SHADERS), previous)
            self.assertEqual(list((root / 'work/tmp').iterdir()), [])

    def test_empty_duplicate_and_excessive_requests_fail_before_compiling(self):
        for keys in ([], [{}], ['a' * 16] * 2, [f'{i:016x}' for i in range(257)]):
            with self.assertRaises(ValueError): batch.prepare(Path('unused'), Path('unused'), keys)
