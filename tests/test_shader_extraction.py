from compression.zstd import compress
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import extract_shaders as shaders


def bnsh():
    data = bytearray(0xa78)
    data[:8] = b'BNSH\0\0\0\0'
    struct.pack_into('<H', data, 12, 0xfeff)
    struct.pack_into('<I', data, 28, len(data))
    data[0x60:0x64] = b'grsc'
    struct.pack_into('<IQ', data, 0x7c, 1, 0x100)
    struct.pack_into('<Q', data, 0x110, 0x180)
    struct.pack_into('<Q', data, 0x180 + 8 + 5 * 8, 0x1c0)
    struct.pack_into('<QQII', data, 0x1c8, 0x200, 0x958, 0x120, 0x758)
    struct.pack_into('<3I', data, 0x200, 0x98761234, 1, 16)
    struct.pack_into('<I', data, 0x220, 0x758)
    struct.pack_into('<8I', data, 0x8f8, 0x10, 0x10, 0x110, 0x120, 0x100, 0, 0, 5)
    struct.pack_into('<4I', data, 0x948, 1, 2, 3, 0)
    struct.pack_into('<I', data, 0x958, 0x12345679)
    return bytes(data)


def sarc(name, content):
    names = name.encode() + b'\0'
    names += bytes(-len(names) % 4)
    payload = 56 + len(names)
    return (struct.pack('<4sHHIIHH', b'SARC', 20, 0xfeff, payload + len(content), payload, 0x100, 0) +
            struct.pack('<4sHHI4I4sHH', b'SFAT', 12, 1, 0x65, 0, 0x01000000, 0, len(content), b'SFNT', 8, 0) +
            names + content)


def romfs(content, directory=b'Shader', name=b'Application.sarc.zs'):
    directories = struct.pack('<6I', 0, 0, 24, 0, 0, 0)
    directories += struct.pack('<6I', 0, 0, 0, 0, 0, len(directory)) + directory
    directories += bytes(-len(directories) % 4)
    files = struct.pack('<IIQQII', 24, 0, 0, len(content), 0, len(name)) + name
    files += bytes(-len(files) % 4)
    data = 80 + len(directories) + len(files)
    return struct.pack('<10Q', 80, 80, 0, 80, len(directories), 80, 0, 80 + len(directories), len(files), data) + directories + files + content


