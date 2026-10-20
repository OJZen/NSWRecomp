import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import build_shaders
import translate_shaders
from index_shaders import archive


class ShaderPipelineTests(unittest.TestCase):
    def test_cache_invalidation_does_not_follow_symlinks(self):
        with tempfile.TemporaryDirectory(prefix='shader-cache-') as temp:
            root = Path(temp); outside = root / 'preserved'; outside.mkdir()
            marker = outside / ('1' * 16 + '.spv'); marker.write_bytes(b'preserve')
            shader_root = root / 'work/shaders'; shader_root.mkdir(parents=True)
            (shader_root / 'spv').symlink_to(outside, target_is_directory=True)
            with patch.object(translate_shaders, 'ROOT', root), self.assertRaisesRegex(ValueError, 'symlink'):
                translate_shaders.translate()
            self.assertEqual(marker.read_bytes(), b'preserve')

    def test_requested_snapshot_ignores_unrequested_incomplete_capture(self):
        with tempfile.TemporaryDirectory(prefix='shader-requests-') as temp:
            root = Path(temp); (root / 'variants').mkdir()
            vertex, pixel, missing = 'a' * 16, 'b' * 16, 'c' * 16
            pair = hashlib.sha256(f'{vertex}-00000000-{pixel}'.encode()).hexdigest()[:16]
            (root / 'index.json').write_text(json.dumps([{'key': vertex, 'stage': 'vs'}, {'key': pixel, 'stage': 'ps'}]))
            for key, request in ((pair, {'base': vertex, 'mask': 0, 'next': pixel}),
                                 (missing, {'base': 'd' * 16, 'mask': 0, 'next': 'e' * 16})):
                (root / 'variants' / (key + '.json')).write_text(json.dumps(request))
            self.assertEqual([e['key'] for e in build_shaders.shader_entries(root, [vertex])], [vertex])
            linked = build_shaders.shader_entries(root, [pair])
            self.assertEqual(linked[0]['next']['key'], pixel)
            for requested in (None, [missing], ['f' * 16], ['../outside'], []):
                with self.assertRaises(ValueError): build_shaders.shader_entries(root, requested)

    def test_translator_change_invalidates_unrequested_spirv(self):
        with tempfile.TemporaryDirectory(prefix='shader-version-') as temp:
            root = Path(temp); output = root / 'work/shaders/spv'; output.mkdir(parents=True)
            old, new = '1' * 16, '2' * 16
            (output / 'translator.sha256').write_text('previous translator')
            (output / (old + '.spv')).write_bytes(b'\x03\x02\x23\x07' + bytes(16))
            (output / (old + '.res.json')).write_text(json.dumps({'key': old, 'stage': 'cs'}))
            def translate(command, **kwargs):
                for job in json.loads(Path(command[-2]).read_text()):
                    (output / (job['key'] + '.spv')).write_bytes(b'\x03\x02\x23\x07' + bytes(16))
                    (output / (job['key'] + '.res.json')).write_text(json.dumps({'key': job['key'], 'stage': job['stage']}))
                return type('Result', (), {'returncode': 0})()
            with patch.multiple(translate_shaders, ROOT=root, TOOLS=root, XLATE=root), \
                    patch.object(translate_shaders, 'shader_entries') as entries, \
                    patch.object(translate_shaders.subprocess, 'run', side_effect=translate):
                entries.return_value = [{'key': new, 'stage': 'cs'}]
                translate_shaders.translate()
                self.assertFalse((output / (old + '.spv')).exists())
                self.assertFalse((output / (old + '.res.json')).exists())
                entries.return_value = [{'key': old, 'stage': 'cs'}]
                translate_shaders.translate()
                for key in (old, new): self.assertTrue(translate_shaders.valid({'key': key, 'stage': 'cs'}, output))

    def test_index_cannot_escape_shader_cache(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for value in ([{'key': '../outside', 'stage': 'cs'}], [{'key': '0' * 16, 'stage': 'unknown'}],
                          [{'key': '0' * 16, 'stage': 'cs'}] * 2, {}):
                (root / 'index.json').write_text(json.dumps(value))
                with self.assertRaises(ValueError): build_shaders.shader_entries(root)

    def test_synthetic_capture_identity_and_range_checks(self):
        control = bytearray(0x758)
        struct.pack_into('<3I', control, 0, 0x98761234, 1, 16)
        struct.pack_into('<I', control, 0x20, len(control))
        code = bytearray(0x120)
        struct.pack_into('<I', code, 0, 0x12345679)
        struct.pack_into('<8I', control, 0x6f8, 0x10, 0x10, 0x110, len(code), 0x100, 0, 0, 5)
        struct.pack_into('<4I', control, 0x748, 1, 2, 3, 0)
        record, program = list(archive(control + code))[0]
        self.assertEqual(record['stage'], 'cs')
        self.assertEqual(record['local'], [1, 2, 3])
        self.assertEqual(record['key'], hashlib.sha256(control + code[0x100:]).hexdigest()[:16])
        self.assertEqual(program, bytes(16))
        for bad in (control + code[:-1], control[:50], b'invalid'):
            with self.assertRaises(ValueError): list(archive(bad))
        struct.pack_into('<I', control, 0x748, 1025)
        with self.assertRaisesRegex(ValueError, 'workgroup'): list(archive(control + code))

    def test_toolchain_identity_invalidates_on_each_changed_input(self):
        with tempfile.TemporaryDirectory(prefix='shader-chain-') as temp:
            root = Path(temp)
            files = [root / 'tools' / (n + '.py') for n in ('build_shaders', 'index_shaders', 'translate_shaders', 'spirv_subgroup')]
            files += [root / 'gfx/tools/dmsh_build.py', root / 'spirv-cross']
            files += [root / (n + '.dll') for n in ('dmsh_xlate', 'Ryujinx.Graphics.Shader', 'Ryujinx.Common', 'Spv.Generator')]
            for file in files: file.parent.mkdir(parents=True, exist_ok=True); file.write_bytes(b'synthetic tool')
            with patch.multiple(build_shaders, ROOT=root, CORE=root, TOOLS=root, XLATE=root, __file__=str(files[0])):
                expected = build_shaders.chain()
                self.assertRegex(expected, r'^[0-9a-f]{12}$')
                for file in files:
                    file.write_bytes(b'changed'); self.assertNotEqual(build_shaders.chain(), expected); file.write_bytes(b'synthetic tool')
                files[-1].unlink(); self.assertEqual(build_shaders.chain(), 'unprepared')
