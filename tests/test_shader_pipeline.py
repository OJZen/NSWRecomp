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
from index_shaders import archive


class ShaderPipelineTests(unittest.TestCase):
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