class ShaderExtractionTests(unittest.TestCase):
    def test_ui_layout_archive_preserves_shader_member(self):
        with tempfile.TemporaryDirectory(prefix='shader-ui-') as temp:
            root = Path(temp); (root / 'work/tmp').mkdir(parents=True)
            image = root / 'romfs.bin'; image.write_bytes(romfs(compress(sarc('bgsh/test.bnsh', bnsh()))))
            with image.open('rb') as stream: _, offset, length = next(shaders.romfs_entries(stream))
            with patch.object(shaders, 'ROOT', root), patch.object(shaders, 'verify_romfs', return_value={'path': str(image)}), \
                    patch.object(shaders, 'romfs_entries', return_value=[('/UI/LayoutArchive/synthetic.blarc.zs', offset, length)]):
                report = shaders.extract(image, root / 'work/extracted')
            self.assertEqual(report['unique_shaders'], 1)
            self.assertEqual(report['packages'][0]['origin'], '/UI/LayoutArchive/synthetic.blarc.zs:bgsh/test.bnsh')

    def test_effect_embedded_archives_are_checked_and_deduplicated(self):
        with tempfile.TemporaryDirectory(prefix='shader-effect-') as temp:
            root = Path(temp); (root / 'work/tmp').mkdir(parents=True)
            payload = b'YB\x05\x00' + bytes(60) + bnsh() + b'BNSH' + bytes(60) + bnsh()
            image = root / 'romfs.bin'
            image.write_bytes(romfs(compress(payload), b'Effect', b'synthetic.esetb.byml.zs'))
            output = root / 'work/extracted'
            with patch.object(shaders, 'ROOT', root), patch.object(shaders, 'verify_romfs', return_value={'path': str(image)}):
                report = shaders.extract(image, output)
            self.assertEqual(report['unique_shaders'], 1)
            self.assertEqual(report['unique_archives'], 1)
            self.assertEqual(report['extracted_bytes'], len(bnsh()))
            self.assertEqual(len(report['packages']), 2)
            self.assertEqual(len(list((output / 'source').iterdir())), 1)
            self.assertEqual({p['origin'].split('+')[0] for p in report['packages']}, {'/Effect/synthetic.esetb.byml.zs'})

    def test_embedded_archive_scan_rejects_truncated_and_false_markers(self):
        data = bnsh(); prefix = b'BNSH' + bytes(60)
        found = list(shaders.embedded_archives(prefix + data + b'trailer'))
        self.assertEqual(found, [(len(prefix), data)])
        self.assertEqual(list(shaders.embedded_archives(prefix + data[:-1])), [])

    def test_isolated_extract_roundtrip_and_refuse_overwrite(self):
        with tempfile.TemporaryDirectory(prefix='shader-extraction-') as temp:
            root = Path(temp); (root / 'work/tmp').mkdir(parents=True)
            image = root / 'romfs.bin'; image.write_bytes(romfs(compress(sarc('test.bnsh', bnsh()))))
            active = root / 'work/shaders'; active.mkdir()
            marker = active / 'pso_user.tsv'; marker.write_text('preserve active cache')
            before = hashlib.sha256(image.read_bytes()).hexdigest()
            output = root / 'work/extracted'
            with patch.object(shaders, 'ROOT', root), patch.object(shaders, 'verify_romfs', return_value={'path': str(image), 'sha256': before}):
                report = shaders.extract(image, output)
                self.assertEqual(report['unique_shaders'], 1)
                self.assertEqual(report['stages'], {'cs': 1})
                self.assertFalse(report['compiled'])
                records = json.loads((output / 'index.json').read_text())
                self.assertEqual(records[0]['local'], [1, 2, 3])
                self.assertEqual((output / 'code' / (records[0]['key'] + '.bin')).read_bytes(), bytes(16))
                with self.assertRaisesRegex(ValueError, 'exists'): shaders.extract(image, output)
                with self.assertRaisesRegex(ValueError, 'under work'): shaders.extract(image, root / 'outside')
                (root / 'work/link').symlink_to(root, target_is_directory=True)
                with self.assertRaisesRegex(ValueError, 'symlink'): shaders.extract(image, root / 'work/link/extracted')
            self.assertEqual(marker.read_text(), 'preserve active cache')
            self.assertEqual(hashlib.sha256(image.read_bytes()).hexdigest(), before)
            self.assertEqual(list((root / 'work/tmp').iterdir()), [])

    def test_archive_ranges_paths_and_decompression_limits(self):
        good = sarc('test.bnsh', bnsh())
        self.assertEqual(list(shaders.sarc_entries(good))[0], ('test.bnsh', bnsh()))
        for bad in (good[:-1], sarc('../escape', b'x'), sarc('/absolute', b'x'), sarc('a\\b', b'x')):
            with self.assertRaises(ValueError): list(shaders.sarc_entries(bad))
        encoded = compress(b'validated')
        self.assertEqual(shaders.decompress(encoded, {}), b'validated')
        for bad in (encoded[:-1], encoded + encoded):
            with self.assertRaises(ValueError): shaders.decompress(bad, {})
        with patch.object(shaders, 'MAX_FILE', 2), self.assertRaisesRegex(ValueError, 'excessive'):
            shaders.decompress(encoded, {})

    def test_failure_does_not_publish_partial_extraction(self):
        with tempfile.TemporaryDirectory(prefix='shader-invalid-') as temp:
            root = Path(temp); (root / 'work/tmp').mkdir(parents=True)
            image = root / 'romfs.bin'; image.write_bytes(romfs(compress(sarc('bad.bnsh', b'bad shader'))))
            with patch.object(shaders, 'ROOT', root), patch.object(shaders, 'verify_romfs', return_value={'path': str(image)}):
                with self.assertRaisesRegex(ValueError, 'unrecognized shader'):
                    shaders.extract(image, root / 'work/extracted')
            self.assertFalse((root / 'work/extracted').exists())
            self.assertEqual(list((root / 'work/tmp').iterdir()), [])

    def test_romfs_payload_and_directory_cycle_are_rejected(self):
        good = romfs(b'x'); bad_range = bytearray(good)
        struct.pack_into('<Q', bad_range, 80 + 56 + 16, 1 << 30)
        bad_cycle = bytearray(good); struct.pack_into('<I', bad_cycle, 80 + 24, 24)
        with tempfile.TemporaryFile() as stream:
            for data in (bytes(bad_range), bytes(bad_cycle)):
                stream.seek(0); stream.truncate(); stream.write(data); stream.flush(); stream.seek(0)
                with self.assertRaises(ValueError): list(shaders.romfs_entries(stream))
